> This is the `feature/hg-malloc-v3` branch of the private OpenSIPS
> mirror. The upstream project README is preserved as
> [`README.opensips.md`](README.opensips.md); the canonical copy of this
> guide lives in-tree at
> [`mem/README.hg_malloc_v3.md`](mem/README.hg_malloc_v3.md).

# HG_MALLOC v3 — the elastic arena

HG_MALLOC v3 lets the shared-memory and per-process arenas **grow and
shrink at runtime**, between a starting size and a cap you reserve up
front. Undersizing no longer fails allocations at 3 a.m., and
oversizing no longer pins gigabytes of RAM around a workload that needs
megabytes — the arena follows the load, within limits you set, with
every decision observable and alertable.

With no cap configured, **nothing changes**: the arena is fixed at
`-m`/`-M`, byte-for-byte the v2 behaviour.

```
                 hcap (the -m INIT:CAP reservation)
  ┌────────────────────────────────────────────────┐
  │ committed (usable, pinned)  │  reserved only   │
  └─────────────────────────────┴──────────────────┘
   ↑ starts at INIT      grows →      ← shrinks
     never below the shrink floor, never above the ceiling
```

Everything below was proven live on a rig before landing; the log lines
and MI output shown are real captures, not mockups.

---

## Quickstart

```bash
# 128 MB now, allowed to grow to 1 GB:
opensips -f opensips.cfg -m 128:1024 -M 16:64 -a HG_MALLOC
```

That alone gives you **exhaustion-triggered growth**: an allocation that
would have failed instead commits one 16 MB granule more and retries,
up to the cap. Loudly:

```
NOTICE:core:hg_malloc_init: shm HG_MALLOC_V3 arena: 128 MB on MAP_HUGETLB 2M pages, 128 MB pinned from swapping
NOTICE:core:hg_malloc_init: shm arena can grow to 1024 MB (896 MB headroom reserved, uncommitted)
...
NOTICE:core:hg_buddy_grow: shm arena grew by 16 MB to 144 MB (8 new pages on MAP_HUGETLB 2M pages; 880 MB headroom left)
```

Add a policy and it also grows **before** anything fails, and gives
quiet memory back:

```
auto_scaling_profile = MEM_SHM
    scale up to 1024 on 80% for 3 cycles within 10
    scale down to 256 on 30% for 120 cycles

shm_auto_scaling_profile = MEM_SHM
```

---

## Why the cap is on the command line

The shm arena is created **before the config file is parsed**, and on
the hugetlb tier the whole cap is reserved from the kernel's page pool
at `mmap()` time. There is no later moment at which a config value
could still shape the reservation — so the reservation lives where
sizing always has, on the command line, and the config supplies
**policy within it**. A profile can never raise the cap; it can only
choose how the space inside it is used.

`-m INIT[:CAP]` — shared memory. `-M INIT[:CAP]` — per-process private
memory. `CAP` is in MB, rounded up to whole huge pages, and must be
≥ `INIT`. Omitting `:CAP` pins the arena at `INIT` (v2 semantics).

The uncommitted remainder is nearly free on tiers 2–4 (page-table
entries only — a 64 MB untouched span was measured at 576 kB of RSS).
On **tier 1 it is earmarked hugetlb pool** — see Sizing below.

---

## The profile

v3 reuses the worker autoscaler's grammar — same tokens, same shape
your configs already use for `use_auto_scaling_profile`:

```
auto_scaling_profile = <NAME>
    scale up to <MB> on <pct>% for <C> cycles [within <W>]
    [scale down to <MB> on <pct>% for <C> cycles]

shm_auto_scaling_profile = <NAME>
pkg_auto_scaling_profile = <NAME>     # optional, may be a different profile
```

| element | meaning for an arena |
|---|---|
| `up to N` | growth **ceiling**, MB (within the `-m` cap) |
| `on P% for C within W` | grow when usage ≥ P% in C of the last W cycles |
| `down to M` | shrink **floor**, MB — may be *below* `-m` |
| `on Q% for C` | shrink one granule after C consecutive cycles at ≤ Q% |
| one cycle | one sweep interval (30 s) |
| implicit | after any grow, shrinking waits 10×C cycles (cool-off) |

