# HG_MALLOC v2 — hugepage-backed slab allocator for OpenSIPS

> **This branch replaces the OpenSIPS memory allocator.** It is the `hg-malloc-v2`
> branch of a fork; the upstream OpenSIPS README follows below unchanged.
> Diff vs upstream master: **40 files**, allocator + CPU pinning + a stress module.
> Nothing else is touched.

## What it is for

OpenSIPS allocates from its own pools rather than libc: one shared pool (`-m`)
that every process reaches, and a private pool per process (`-M`). At scale two
things hurt — TLB pressure from 4 KB pages across a multi-gigabyte shared pool,
and contention on the allocator lock when dozens of workers allocate at once.

HG_MALLOC addresses both. It reserves one arena up front, pins it into RAM, and
serves fixed size classes from per-thread caches, so the common allocation path
takes no lock at all. The arena's page backing is chosen by a **four-rung ladder**
— reserved huge pages first, then THP, then THP retrofitted, then plain 4 KB —
each rung attempted and then *verified* through `/proc`; a rung that is
unavailable degrades to the next one and never fails startup. Huge pages are the
best case, not a prerequisite.

Inside the arena, memory is cut into **class-dedicated blocks**. A freed cell
returns to its own block, but a block whose cells have *all* been freed loses its
class entirely: it goes back to an internal buddy allocator, merges with its
buddy, and can be re-cut for any other size class. So a burst in one class does
not permanently reserve memory against the others — the in-use footprint falls
back instead of only ever growing to its high-water mark. What does *not* shrink
is the reservation itself: it stays mapped and pinned for the life of the
process, by design.

It is worth using when you run large shared pools on many workers and care about
tail latency and page-table pressure. A reserved huge-page pool gives the best
backing but is **not** required — see the tier ladder below; nor is `memlock`,
which if it cannot be raised costs you the pinning (the arena runs unpinned and
swappable, with a warning) rather than the allocator. What it genuinely cannot do
is resize while running: `-m`/`-M` are fixed for the life of the process, and the
whole reservation is paid for at startup rather than as it is touched.

## Compared with the other allocators

| | Q_MALLOC | F_MALLOC | HP_MALLOC | F_PARALLEL_MALLOC | **HG_MALLOC v2** |
|---|---|---|---|---|---|
| Design | safety-checked | minimal overhead | fine-grained locking | parallel buckets | hugepage slab + per-thread cache |
| Page backing | 4 KB | 4 KB | 4 KB | 4 KB | **2 MB huge pages** — hugetlb, else THP, else THP-collapse, else 4 KB |
| Host requirements | none | none | none | none | **none to run** — a hugetlb pool and raised `memlock` buy tier 1 and pinning, and their absence costs only those |
| If huge pages are unavailable | n/a | n/a | n/a | n/a | starts on the next rung down and says which; **slab, per-thread caches, lock-free path and reclaim all still apply** — only the guaranteed TLB win is given up |
| Fast path | locked | locked | locked, sharded | locked, sharded | **lock-free** |
| Pool size | fixed at `-m`/`-M` | fixed at `-m`/`-M` | fixed at `-m`/`-M` | fixed at `-m`/`-M` | fixed at `-m`/`-M` |
| Resident memory | grows as touched | grows as touched | grows as touched | grows as touched | **pre-faulted in full at start**, pinned if `memlock` allows |
| Cost of oversizing | low — untouched pages stay unbacked | low | low | low | **high — you pay for it immediately** |
| Returns memory | within pool | within pool | within pool | within pool | to internal buddy; **carve can shrink** |
| Cross-class reuse | yes | yes | yes | yes | via the buddy, once a block drains |
| Introspection | basic stats | basic stats | basic stats | basic stats | **30 statistics + 2 MI commands** |
| Corruption detection | runtime checks | — | — | — | counters + `LM_CRIT` |

### Pros

- **Fewer TLB misses.** A multi-GB pool on 2 MB pages needs ~512× fewer page-table
  entries than on 4 KB pages.
- **Lock-free common path.** Per-thread caches mean workers do not serialise on
  an allocator lock for ordinary allocations.
- **Predictable residency.** The arena is pre-faulted and `mlock`ed, so there is
  no page-fault cost during traffic and no swapping. (Tier 1 needs no `mlock` —
  hugetlb pages are unswappable by construction. On tiers 2–4 a capped `memlock`
  limit costs the pinning, not the arena: it is still pre-faulted, and says so.)
