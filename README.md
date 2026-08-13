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

HG_MALLOC addresses both. It reserves one arena up front, backed by **2 MB huge
pages**, pins it into RAM, and serves fixed size classes from per-thread caches
so the common allocation path takes no lock at all. Memory freed back to a size
class stays in that class; whole blocks are returned to an internal buddy
allocator once they drain, so the arena can shrink again rather than only ever
growing to its high-water mark.

It is worth using when you run large shared pools on many workers and care about
tail latency and page-table pressure. It is **not** worth using if you cannot
reserve huge pages, cannot raise the `memlock` limit, or need the pool to grow
while running.

## Compared with the other allocators

| | Q_MALLOC | F_MALLOC | HP_MALLOC | F_PARALLEL_MALLOC | **HG_MALLOC v2** |
|---|---|---|---|---|---|
| Design | safety-checked | minimal overhead | fine-grained locking | parallel buckets | hugepage slab + per-thread cache |
| Page backing | 4 KB | 4 KB | 4 KB | 4 KB | **2 MB huge pages** |
| Fast path | locked | locked | locked, sharded | locked, sharded | **lock-free** |
| Memory residency | grows on demand | grows on demand | grows on demand | grows on demand | **reserved and pinned at start** |
| Returns memory | within pool | within pool | within pool | within pool | to internal buddy; **arena can shrink** |
| Grows at runtime | yes | yes | yes | yes | **no — fixed at start** |
| Cross-class reuse | yes | yes | yes | yes | **no — classes are isolated** |
| Introspection | basic stats | basic stats | basic stats | basic stats | **30 statistics + 2 MI commands** |
| Corruption detection | runtime checks | — | — | — | counters + `LM_CRIT` |

### Pros

- **Fewer TLB misses.** A multi-GB pool on 2 MB pages needs ~512× fewer page-table
  entries than on 4 KB pages.
- **Lock-free common path.** Per-thread caches mean workers do not serialise on
  an allocator lock for ordinary allocations.
- **Predictable residency.** The arena is pre-faulted and `mlock`ed, so there is
  no page-fault cost during traffic and no swapping.
- **It gives memory back.** Drained blocks return to the buddy allocator, so the
  carve shrinks when load drops instead of staying at peak.
- **You can see inside it.** 30 exported statistics plus `hg_stats`, rather than
  a used/free pair.

### Cons — read these before deploying

- **The arena cannot grow at runtime.** It is sized and pinned once. Resizing
  means stop → adjust → start; a running node cannot be rescued by adding pages.
- **It reserves what you ask for, immediately.** A generous `-m`/`-M` is not free
  the way it is under Q_MALLOC — resident memory equals the configured size from
  the first second.
- **Classes do not share.** Memory freed in one size class is never handed to
  another. A workload that shifts its allocation profile can exhaust one class
  while the arena still has room elsewhere.
- **It needs host configuration.** Huge pages must be reserved and `memlock`
  raised, or the arena silently drops to a lower tier and you lose the benefit
  without an obvious error.
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

The arena reports which tier it obtained. **`MAP_HUGETLB 2M pages`** is what you
want; `plain 4K pages` means the pool was too small or `memlock` was still
capped — it still runs, which is why this is easy to miss.

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
| `tier` | must be `MAP_HUGETLB 2M pages`, or the point is lost |
| `blocks_carved` − `blocks_returned` | blocks in use; if returned tracks carved, reclaim is working |
| `corruption.total` | must stay `0`. Non-zero is a real defect, not tuning |

All 30 counters are also exported as `hg_shm_*` statistics for scraping:

```bash
opensips-cli -x mi statistics:get hg_shm_carved hg_shm_corruption
```

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