Usage is carved-of-committed. "Down to" below `-m` is deliberate: with
a profile attached, **the profile is what you asked for** — `-m` is
just the starting size.

### Validation is fail-loud

A profile that cannot work stops startup with the reason. All real
messages:

```
ERROR: shm_auto_scaling_profile 'MEM_SHM' does not name an auto_scaling_profile
ERROR: shm profile 'MEM_SHM': the arena has no growth room - give the reservation on the command line (-m INIT:CAP)
ERROR: shm profile 'MEM_SHM': scale-up target 96 MB does not exceed the initial 128 MB - the profile could never act
ERROR: shm profile 'MEM_SHM': scale-up target 2048 MB exceeds the 1024 MB reservation - raise the :CAP
ERROR: shm profile 'MEM_SHM': scale-down target 2 MB is below the 4 MB minimum viable arena
```

A profile named while a different allocator is selected is ignored with
a WARN, not an error.

### Attach line

Every accepted profile announces itself once, so a config's effect is
verifiable from the log alone:

```
NOTICE:core:hg_autoscale_apply: shm auto-scaling profile 'MEM_SHM': 32..160 MB (start 64), up at 60% for 2/3 cycles, down at 20% for 1 cycles (cooldown 10)
```

### Supporting globals

```
hg_ram_floor_mb = 2048        # host-RAM guard; default max(256MB, MemTotal/20)
hg_autoscale_dry_run = 1      # advise-only mode, see below
```

---

## Worked examples

### 1. Billing gateway, hugetlb tier

Goal: start at today's proven 128 MB, allow 1 GB for storms, never
shrink below 128 (a gateway holds long-lived state).

```bash
# /etc/default/opensips
MALLOC=HG_MALLOC
SHM_MEMORY=128:1024
PKG_MEMORY=16:32
```

```
# opensips.cfg
auto_scaling_profile = MEM_SHM
    scale up to 1024 on 75% for 2 cycles within 5
    scale down to 128 on 25% for 120 cycles

shm_auto_scaling_profile = MEM_SHM
```

**Pool sizing — the rule that matters:** the pool must fit the **caps**,
not the initial sizes, plus a fork-window margin:

```
shm cap:            1024 MB            = 512 pages
pkg cap × workers:    32 MB × 31       = 496 pages
fork-window COW margin (~12%):         ≈ 120 pages
                                       -----------
vm.nr_hugepages                        = 1128
```

The margin is not optional: children briefly COW-touch the parent's
pkg arena before building their own, and each touched page draws a
**free** pool page. A pool with zero free pages at fork time SIGBUSes
children — measured, and it is the same crash family as a real 2026-08
production incident. If the pool cannot fit a cap, the arena falls
back to a cap-less hugetlb reservation: it keeps its huge pages,
cannot grow, and says so:

```
NOTICE: hugetlb pool cannot back a 1024 MB cap; reserving the 128 MB in use instead - the arena keeps huge pages but cannot grow. Raise vm.nr_hugepages to allow growth.
```

### 2. Load balancer, measured-first

An LB's real footprint is small (a production LB measured 11.9 MB peak
over 15 h). Start small, keep generous storm headroom, hand idle
memory back aggressively:

```bash
opensips -f lb.cfg -m 64:512 -M 16 -a HG_MALLOC
```

```
auto_scaling_profile = MEM_LB
    scale up to 512 on 70% for 2 cycles within 4
    scale down to 32 on 20% for 20 cycles

shm_auto_scaling_profile = MEM_LB
```

Note `down to 32` sits below `-m 64`: after a storm passes, the arena
returns even the initial allocation down to 32 MB.

### 3. First rollout — dry-run

Deploy with the policy you *think* is right, but let it only narrate:

```
hg_autoscale_dry_run = 1
```

The arena behaves exactly like a fixed v2 arena while every decision
the policy *would* have taken is logged:

```
NOTICE:core:hg_grow_tick: shm: DRY RUN - would grow (committed 64 MB, usage 97%, profile ceiling 160 MB)
WARNING:core:hg_buddy_grow: shm: DRY RUN - would grow for a 285488 byte request (committed 64 MB); counting further suppressed grows in hg_shm_grow_refused
```