- **It gives memory back, across classes — and does it eagerly.** Reclaim is two
  mechanisms that are useless apart. *GC un-types*: a block whose cells are all
  free goes back to the buddy and stops belonging to a size class, so a block of
  free 96-byte cells — worthless to a class-768 requester — becomes available to
  anyone. *Defrag re-joins*: a freed block merges with its buddy, that with its
  buddy, up to a whole page, O(1) per step because a buddy's address is the
  block's own with one bit flipped. Run only GC and you get correctly-sized but
  fragmented free space; run only defrag and nothing ever becomes free enough to
  merge.

  Both are **event-driven rather than periodic**, and that is the part that
  matters operationally. A free just pushes onto a thread's LIFO; a block's live
  counter only moves at cache/block transitions — 1–3% of operations, already
  under the lock — so testing *"did this block hit zero, and can it merge"*
  there costs almost nothing and returns memory the moment it is genuinely free.
  A deferred or timer-driven sweep is slowest exactly when the reserve is most
  needed. Merging is delayed slightly on purpose: eager coalescing thrashes at a
  size boundary (free, merge to 16 KB, immediately need 8 KB, split back).

  Measured on a production gateway across a business day: carve **23.0 MB →
  18.6 MB**, with blocks returned (+1016) outpacing blocks carved (+848) — the
  arena shrinking under sustained load rather than holding its peak. Watch it
  with `gc_passes`, `blocks_carved` and `blocks_returned` in `hg_stats`.

  Two honest limits: merging is **buddy-only**, so two free neighbours that are
  not partners never merge; and the large tier (allocations above a page) is
  returned only when a chunk empties completely, so in practice it settles at
  its high-water mark.
- **You can see inside it.** 30 exported statistics plus `hg_stats`, rather than
  a used/free pair.
- **It runs anywhere, and tells you what it got.** Page backing is a four-rung
  ladder, and a miss degrades to the next rung — it never fails startup:

  | Tier | Mechanism | |
  |---|---|---|
  | 1 | `mmap(MAP_HUGETLB)` | huge pages from the reserved hugetlb pool |
  | 2 | `MADV_HUGEPAGE` | THP, huge at first fault |
  | 3 | `MADV_COLLAPSE` | THP, retrofitted after filling |
  | 4 | plain 4 KB pages | always works |

  A reserved pool is therefore an **optimisation, not a prerequisite**. Where you
  cannot reserve one — a container, a VM you do not control, a laptop — tiers 2
  and 3 still get huge pages through THP, and even on tier 4 you keep the
  lock-free fast path, the per-thread caches, the class-dedicated blocks and the
  reclaim. Only the guaranteed TLB win is given up. The tier reached is logged at
  startup and reported by `hg_stats`, so it is never a guess.

### Cons — read these before deploying

- **It reserves what you ask for, immediately.** This is the one that bites. *No*
  OpenSIPS allocator grows its pool at runtime — `-m`/`-M` are fixed at startup
  for all of them — but the others obtain the pool with a plain `mmap`, so pages
  only become resident as they are touched and an over-generous setting costs
  almost nothing. HG_MALLOC pre-faults and `mlock`s the whole arena, so resident
  memory equals the configured size from the first second. Oversizing is cheap
  under Q_MALLOC and expensive here.
- **Sizing mistakes are not recoverable in place.** Since nothing grows at
  runtime, correcting `-m`/`-M` means stop → adjust → start under any allocator;
  with HG_MALLOC the huge-page pool has to be adjusted too.
- **Cross-class reuse needs a block to drain fully.** A block is dedicated to one
  size class while in use, so memory freed inside it is reused by that class
  first. Only when the block empties completely does it return to the buddy and
  stop belonging to any class, after which it can be re-carved for another. A
  workload that shifts its profile therefore recovers, but not instantly — and a
  class whose blocks each retain one live cell holds them. (This is what v2
  changed: in v1 chunks were carved per class and never returned, so a burst
  pinned its peak for the life of the process.)
- **Getting the best backing needs host configuration.** A reserved huge-page
  pool and a raised `memlock` limit are what buy tier 1. Without them it still
  starts, on a lower rung — correct behaviour, but it does mean reading the
  startup line rather than assuming you got what you configured for.
- **Linux only.** It relies on `MAP_HUGETLB` and `mlock`.

## Enabling it

Compiled in alongside the other allocators and selected at runtime, so falling
back needs no rebuild.

```makefile
# Makefile.conf — already present in Makefile.conf.template
DEFS+= -DHG_MALLOC
```

```bash
opensips -a HG_MALLOC -m 256 -M 8 -f /etc/opensips/opensips.cfg
```

```sh
# or, under the packaged systemd unit, in /etc/default/opensips
MALLOC=HG_MALLOC
SHM_MEMORY=256
PKG_MEMORY=8
```

### Host prerequisites

```bash
# huge pages: (-m + -M x processes) / 2 on 2 MB pages, plus headroom
echo 'vm.nr_hugepages = 1536' > /etc/sysctl.d/60-opensips.conf
sysctl -p /etc/sysctl.d/60-opensips.conf
grep HugePages_ /proc/meminfo          # confirm the grant, it can come up short

# the arena is mlock()ed in full; the packaged unit ships a 64 KB ceiling
systemctl edit opensips                # [Service]  LimitMEMLOCK=infinity
```

The arena logs the tier it obtained at startup, and `hg_stats` reports it. Tier 1
(`MAP_HUGETLB 2M pages`) is the best case; a lower rung is a deliberate, working
fallback rather than an error. Read the line instead of assuming.

## CPU pinning

Ships with the allocator, because placing an arena only means something if the
workers stay where they were placed.

```
cpu_pinning     = 1        # master switch
pin_workers     = 1

pin_udp_cpus    = "0-7"    # per process type, all optional
pin_tcp_cpus    = "8-11"
pin_timer_cpus  = "12"
pin_module_cpus = "13-15"
```

A UDP listener can also carry its own `pin_cpus`, overriding the group:

```
socket=udp:10.0.0.1:5060 use_workers 8 pin_cpus "0-3"
```

## Introspection

Two MI commands:

```bash
opensips-cli -x mi core:hg_stats     # arena internals: carve, live, GC, corruption
opensips-cli -x mi core:hg_advise    # what -m and -M should be, from observed peaks
```

`hg_advise` is the one to reach for when sizing: it derives a recommendation from
the high-water marks the allocator actually saw, instead of guesswork.

Abridged `hg_stats` output:

```json
{
  "shm": {
    "tier": "MAP_HUGETLB 2M pages",
    "pinned_mb": 256,
    "carved": 19453952,
    "carved_peak": 24223744,
    "free_to_carve": 249012736,
    "live_committed": 10706944,
    "live_cells": 6180,
    "blocks_carved": 2440,
    "blocks_returned": 2104,
    "gc_passes": 2019,
    "corruption": { "total": 0, "double_free": 0, "nfree_underflow": 0 },
    "reserve_floor": 2048, "below_floor": 0, "floor_crossings": 0
  }
}
```

Three numbers worth watching:

| Field | Meaning |
|---|---|
| `tier` | which backing it obtained; tiers 1–3 are all huge pages |
| `blocks_carved` − `blocks_returned` | blocks in use; if returned tracks carved, reclaim is working |
| `corruption.total` | must stay `0`. Non-zero is a real defect, not tuning |

All 30 counters are also exported as `hg_shm_*` statistics for scraping:

```bash
opensips-cli -x mi statistics:get hg_shm_carved hg_shm_corruption
```

### Sizing with `hg_advise`

Because the whole reservation is paid for at startup, an oversized `-m` is not
free the way it is under the other allocators. `hg_advise` closes that loop: it
reads the high-water marks the allocator actually reached and says what `-m` and
`-M` should be. Run it after a busy period, apply, restart, re-read.

A first read on a server started with `-m 512 -M 32`:

```json
{
  "uptime_s": 141,
  "shm": {
    "configured_mb": 512,
    "peak_bytes": 6742016,
    "peak_pct_of_configured": 1,
    "margin_applied": 2,
    "recommended_mb": 14,
    "verdict": "oversized - the surplus is pinned and unusable elsewhere",
    "live_slab_bytes": 121504,
    "live_large_bytes": 3564464,
    "large_share_pct": 96,
    "caveat": "most of this arena's live bytes are in the large tier, and at least some large consumers size themselves as a fraction of the arena - so this recommendation is an upper bound, not a fixed point. Apply it once, restart, and re-read rather than iterating."
  },
  "pkg": {
    "configured_mb": 32,
    "peak_bytes": 4363536,
    "peak_pct_of_configured": 13,
    "margin_applied": 3,
    "recommended_mb": 14,
    "verdict": "oversized - the surplus is pinned and unusable elsewhere",
    "processes": 6,
    "pinned_total_mb": 192,
    "recommended_total_mb": 84
  },
  "notes": [
    "advice is based on the high-water reached SO FAR; a longer or heavier window can only raise it",
    "uptime is under 2 hours - too short to have seen a busy period, treat the numbers as provisional",
    "pkg peak is the worst single process, which is the one -M has to cover; every process gets its own arena of that size"
  ]
}
```

498 MB of the shm arena is pinned and doing nothing. Restarting the same server
with `-m 14` and re-reading gives:

```json
"shm": {
  "configured_mb": 14,
  "peak_bytes": 6742016,
  "peak_pct_of_configured": 45,
  "margin_applied": 2,
  "recommended_mb": 14,
  "verdict": "reasonable"
}
```