Watch for a few days, tune the thresholds, then flip the flag off.

### 4. Cap only, no profile

```bash
opensips -f opensips.cfg -m 256:2048 -a HG_MALLOC
```

No proactive growth, no policy shrink — but exhaustion grows the arena
instead of failing allocations, and after four consecutive quiet sweep
intervals (2 minutes) with abundant free space, one granule at a time
is handed back, never below `-m 256`.

---

## What it does at runtime

**Growth** commits more of the already-mapped reservation: pre-faulted,
pinned, verified. Each delta re-negotiates its backing with the kernel
— a THP arena's delta may land on 4 K pages, and the accounting then
reports the split honestly rather than pretending one tier
(`tier_bytes` in `hg_stats`). Growth is **refused, never degraded**
when the host cannot back it: the populate step reports failure as a
clean errno (no worker ever SIGBUSes on half-committed memory), and a
host-RAM check keeps `MemAvailable` above the floor —  pkg deltas are
charged **× the process count**, because every worker grows its own
arena under the same workload:

```
WARNING:core:hg_grow_ram_refused: pkg: refusing to grow by 16 MB: 144 MB effective (x9 processes) would leave the host under the 14589 MB floor (MemAvailable 14562 MB). Freeing host memory or lowering the floor lifts this.
```

**Shrink** releases whole-free top pages via `madvise` (chosen and
verified by measurement — it frees the pages for *every* attached
process, even ones holding them mlocked, and the range recommits
cleanly). Only pages the buddy proves wholly free can ever be released
— a cell parked in any thread's private cache keeps its page
ineligible, so no coordination with other processes is needed. The
top-order free list is kept address-ordered so allocations concentrate
low and the top drains. On tier 1 the pages return to
`HugePages_Free`; elsewhere to general RAM.

```
NOTICE:core:hg_buddy_shrink: shm arena shrank by 16 MB to 144 MB (8 pages released to the hugetlb pool; 112 MB of growth still held)
```

**A full cycle, as captured on the rig** (grow to the ceiling, refuse
there, cool off, give it all back, regrow into the released range):

```
NOTICE: shm arena grew by 16 MB to 80 MB   (8 new pages on plain 4K pages; 176 MB headroom left)
...
NOTICE: shm arena grew by 16 MB to 160 MB  (8 new pages on plain 4K pages; 96 MB headroom left)
NOTICE: shm: at the 160 MB growth ceiling (the profile scale-up target), a 285488 byte request must fail - counting further refusals in hg_shm_grow_refused
...                                        [load released; 10-cycle cool-off]
NOTICE: shm arena shrank by 16 MB to 144 MB (8 pages released to the host; 112 MB of growth still held)
...
NOTICE: shm arena shrank by 16 MB to 32 MB  (8 pages released to the host; 0 MB of growth still held)
...                                        [load returns]
NOTICE: shm arena grew by 16 MB to 48 MB   (8 new pages on plain 4K pages; ...)
```

---

## Monitoring and alerting

### MI

```bash
opensips-cli -x mi core:hg_stats
```

```json
"shm": {
    "tier": "plain 4K pages",
    "committed": 167772160,
    "cap": 268435456,
    "grow_headroom": 100663296,
    "grows": 6,
    "grow_bytes": 100663296,
    "grow_refused": 1,
    "grow_blocked": 0,
    "shrinks": 8,
    "shrink_bytes": 134217728,
    ...
}
```

`tier_bytes` appears whenever more than one backing tier holds bytes —
that is the honest answer to "is my grown arena still on huge pages".

### Statistics (Prometheus-friendly)

```
hgmem:hg_shm_committed      bytes committed right now
hgmem:hg_shm_cap            the reservation
hgmem:hg_shm_grows          counter
hgmem:hg_shm_grow_bytes     counter
hgmem:hg_shm_grow_refused   counter  — rising = demand is hitting a wall
hgmem:hg_shm_grow_blocked   GAUGE    — the one to alert on
hgmem:hg_shm_shrinks        counter
hgmem:hg_shm_shrink_bytes   counter
```

### GROW-BLOCKED

A **resource** refusal (host RAM, mlock limit — not an admin ceiling)
that persists across a reclaim opportunity latches an alertable state:
the `hg_shm_grow_blocked` gauge goes to 1, one WARN is logged, and an
event is raised — and re-raised every 5 minutes while it holds:

```
event_route[E_CORE_SHM_GROW_BLOCKED] {
    xlog("L_WARN", "arena $param(arena) blocked: committed $param(committed_mb)MB / cap $param(cap_mb)MB, $param(grow_refused) refusals\n");
    # page someone, raise a ticket, ...
}
```

The hysteresis is deliberate and was verified in both directions: a
spike that one reclaim pass absorbs, or an isolated refusal per
interval, never alerts; a sustained stream does. It clears itself when
a grow succeeds or the demand goes away:

```
WARNING: shm: GROW-BLOCKED latched - the arena cannot grow and a GC pass did not change that (648586 refusals so far). Alert on hg_shm_grow_blocked; details precede this line.
NOTICE:  shm: GROW-BLOCKED cleared - demand fell back below the floor
```

An **admin ceiling** refusing is a NOTICE and never latches — a limit
doing its job is not an incident.

---

## Sizing rules that are not obvious

1. **Tier 1: the pool must fit the caps.** The whole `-m` cap is
   reserved from the hugetlb pool at map time, and every per-child pkg
   arena reserves its own `-M` cap the same way. Budget
   `shm_cap + pkg_cap × workers + ~12% margin` pages.
2. **Leave free pages for the fork window** (the COW margin above).
   Zero free pages at fork time = SIGBUS in children.
3. **pkg caps multiply.** `-M 16:64` on a 30-worker gateway is up to
   1.9 GB of potential pinned memory. The RAM-floor check accounts for
   it at grow time; your capacity plan should too.
4. **Tier-1 shrink returns pages to the pool, not to RAM.** Freeing
   host memory requires shrinking `vm.nr_hugepages` as well.
5. **When verifying pool behaviour, never read `HugePages_Free`
   alone.** Per-process *private* hugetlb (each worker's pkg arena and
   its copy-on-write touches of the inherited parent arena) moves the
   same counter. Compute
   `object_pages = (Total - Free) - Σ Private_Hugetlb(all pids)`
   from `/proc/meminfo` + `/proc/<pid>/smaps` — three test rigs
   misread a working shrink as broken before this was applied.

---

## Troubleshooting

| symptom | cause | fix |
|---|---|---|
| startup: `the arena has no growth room` | profile attached but no `:CAP` on `-m`/`-M` | add the cap: `-m 128:1024` |
| startup: `scale-up target ... exceeds the ... reservation` | profile wants more than the cap | raise `:CAP` or lower the target |
| `hugetlb pool cannot back a ... cap` at init | `vm.nr_hugepages` smaller than the caps | grow the pool (rule 1), restart |
| arena grew but new pages are 4K on a THP host | each delta negotiates independently | expected; see `tier_bytes`, consider tier 1 |
| never shrinks | top page busy, or inside the post-grow cool-off, or usage above the down-threshold | check `hg_stats` free/carved; the cool-off is 10× the down-cycles |
| `DRY RUN - would grow` but nothing happens | `hg_autoscale_dry_run = 1` | that is the point; set 0 to act |
| `grow_refused` climbing, gauge 0 | isolated refusals, hysteresis holding | by design; the gauge latches on sustained refusal |
| children SIGBUS at startup on tier 1 | pool sized to caps with zero margin | rule 2 |

---

## Limitations

* The **pre-fork parent** pkg arena predates the config and stays fixed
  at `-M`'s initial size; profiles govern every per-child arena. The
  attendant barely allocates, so this costs nothing in practice.
* Shrink is **top-only** (the address-space invariants depend on the
  arena staying one contiguous range) — prefer-low allocation exists to
  drain the top, but a single live cell in the top page blocks release
  until it moves or dies.
* Growth granule is 16 MB (huge-page rounded); commits happen under the
  arena lock — deliberate, bounded, and rare.
* Linux-only elasticity; elsewhere the arena is plain fixed memory.
* Tested back to kernel 5.4 (the oldest in this fleet), on 4 K, THP and
  hugetlb backings, x86_64.