| Field | Meaning |
|---|---|
| `peak_bytes` | high-water of real used bytes, since start — the number everything else derives from |
| `margin_applied` | headroom multiplier: **×2** for shm, **×3** for pkg (a pkg arena is per process and cannot borrow) |
| `recommended_mb` | `peak × margin`, floored at 8 MB shm / 2 MB pkg, rounded up to a whole huge page |
| `verdict` | from peak as a share of configured: >70% `TIGHT`, >50% `watch`, <20% `oversized`, otherwise `reasonable` |
| `large_share_pct` | how much of the peak is the large tier. High means the advice is an upper bound — some large consumers size themselves *as a fraction of the arena*, so shrinking `-m` shrinks them too |
| `pinned_total_mb` / `recommended_total_mb` | pkg only: `-M × processes`, what you are actually paying versus what it would cost |

The `notes` array is where it tells you not to trust it yet — short uptime, no
busy period seen. Both examples above carry that note, which is exactly why the
advice is "apply once and re-read", not "iterate to a fixed point".

## Building

```bash
git clone -b hg-malloc-v2 https://github.com/Lt-Flash/opensips.git
cd opensips
cp Makefile.conf.template Makefile.conf     # -DHG_MALLOC already set
make -j"$(nproc)" && make install
opensips -V | grep HG_MALLOC_V2             # confirm it is compiled in
```

Verified: gcc and clang on x86-64, and cross-compiled for arm64 and arm32 —
zero errors on all four. On arm32 the allocator emits `-Wcast-align` warnings,
expected for code doing pointer arithmetic over carved memory where alignment
comes from the carve rather than the cast.

Deeper notes live in [`mem/README.hg_malloc`](mem/README.hg_malloc) and
[`mem/README.hg_arena_v2`](mem/README.hg_arena_v2).

---

[![Build Status](https://github.com/OpenSIPS/opensips/actions/workflows/main.yml/badge.svg?branch=master)](https://github.com/OpenSIPS/opensips/actions/workflows/main.yml?query=branch%3Amaster++)
[![Unit Tests](https://github.com/OpenSIPS/opensips/actions/workflows/unittests.yml/badge.svg?branch=master)](https://github.com/OpenSIPS/opensips/actions/workflows/unittests.yml?query=branch%3Amaster++)
[![OSS-Fuzz](https://github.com/OpenSIPS/opensips/actions/workflows/cifuzz.yml/badge.svg?branch=master)](https://github.com/OpenSIPS/opensips/actions/workflows/cifuzz.yml?query=branch%3Amaster++)
[![Cross Platform Builds](https://github.com/OpenSIPS/opensips/actions/workflows/multiarch.yml/badge.svg?branch=master)](https://github.com/OpenSIPS/opensips/actions/workflows/multiarch.yml?query=branch%3Amaster++)
[![RTP.io](https://github.com/OpenSIPS/opensips/actions/workflows/rtp.io.yml/badge.svg?branch=master)](https://github.com/OpenSIPS/opensips/actions/workflows/rtp.io.yml?query=branch%3Amaster++)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/7580/badge.svg)](https://scan.coverity.com/projects/opensips-opensips)

# Welcome to OpenSIPS Project


## About

OpenSIPS is a GPL licensed SIP server implementation. It started as a fork of
Fokus Fraunhofer SIP Express Router (SER) project. OpenSIPS wants to be a more
open project, not only from license point of view, but more open as project
management, especially for external contributions.

OpenSIPS wants to overcome the development latency of current SER project,
to ensure a shorter path into a release for new added features.
OpenSIPS is a project maintained by OpenSIPS Solutions
           <http://www.opensips-solutions.com/>
by a team including core and main developers of SER project.


## Info
For information regarding the OpenSIPS installation, please see the [INSTALL](INSTALL)
file.

For current developers/contributors of this project, see the [AUTHORS](AUTHORS) file.
For complete license information, please see the [COPYING](COPYING) file.
For an overview of OpenSIPS modules, a [modules listing](https://www.opensips.org/Documentation/Modules)
is available on the opensips.org website.


## Docs

Documentation about each module can be found in the [README]() file in each
module directory. For online documentation, please see
           <https://opensips.org/Resources/Documentation>

For additional documentation, tutorials and examples please see also
           <https://opensips.org/Resources/DocsTutorials>



## Questions

For any question related to the OpenSIPS usage, please use the
           <users@lists.opensips.org>
public mailing list.

For questions regarding the development of OpenSIPS - like contributions, bug
reports, etc - please use the
           <devel@lists.opensips.org>
public mailing list.

For questions regarding businesses around OpenSIPS - like products,·
consultancy, trainings, etc - please use the
           <business@lists.opensips.org>
public mailing list.

Also there is a generic news mailing list where you can learn about what is·
new or important for the OpenSIPS project, about alerts and updates regarding
relaces and about events around the project.
           <news@lists.opensips.org>
