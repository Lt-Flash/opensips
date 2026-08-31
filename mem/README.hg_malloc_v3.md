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

## Status (2026-08-23)

Complete and validated: `feature/hg-malloc-v3-master`. Elastic shm and
pkg arenas with two-phase commit, live-usage gating, a maintenance
process that grows ahead of demand, whole-page large chunks, lock
histograms and a stall event, and module arenas (section 11b). On the
three-node 1,000,000-contact rig it passes every harness bar on both
host configurations: 2,272 MB committed where a fixed arena needs 3,072,
zero lost requests, cold-pull p99 46% / 58% below F_MALLOC at 5% less
memory. Figures and per-configuration numbers: the README of
`feature/usrloc-pull-sharing-devel`. Price: ~7% more memory per cell
than a hand-packed chunk allocator when HG manages a module's cells.

## 0. Why an elastic arena

Every OpenSIPS deployment ships with two numbers somebody guessed:
`-m`, the shared-memory arena all calls, dialogs and caches live in,
and `-M`, the private arena **each worker process** gets. Both are
fixed at boot. Both are a bet.

**Bet low and you lose at the worst possible moment.** Shared memory
exhausts in the middle of your best traffic hour: allocations fail,
calls drop, and the only fix is a restart with a bigger number —
downtime, during the incident, on every node it touches.

**Bet high and you pay for it every quiet hour.** The arena is pinned
physical memory; on the hugetlb tier it is carved out of the host at
boot and nothing else can ever use it. A production load balancer we
measured peaks at **11.9 MB** of shm over 15 hours of full traffic —
inside a 128 MB fixed arena. And `-M` multiplies: 16 MB across one
gateway's 54 workers is **864 MB** of committed RAM backing arenas that
mostly hold under 3 MB each, because with a fixed `-M` you size *every*
worker for the *busiest* worker's worst minute.

And the bet cannot be won, because the right number is a moving
target — it shifts with traffic mix, dialog lifetimes, the module set,
the season:

```mermaid
xychart-beta
    title "One day, one node: fixed arena vs what the traffic needs (MB)"
    x-axis ["00h","03h","06h","09h","12h","15h","18h","21h","24h"]
    y-axis "MB" 0 --> 560
    line [512,512,512,512,512,512,512,512,512]
    line [64,32,32,96,224,336,368,160,64]
    line [41,17,15,83,201,318,344,138,49]
```

*Top, flat: a fixed `-m 512` sized for the storm, paid around the
clock. Middle, stepped: what v3 keeps committed — granule steps up
under load, shrink lagging the peak by the quiet window. Bottom: live
demand. (Illustrative shapes; a measured cycle is charted in
Section 9.)*

v3 replaces both bets with a **range**: `-m 64:512 -M 16:32`. Start at
the size you can defend, reserve the ceiling (address space — free),
commit physical pages only as demand arrives, hand back what goes
quiet. Growth stops at three independent limits — your policy, the
backing tier, the host's real free RAM (pkg charged × the worker
count). Every decision is a counter you can scrape, the one state that
deserves a page is a latched event, and a dry-run mode narrates what it
*would* have done before you let it do anything.

Every mechanism below was chosen **by measurement first** — including
two designs that looked right on paper and were proven memory-corrupting
before a line of allocator code was written. The log lines, MI output
and numbers in this document are real captures from the proof rigs, not
mockups.

**Contents**

0. [Why an elastic arena](#0-why-an-elastic-arena)
1. [Build](#1-build)
2. [Quickstart](#2-quickstart)
3. [Concepts and architecture](#3-concepts-and-architecture)
4. [Why the cap is on the command line](#4-why-the-cap-is-on-the-command-line)
5. [The memory tiers](#5-the-memory-tiers)
6. [Growth](#6-growth)
7. [The three-limit ceiling](#7-the-three-limit-ceiling)
8. [GROW-BLOCKED — the alertable state](#8-grow-blocked--the-alertable-state)
9. [Shrink](#9-shrink)
10. [The profile — configuration reference](#10-the-profile--configuration-reference)
11. [Dry-run mode](#11-dry-run-mode)
12. [pkg arenas — what is different](#12-pkg-arenas--what-is-different)
13. [Deployment cookbook](#13-deployment-cookbook)
14. [Monitoring and alerting](#14-monitoring-and-alerting)
15. [Log line reference](#15-log-line-reference)
16. [Sizing rules that are not obvious](#16-sizing-rules-that-are-not-obvious)
17. [Troubleshooting](#17-troubleshooting)
18. [Testing — the rig and how to reproduce the proofs](#18-testing--the-rig-and-how-to-reproduce-the-proofs)
19. [Appendix A — measured kernel facts](#19-appendix-a--measured-kernel-facts)
20. [Appendix B — internals map for developers](#20-appendix-b--internals-map-for-developers)
21. [Limitations](#21-limitations)

---

## 1. Build

HG_MALLOC lives in core (`mem/`), not a loadable module — there is no
`include_modules` entry for it and no separate module build step. Any
standard OpenSIPS build already contains it:

```bash
make proper
make -j$(nproc) install PREFIX=/opt/opensips-4.1.0-<rev>
```

**Leave `-DDBG_MALLOC` in `Makefile.conf`'s `DEFS`** (it ships on by
default in the template). It does not turn on debug tracking for the
allocator you run — it compiles in the *debug-instrumented flavours*
(`HG_MALLOC_DBG`, alongside `F_MALLOC_DBG` etc.) as additional runtime
choices, selected the same way as the plain ones. Stripping it to "clean
up" a build does not skip anything HG_MALLOC-specific; the tree fails to
build without it, from an unrelated include-chain dependency several
other core files have on it. Every build in this repo's CI carries it.

**Build and runtime allocator selection are two different steps.**
`opensips -V` lists every allocator *compiled in* — it says nothing about
which one is running. Selection happens at process start, via `-a
HG_MALLOC` (or `-a HG_MALLOC_DBG` for the instrumented flavour), or
`MALLOC=HG_MALLOC` in whatever env file your unit's `ExecStart` reads
`-a` from. Confirm what actually started with the log line, not the
build:

```
NOTICE:core:main: using 128 MB of shared memory, allocator: HG_MALLOC_V3
```

or `opensips-cli -x mi core:hg_stats`, which errors cleanly
(`"HG_MALLOC is not the active allocator"`) if something else is running
— a fixed-size hugetlb pool sitting at `Free == Total` after a restart is
the same tell from the outside, if you'd rather check without MI.

**Host prerequisites are all optional and the arena degrades gracefully
without them** — this is the tier ladder in Section 5, not a build
requirement. If you want the hugetlb tier specifically:

- Reserve `vm.nr_hugepages` sized to the **cap**, not the init size — a
  grow event needs the whole reservation available in the pool the
  moment it fires, not just the starting commitment.
- Set `LimitMEMLOCK` (systemd) or `ulimit -l` to at least the cap too,
  and remember pkg is **per worker**: the real number is the pkg cap
  times worker count, added to the shm cap.

Skip both and HG_MALLOC starts on THP, then `MADV_COLLAPSE`, then plain
4 KB pages automatically — see Section 5 for what each tier costs.

**Cross-compiling** needs nothing allocator-specific: HG_MALLOC is
written against standard POSIX `mmap`/`mlock`/`madvise`, no
architecture-specific code path. Verified via cross + qemu for
arm32/arm64/i386/mips64, and full native-userland container builds for
arm32/arm64 (Section 18).

Next: Section 2 for the smallest working `-m`/`-M`/`-a` invocation.

---

## 2. Quickstart

```bash
# 128 MB now, allowed to grow to 1 GB:
opensips -f opensips.cfg -m 128:1024 -M 16:64 -a HG_MALLOC

# same grammar takes k/m/g suffixes on either half - bare numbers are MB:
opensips -f opensips.cfg -m 16:128 -M 512k:1m -a HG_MALLOC
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

auto_scaling_profile = MEM_PKG
    scale up to 64 on 80% for 2 cycles within 5
    scale down to 8 on 20% for 60 cycles

shm_auto_scaling_profile = MEM_SHM
pkg_auto_scaling_profile = MEM_PKG
```

The pkg profile governs **every worker's private arena individually**,
so its numbers are per-worker scale — an order of magnitude below the
shared arena's (Section 12).

Watch it work:

```bash
opensips-cli -x mi core:hg_stats           # committed / cap / grows / shrinks / tiers
```

---

## 3. Concepts and architecture

### 3.1 Committed vs reserved

The arena block (`struct hg_block`) carries two sizes:

| field | meaning |
|---|---|
| `hsize` | **committed** — pre-faulted, pinned, published to the buddy allocator, usable |
| `hcap`  | **reserved** — the size of the one mapping created at startup |

`hcap == hsize` (no `:CAP` given) is a fixed arena — exactly v2.

### 3.2 The one invariant everything rests on

**The whole cap is mapped once, `MAP_SHARED`, before fork.** Growth and
shrink never create, destroy, or re-protect mappings — they only change
which parts of that one shmem object are populated.

This is forced, not stylistic. `mmap()` and `mprotect()` edit **one
process's page tables**, and the shm arena is shared by ~30 workers
that forked before any growth happens. The "obvious" design — keep the
tail `PROT_NONE` and `MAP_FIXED` deltas in on demand — was implemented
as a userspace rig first and **measured to be memory-unsafe in both
directions** on the fleet's oldest kernel (5.4):

* **Growth** via post-fork `MAP_FIXED`: the grower reads its new pages
  fine; a worker that forked earlier still has `PROT_NONE` there and
  **SIGSEGVs** on the first cell handed out of grown space. (Control
  arm: the pre-fork committed prefix was visible in the same worker.)
* **Shrink** via `mmap(PROT_NONE|MAP_FIXED)`: it rebinds only the
  caller's mapping. Measured: after the shrinker released and re-grew a
  range and wrote `0x77`, another worker **still read the old `0xEE`**
  — two processes silently disagreeing about one arena address.

With the whole-cap mapping, growth is a *commit* (populate + pin) and
shrink is a *punch* (`madvise`) on the shared object — both visible to
every process by construction. The untouched reserved tail costs only
page-table entries: a 64 MB mapped-untouched span was measured at
**576 kB of RSS**.

### 3.3 The buddy grid grows without moving metadata

The buddy allocator's per-page descriptors (`struct hg_page`,
leaf-order arrays, bitmaps) are laid out at init **for the full cap**
(`npages_cap`), not just the committed pages. The overhead is ~0.05% of
each never-committed page, paid up front — and it means a grow only
*publishes* pages that already have descriptors. The alternative would
be finding room for metadata in an arena that is, by definition of why
it is growing, full.

Each page descriptor also carries the **achieved backing tier of the
commit that brought it in** (one byte, fits existing padding) — that is
what keeps the per-tier accounting truthful in both directions, since
shrink releases top pages that may come from any delta.

### 3.4 The ownership registry records the cap

`hg_arena_reg[]` (the cross-arena pointer-ownership table used on free
paths) records `hcap`, not `hsize`. Growth must never invalidate a
registry entry, or a pointer into freshly grown space would be misread
as foreign and "routed" to another arena. The whole cap's address range
belongs to this arena from reserve time; uncommitted ranges cannot hold
live cells, so the wider range cannot misattribute anything that
exists.

---

## 4. Why the cap is on the command line

The shm arena is created **before the config file is parsed**
(`init_shm_mallocs()` runs before `parse_opensips_cfg()` in `main()`),
and on the hugetlb tier the whole cap is reserved from the kernel's
page pool **at `mmap()` time** (measured — see Appendix A). There is no
later moment at which a config value could still shape the reservation.

So the reservation lives where sizing always has:

```
-m INIT[:CAP]        shared memory, MB
-M INIT[:CAP]        per-process private memory, MB
```

`CAP` is rounded up to whole huge pages and must be ≥ `INIT` (refused
otherwise, at option parsing). Omitting `:CAP` pins the arena at
`INIT`.

The config then supplies **policy within the reservation** — resolved
and validated in `init_shm_post_yyparse()`, the same post-parse hook
HP_MALLOC uses for memory warming. A profile can never raise the cap;
it can only choose how the space inside it is used.

---

## 5. The memory tiers

The arena tries four backings in order, each **attempted and then
verified through `/proc`** — never inferred from kernel version or
sysfs configuration:

| tier | mechanism | verification | properties |
|---|---|---|---|
| 1 | `mmap(MAP_HUGETLB)` | mmap success | unswappable by construction; pool-accounted |
| 2 | `MADV_HUGEPAGE` before first touch | `/proc/self/smaps` PMD-mapped | THP at fault |
| 3 | `MADV_COLLAPSE` after fill | `ShmemHugePages` delta | THP retrofitted |
| 4 | plain 4 K | — | always works; mlock-pinned |

Two v3-specific rules:

* **Every growth delta re-negotiates its own backing.** A THP arena's
  delta may land on 4 K next to memory that got 2 M at init. Backing is
  an *outcome per range*, never an attribute of the arena — which is
  why `hg_stats` reports a `tier_bytes` split whenever more than one
  tier holds bytes, instead of one label that would be a lie.
* **Tier 1 reserves the whole cap from the pool at map time**
  (measured: mapping 64 MB moved `HugePages_Rsvd` by exactly 32 pages
  before any fault). Consequences: tier-1 growth can never fail
  mid-flight — the pages are earmarked — and the pool must be sized for
  the **caps** (Section 16). If the pool cannot fit the cap, the arena
  falls back to a cap-less hugetlb reservation: it keeps huge pages,
  cannot grow, and says so.

Measured cost context (same workload, `perf` self-time across allocator
symbols): tier 1 ≈ 2.78%, tier 2 ≈ 2.97%, tier 3 ≈ 3.07%, tier 4 ≈
3.38% — against F_MALLOC 7.56%. Huge pages are the smaller half of the
win; losing a delta to 4 K is a percent, not a disaster.

---

## 6. Growth

Two triggers, one mechanism.

**Exhaustion (always armed once a cap exists).** The three points where
an allocation can die of buddy exhaustion — small-object chunk carving,
the large tier, region allocation — each grow-and-retry exactly once.
If the arena grew, the freshly published whole pages satisfy the retry
by construction; a second miss can only mean the grow itself was
refused, and the caller's ordinary exhaustion error follows.

**The commit is two-phase.** A grow reserves its granule under the
arena lock (`committed_pending` moves, `grow_inflight` is set), then
releases the lock for the expensive part — the page faults, the pin and
the backing verification, tens of milliseconds for 16 MB — and takes it
again only to publish the new whole pages (O(1)). Every other slow path
keeps running during the populate. One grow is in flight at a time; a
worker that exhausts while it runs waits for the publish *without* the
lock (100 µs slices, 2 s cap, then the request is refused and counted
in `grow_wait_timeouts`) and retries its carve against the fresh pages.
Shrink never moves the top while a grow is in flight.

Measured on the 1M-record bench (3 nodes, 16 MB granules, 115 grows per
million records absorbed, host on `shmem_enabled=advise`, Section 13.5):
before the split a grow held the lock for the whole populate — 20 ms
mean, 34 ms max, every other slow path queued behind it; after it the
worst hold in the arena is 0.9 ms, the exhausted workers wait 10 ms on
average without the lock, and the elastic arena's request latencies
equal the fixed arena's (cold-pull p99 15 vs 17 ms, fill p99 0.83 vs
0.89, zero loss) while committing 2,352 MB instead of 3,072 (2,272 once
the large tier packs whole pages, 13.7). On a host
left at `shmem_enabled=never` the lock holds are gone as well, but the
waiters still pay the requester's populate plus collapse (39 ms mean).

**The maintenance process.** An elastic shm arena (a cap above the
initial size) gets a dedicated core process, `HG maintenance`, forked
after the timer processes. It serves no request; every second it takes
the arena lock for microseconds to run the headroom rule below, and
every thirtieth tick — the profile's cycle — the profile gate and the
shrink gate; the populate of any granule it decides to grow runs in
*this* process, with the lock released. It exists because timer jobs are
executed by whichever process reads the timer-job pipe, the SIP workers
included, so a "proactive" grow from a timer job still parked a worker
for the populate (measured: a 2 s grow tick made the tails worse). While
it is alive the sweep's own ticks stand down (`hg_stats` →
`maintenance_process`); exhaustion growth stays armed in every process
as the backstop it always was. A fixed arena (no cap) forks no process.

**Headroom, always on (`hg_grow_ahead`, default 1).** Every second in
the maintenance process (once per sweep interval without one), whenever
the reservation still has room and the free buddy grid has dropped below
twice the reserve floor, one granule is grown ahead of demand. This is the rule the warm-path tail asked for — the
arena sitting at its floor, every sweep flushing caches to stay above it
— and with the two-phase commit it costs the data path nothing.

**Proactive (with a profile).** Once per sweep interval (30 s), usage
(live — what is handed out, not the carved footprint with its recycled
caches, which the gate used until it drove a 35 % profile straight to the
ceiling) is compared to the profile's up-threshold using a
cycles-in-window count. Crossing it grows one granule **before any
allocation fails**. The exhaustion path stays armed for bursts between
ticks.

The commit itself (`hg_mem_commit()`):

1. optional host-RAM check (Section 7) — before any work;
2. `mlock()` of the delta — chosen because it populates *exactly* that
   range, pins it, and reports failure through `errno` instead of
   letting a worker SIGBUS later on half-committed memory. Growth is
   **refused, never degraded**;
3. per-delta tier verification (Section 5), `tier_bytes` accounting;
4. only then: `hsize` moves, pages are published, the reserve floor is
   recomputed, counters tick.

The pre-fault runs under the arena lock — a deliberate, bounded trade.
Growth is once per granule of genuine demand; every other worker in the
slow path at that moment is *also* out of memory and would only queue
on the same growth.

Granule: 16 MB (huge-page rounded).

---

## 7. The three-limit ceiling

`min(admin, tier, host RAM)` — each limb enforced where it is real:

| limb | what | where enforced |
|---|---|---|
| admin | the profile's scale-up target, else the `-m` cap | `hg_buddy_grow()` — refusing it is a NOTICE, never an alert: a limit doing its job is not an incident |
| tier | hugetlb pool | **at reserve time** (map-time pool reservation — measured); tiers 2–4 additionally by `mlock`'s own errno |
| host RAM | `MemAvailable` vs a floor | `hg_grow_ram_refused()` before each commit |

The RAM floor defaults to `max(256 MB, MemTotal/20)`, configurable:

```
hg_ram_floor_mb = 2048
```

Tier 1 **skips** the RAM limb outright: its pages were carved out of
host RAM when the pool was created — charging them again would
double-count (also measured, not assumed).

**pkg deltas are charged × the process count**: every worker grows its
own private arena under the same workload, so the single-arena delta
understates the real host cost ~30× on a gateway. Verified
differentially — with one floor, a 16 MB pkg delta was refused while
16 MB shm deltas grew:

```
WARNING:core:hg_grow_ram_refused: pkg: refusing to grow by 16 MB: 144 MB effective (x9 processes) would leave the host under the 14589 MB floor (MemAvailable 14562 MB). Freeing host memory or lowering the floor lifts this.
```

The whole grow path, trigger to publish:

```mermaid
flowchart TD
    A["allocation would fail
    (chunk carve / large tier / region)"] -- "grow-and-retry" --> G
    P["proactive tick:
    profile up-window met"] --> G
    G["grow request, whole 16 MB granules"] --> L1{"admin ceiling:
    profile up-target, else the cap"}
    L1 -- "at ceiling: NOTICE, refused++" --> R["refusal counters
    (hg_shm_grow_refused / pkg twin)"]
    L1 -- ok --> L2{"tier: does the backing
    (hugetlb pool) cover the delta?"}
    L2 -- no --> R
    L2 -- ok --> L3{"host RAM floor:
    MemAvailable - delta >= floor
    (pkg: delta x worker count)"}
    L3 -- under --> R
    L3 -- ok --> C["mlock() the new granules:
    commit, verify tier per delta"]
    C --> W["publish pages to the buddy grid
    grows++, headroom NOTICE, cooldown armed"]
    R -- "resource refusals only" --> B["GROW-BLOCKED machinery
    (Section 8)"]
```

---

## 8. GROW-BLOCKED — the alertable state

A **resource** refusal (host RAM, mlock limit — not an admin ceiling)
should page someone *only if it means something*. The state machine:

```mermaid
stateDiagram-v2
    [*] --> idle
    idle --> armed: resource refusal
    armed --> idle: quiet for a full sweep interval
    armed --> LATCHED: refusal recurs after a GC pass
    armed --> LATCHED: full sweep interval, refusals still accruing
    LATCHED --> idle: a grow succeeds
    LATCHED --> idle: demand falls below the floor recovery mark
    note right of LATCHED
        gauge hg_shm_grow_blocked = 1
        one WARN + E_CORE_SHM_GROW_BLOCKED
        re-raised every 5 min while held
    end note
```

Two latch routes exist because of a measurement: the original
"survived a GC pass" rule alone sat through **5 million refusals with
`gc_passes == 0`** — a full arena where nothing is reclaimable is
exactly the state that most needs the alert, and it never runs GC. The
sweep timer is therefore a second promoter: armed + still refusing a
full interval later ⇒ latch.

The hysteresis was verified in both directions: isolated
one-refusal-per-interval spikes never latch (each arming is disarmed by
the next quiet tick); a sustained stream latches.

Surfaces: the `hg_shm_grow_blocked` **gauge** (the alertable one), one
WARN log line, and the `E_CORE_SHM_GROW_BLOCKED` event — raised **from
the sweep timer**, never under the arena lock: `evi_raise_event()`
allocates shm, and raising it inside the allocator that just refused to
grow would re-enter a full arena.

```
event_route[E_CORE_SHM_GROW_BLOCKED] {
    xlog("L_WARN", "arena $param(arena) blocked: committed $param(committed_mb)MB / cap $param(cap_mb)MB, $param(grow_refused) refusals\n");
}
```

The event is published with the other core events unconditionally, so
`event_route` can always subscribe; only the *raise* is conditional.
Shm arena only, honestly so: each pkg arena's state is private to its
process (its WARN appears in that worker's log, its counters in its own
`hg_stats` pkg section).

Log flood protection is separate from the latch and applies everywhere:
refusal details are logged **once per episode** (re-armed by the next
successful grow). The first at-cap soak without this printed 239,458
identical NOTICEs in four seconds; the counter carries the magnitude,
the log carries the fact.

---

## 9. Shrink

### 9.1 Why it is safe with zero cross-process coordination

Only pages the buddy proves **wholly free** can be released, and only
from the **top** of the committed range:

* *Whole-free is a proof, not a heuristic.* Buddy merging is eager, so
  an all-leaves-free page has provably merged into one top-order block.
  And a cell parked in any thread's private `__thread` cache has **not**
  decremented its block's live count — that happens only at cache/block
  transitions — so its block is still carved and its page can never
  appear whole-free. Contrapositive: whole-free ⇒ no live pointer into
  the page exists anywhere ⇒ nothing to coordinate.
* *Top-only* keeps `hg_owns()` one contiguous range test and the
  ownership registry valid — the address-space invariants of the whole
  allocator.

### 9.2 The primitive — measured, with the wrong answers named

`munlock()` + `madvise(MADV_REMOVE)` for the shared arena: it punches
the **shmem object**, so every mapped process is affected. Measured on
kernel 5.4:

* frees the pages even while another process holds them `VM_LOCKED`
  (worker RSS dropped by exactly the punched size; re-read returned
  zeroes);
* the range recommits cleanly afterwards (mlock + write, visible
  cross-process);
* on hugetlb, the pages **return to `HugePages_Free`** and are drawn
  back out on re-fault — the pool round-trip.

pkg arenas (`MAP_PRIVATE`) use `MADV_DONTNEED` — per-process memory,
no cross-process question exists.

The one primitive that must never be used is
`mmap(PROT_NONE|MAP_FIXED)` over the range — see 3.2; it was measured
leaving other workers reading stale bytes.

A kernel that refuses the advice latches `shrink_unsupported` once and
the arena simply stays grown — nothing retries, nothing spams.

### 9.3 Ordering and policy

The punch runs **before** any bookkeeping, under the arena lock, so a
refused release changes nothing and no worker can carve from pages
mid-punch. On success: pages leave the free lists and the grid,
`hsize`/`npages`/floor/counters adjust, per-page tiers decrement
`tier_bytes`.

Cadence is down-slow by design: one granule per quiet window
(no-profile default: 4 consecutive quiet sweep intervals = 2 minutes;
with a profile: its own threshold and cycle count). Hard safety
conditions hold regardless of policy — never below the floor, never
while grow-blocked, top page must already be whole-free, and free space
must stay clear of the reserve floor's recovery threshold even after
giving the granule back, so a shrink can never re-trigger the pressure
that would regrow it. Any grow resets the window and starts the
profile's post-grow cool-off (10× the down-cycles).

**Prefer-low allocation** makes tops drain: the top-order free list is
kept in ascending address order, so whole-page carves always take the
lowest free page; multi-page runs already scanned ascending. Lower
orders stay LIFO — their placement belongs to the cell-level
concentration policy, and their lists are the long ones where an
ordered walk would cost.

Floor: never below `-m`/`-M`'s initial size — unless a profile says
otherwise (Section 10): with a profile attached, the profile is the ask
and `-m` is just the starting size.

A full cycle, measured on a live process (rig arm K, Section 18): grow
under MI-driven load, cooldown, the shrink train once the quiet window
passes, regrowth on the next pulse — committed walking 32→96→16→80 MB
while the reservation never moves and the hugetlb pool gets every
released page back:

```mermaid
xychart-beta
    title "Measured: committed MB through one load cycle (reservation fixed)"
    x-axis ["idle", "hold", "cooldown", "quiet + shrink train", "second hold"]
    y-axis "committed MB" 0 --> 110
    bar [32, 96, 96, 16, 80]
    line [32, 96, 96, 16, 80]
```

---

## 10. The profile — configuration reference

v3 reuses the worker autoscaler's grammar — same tokens, same shape
your configs already use for `use_auto_scaling_profile`:

```
auto_scaling_profile = <NAME>
    scale up to <MB> on <pct>% for <C> cycles [within <W>]
    [scale down to <MB> on <pct>% for <C> cycles]

shm_auto_scaling_profile = <NAME>
pkg_auto_scaling_profile = <NAME>       # optional, may be a different profile
hg_ram_floor_mb          = <MB>         # 0 = auto (max(256MB, MemTotal/20))
hg_autoscale_dry_run     = 0|1
hg_lock_stall_us         = N        # arena-lock hold counted as a stall; 0 disables; default 1000
hg_grow_ahead            = 0|1      # keep the free grid above 2x the reserve floor by growing a granule ahead (default 1)
```

| element | meaning for an arena |
|---|---|
| `up to N` | growth **ceiling**, MB, huge-page rounded — the admin limb, within the `-m` cap |
| `on P% for C within W` | grow when usage ≥ P% in C of the last W cycles (`within W` omitted ⇒ W = C) |
| `down to M` | shrink **floor**, MB — may be *below* `-m` |
| `on Q% for C` | shrink one granule after C consecutive cycles at ≤ Q% |
| one cycle | one sweep interval (30 s); with the maintenance process, thirty of its one-second ticks |
| implicit | post-grow cool-off of 10×C cycles before shrink counting resumes |

Usage is **live** — what is handed out, `live_committed` in `hg_stats`
— not the carved footprint with its recycled caches (the gate used that
figure until a 35 % profile drove a 893 MB working set straight to its
3,072 MB ceiling). Profile numbers are copied **into** the (possibly
shared) arena block at attach — never pointed to; the profile structs
live in process-local memory.

### Choosing the numbers

Read `P` as the occupancy you want the arena to run at: the profile
grows one granule per cycle for as long as live usage is above `P` % of
committed, so `on 35%` means "keep two thirds of the committed arena
free" — measured on the 1M-record bench that is 1,840 MB committed for a
238 MB working set on the owners and the ceiling on the puller, nothing
wrong with the gate, just what 35 asks for. For a production node `P`
of 70–80 % is the usual shape (the headroom rule of Section 6 already
keeps the free grid above twice the reserve floor underneath it), and
`Q` well below `P` — 20–40 % — so the band between them is wide enough
that a busy hour does not oscillate; `C` for the shrink in cycles of
30 s (20 cycles = 10 minutes quiet), and remember the implicit cool-off
of 10×C after any grow before shrinking even starts counting. The
examples in the cookbook (13.1, 13.2, 13.4) are of that shape.

### Validation — fail-loud, all real messages

A profile that cannot work stops startup with the reason:

```
ERROR: shm_auto_scaling_profile 'MEM_SHM' does not name an auto_scaling_profile
ERROR: shm profile 'MEM_SHM': the arena has no growth room - give the reservation on the command line (-m INIT:CAP)
ERROR: shm profile 'MEM_SHM': scale-up target 96 MB does not exceed the initial 128 MB - the profile could never act
ERROR: shm profile 'MEM_SHM': scale-up target 2048 MB exceeds the 1024 MB reservation - raise the :CAP
ERROR: shm profile 'MEM_SHM': scale-down target 1 MB is below the 2 MB minimum viable arena
ERROR: shm profile 'MEM_SHM': scale-down target 512 MB is not below the scale-up target 256 MB
```

### Units, small arenas, and the minimum

The profile numbers are **MB by default**; any of the two size positions
takes a `k`/`m`/`g` suffix, and one suffix anywhere switches the whole
profile to KB internally (`scale up to 16m ... down to 512k` is
consistent). `-m`/`-M` take the same suffixes on either half of
`INIT[:CAP]` — `-M 512k:1m` — with bare numbers staying MB, so no
existing invocation changes meaning.

The minimum viable arena follows the arena's own page unit, not a fixed
number:

* An arena whose whole reservation (`max(INIT, CAP)`) holds at least one
  system huge page runs on huge-page geometry, and its minimum — for the
  initial size and for a profile's scale-down floor alike — is **one**
  system huge page (2 MB on a stock x86_64 host, whatever
  `/proc/meminfo Hugepagesize` says elsewhere). It used to be two.
* An arena whose whole reservation is smaller than one huge page runs in
  **small mode**: a 256 KB page grid, the hugetlb/THP tiers skipped
  outright (nothing sub-huge-page can ever become a huge page), growth
  stepping one 256 KB page at a time, and a **512 KB** minimum.
  `-M 512k:1m` is a real, growing, shrinking elastic arena on a box
  where 4 MB per worker is too much to ask. The 512 KB floor is
  physics, not policy: a buddy block never spans pages, so the page caps
  every contiguous allocation — and the arena's own fixed furniture (the
  ~31 KB `hg_block` header, slab chunks up to ~131 KB for the largest
  class, core startup's single ~140 KB `init_pvar_support` object)
  needs a 256 KB ceiling to exist at all. A 64 KB grid was tried and
  failed on exactly those.

The one geometric consequence to know: a floor below 2 MB requires the
whole arena to be sub-huge-page. With a 16 MB cap the arena is on
huge-page geometry and committed sizes are whole huge pages — `scale
down to 1m` is not refusable-policy but impossible-geometry (half a huge
page cannot exist), so the floor there is `2m`.

A profile named while a different allocator runs is ignored with a
WARN. Every accepted profile announces itself once, so a config's
effect is verifiable from the log alone:

```
NOTICE:core:hg_autoscale_apply: shm auto-scaling profile 'MEM_SHM': 32..160 MB (start 64), up at 60% for 2/3 cycles, down at 20% for 1 cycles (cooldown 10)
```

---

## 11. Dry-run mode

```
hg_autoscale_dry_run = 1
```

Advise-only: ticks **and even emergency exhaustion growth** log what
they would have done and act never — the arena behaves exactly like a
fixed v2 arena while you watch what the policy thinks:

```
NOTICE:core:hg_grow_tick: shm: DRY RUN - would grow (committed 64 MB, usage 97%, profile ceiling 160 MB)
WARNING:core:hg_buddy_grow: shm: DRY RUN - would grow for a 285488 byte request (committed 64 MB); counting further suppressed grows in hg_shm_grow_refused
NOTICE:core:hg_shrink_tick: shm: DRY RUN - would shrink (committed 96 MB, usage 12%)
```

Suppressed grows still count in `grow_refused`, so the *volume* of what
dry-run declined is measurable, not just its existence. Proven: a
dry-run arena at 97% usage under held load produced the advice lines
and **zero** actual grows, committed unchanged.

Deployment pattern: ship the profile with dry-run on, watch a few days,
tune thresholds against the advice lines, flip to 0.

---

## 11b. Module arenas — memory a module asks for and HG manages

A module whose memory has nothing to do with transactions — a cache with
its own lifetime, class mix and growth — can ask for an arena of its own
instead of carving chunks out of shm and managing them itself:

```c
#include "../../mem/mem_arena.h"
mem_arena_t *a = shm_arena_create("cachedb_perf", init_bytes, cap_bytes); /* mod_init only */
shm_arena_set_profile(a, "CACHE_PROFILE");     /* optional: the Section 10 grammar */
p = mem_arena_malloc(a, size);  mem_arena_free(a, p);
```

The arena is an HG block like shm — slab classes, per-process caches,
block GC and cross-class re-typing, elastic growth and shrink within its
own `INIT:CAP`, the maintenance process (which ticks every registered
shared arena), its own section in `hg_stats` and its own lock
histograms — created before the fork so every child inherits the one
shared mapping, independent of which allocator `-a` selected for the
core. The ownership registry routes a free to whichever arena owns the
pointer. With no HG_MALLOC compiled in, `shm_arena_create()` returns
NULL and the module falls back to its own scheme. Up to 8 arenas.

**First consumer: cachedb_perf.** Its `memory_backing` parameter
(`auto|core|own-hg|own`) decides at startup where its cells live:
`core` — the shm allocator is HG, so every cache cell is an HG slab
cell and nothing is managed in the module; `own-hg` — `arena_hugepage_mb`
(and `arena_hugepage_cap_mb`, `arena_profile`) turn into an HG arena
named `cachedb_perf`, listed by name in `hg_stats`, whatever `-a`
selected for the core; `own` — any other allocator, where the module
runs its own slot allocator with its own reclaim process. `auto` picks
own-hg when an arena is asked for, core when the shm allocator is HG,
own otherwise.

## 12. pkg arenas — what is different

* **Per-child, post-config.** Each worker's private arena is created at
  fork time (after the config is parsed), so it takes the full policy:
  cap from `-M INIT:CAP`, profile numbers from a fork-inherited
  resolution.
* **The pre-fork parent arena stays fixed** at `-M`'s initial size — it
  predates the config, and the attendant barely allocates. Documented
  cost: none in practice.
* **Ticks run in the owner.** Only a process can shrink its own private
  arena; the pkg grow/shrink gates tick from each process's sweep-flush
  path, once per interval, same cadence as shm.
* **Host costs multiply.** Both the RAM-floor check (automatic) and
  your capacity plan (manual) must charge pkg deltas × the worker
  count. `-M 16:64` on a 30-worker gateway is up to 1.9 GB of potential
  pinned growth — and on tier 1, the same multiplication applies to
  **pool reservations** (each child's arena reserves its own cap at map
  time).

What the multiplier means on a real 54-worker gateway — to survive one
worker's burst with a fixed `-M` you must hand the burst size to all
54; with a range, the floor is the norm and the burst is one worker's
temporary excursion:

```mermaid
xychart-beta
    title "pkg, 12 of 54 workers: fixed -M sized for the burst vs v3 (MB)"
    x-axis ["w1","w2","w3","w4","w5","w6","w7","w8","w9","w10","w11","w12"]
    y-axis "MB per worker" 0 --> 36
    bar [32,32,32,32,32,32,32,32,32,32,32,32]
    bar [16,16,16,32,16,16,16,16,16,16,16,16]
```

*First bars: fixed `-M 32` because worker 4 once needed 32 —
1,728 MB across 54 workers, around the clock. Second bars: v3 with
`-M 16:32` — 53 workers hold the 16 MB floor, the burst worker grows a
granule and returns it after the quiet window; peak 880 MB. When the
profile's down-target sits below `-M`, the floor drops further still.
(Illustrative; the five-worker independence proof is below.)*

Proven: five workers each grew their own arena 8→24→40 MB
independently, on per-delta-verified THP backing, with every stamped
word intact.

---

## 13. Deployment cookbook

### 13.1 Billing gateway, hugetlb tier

Start at the proven working size, allow storm growth, never shrink
below the start (a gateway holds long-lived state).

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

```bash
# /etc/sysctl.d/60-opensips.conf — the pool must fit the CAPS:
#   shm cap:              1024 MB          = 512 pages
#   pkg cap × 31 workers:   32 MB × 31     = 496 pages
#   rounding / restart headroom (~5%)      ≈ 50 pages
vm.nr_hugepages=1060
```

```ini
# systemd drop-in
[Service]
LimitMEMLOCK=infinity
```

Only the caps draw from the pool. The pre-fork (attendant) pkg arena —
the one every child inherits copy-on-write — is deliberately **not**
hugetlb-backed: a child's COW fault inside a hugetlb mapping has no 4K
fallback and no reservation, so an empty pool at fork time was a silent
SIGBUS (measured; it is how TCP main, the last no-script child, died at
startup on a short pool — twice). Its ladder starts at THP instead,
whose COW splits to 4K pages, and no-script children no longer walk the
inherited route AST with `pkg_free` before swapping to their own arena.
Startup therefore cannot SIGBUS on pool state: a short pool only pushes
late children down the tier ladder. If the pool cannot fit a cap:

```
NOTICE: hugetlb pool cannot back a 1024 MB cap; reserving the 128 MB in use instead - the arena keeps huge pages but cannot grow. Raise vm.nr_hugepages to allow growth.
```

### 13.2 Load balancer, measured-first

An LB's real footprint is small (a production LB measured **11.9 MB
peak shm over 15 h** of full traffic; worst per-process pkg 2.4 MB).
Start small, keep storm headroom, hand idle memory back:

```bash
opensips -f lb.cfg -m 64:512 -M 16 -a HG_MALLOC
```

```
auto_scaling_profile = MEM_LB
    scale up to 512 on 70% for 2 cycles within 4
    scale down to 32 on 20% for 20 cycles

shm_auto_scaling_profile = MEM_LB
```

`down to 32` sits below `-m 64` deliberately: after a storm passes, the
arena returns even part of the initial allocation.

### 13.3 First rollout — dry-run

Section 11. Profile + `hg_autoscale_dry_run = 1`, observe, tune, flip.

### 13.4 Cap only, no profile

```bash
opensips -f opensips.cfg -m 256:2048 -a HG_MALLOC
```

Exhaustion growth + conservative built-in shrink (4 quiet intervals per
granule, never below `-m 256`). No proactive behaviour, no config
surface at all.

### 13.5 Shared-memory THP at fault: `shmem_enabled=advise`

The kernel default `transparent_hugepage/shmem_enabled = never` means a
shared (shm) arena gets huge pages only through the `MADV_COLLAPSE`
retrofit: every grow populates 4 K pages and then copies them into huge
pages. Measured on the 1M bench, that collapse is three quarters of each
grow's commit — 48 ms of the 64 ms a 16 MB grow held the arena lock —
and it is the part that costs requests: fill p99 6.05 ms with 151 lost
REGISTERs, cold-pull p99 50 ms, 2,143 kernel UDP drops. With

```
echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

(runtime, no restart; persist via sysfs in your config management) the
`MADV_HUGEPAGE` the arena already issues makes the populate fault 2 MB
pages directly — the init line reads `THP 2M pages via MADV_HUGEPAGE
(huge at fault)` — and the same run gave fill p99 0.89 ms, cold p99
23 ms, zero lost, zero drops, the commit down to 29 ms mean. Set it on
every host that runs an elastic shm arena; the hugetlb tier (13.1) is
unaffected by it.

### 13.6 A residual stall is latency, not loss: `maxbuffer`

A worker parked for tens of milliseconds cannot drain its UDP socket;
with the default 256 KB receive buffer a burst of ~1,500 INVITE-sized
datagrams overflows it and the kernel drops (`UdpRcvbufErrors`) — the
lost-request counts in every elastic run above came from exactly that.
The socket queue is the only buffer between the NIC and the workers
(OpenSIPS reads one datagram at a time with `recvfrom()`; there is no
second, internal queue), and its size is what OpenSIPS asks for at
startup — the core parameter `maxbuffer`, 256 KB by default — bounded by
what the kernel allows, `net.core.rmem_max`. Both have to move, and
neither is code:

```
sysctl -w net.core.rmem_max=8388608      # the kernel's ceiling
maxbuffer = 8388608                       # opensips.cfg: what the SIP sockets ask for
```

Raising only the sysctl changes nothing (the sockets still ask for
256 KB); raising only `maxbuffer` is clamped at the old ceiling. With
both, a burst that a stall leaves unread parks in the queue and is
drained afterwards, one datagram at a time — a stall the allocator has
not yet removed becomes latency rather than loss. All UDP workers of a
socket share that queue, so a single parked worker never drops anything;
drops need every worker parked at once, which is what the two-phase
commit ended.

### 13.7 Large allocations pack into whole pages

Allocations above the largest cell class (64 KB) go to the large tier,
which carves its chunks from the buddy grid. On arenas of 256 MB and
more a large chunk is one whole huge page (2 MB); below that it keeps
the slab tier's 256 KB granule. The reason is packing: a 256 KB request
plus its headers does not fit twice in a 512 KB block, so with the small
granule half of every block stayed dead — measured at 420 MB of idle
remainders on a million-record node whose cache blobs arrive in 256 KB
chunks. A 2 MB chunk holds seven of them. `hg_stats` →
`large_backing` versus `large_live` shows the packing loss directly.
Measured on the 1M rig: the puller commits 2,272 MB for the same
1,852 MB of records instead of 2,688 (110 grows instead of 136, all
ahead of demand), the owners carve 925 MB instead of 1,105, and every
request figure is unchanged on both host configurations.

### 13.8 The first soak, charted

Two nodes, first 14 hours on v3 (2026-08-14/15), every point taken from
the arena's own grow/shrink NOTICE lines. The LB ran the section 13.2
recipe untouched; the gateway was deliberately re-cut mid-soak to a
near-empty start (`-m 8:512 -M 2:32`) to make growth earn everything.

```mermaid
xychart-beta
    title "LB .250: committed MB (top line shm, flat line pkg)"
    x-axis ["23:49 boot", "23:59", "00:09", "08:19", "10:09", "14:00"]
    y-axis "committed MB" 0 --> 70
    line [64, 48, 32, 48, 34, 34]
    line [16, 16, 16, 16, 16, 16]
```

*The 13.2 config doing its job unattended: idle 64 shrinks to the 32
floor within 20 minutes of boot, morning traffic grows it back to 48,
and the after-peak shrink releases only what is genuinely empty —
committed lands on 34, not 32, because 2 MB of the growth still holds a
live allocation ("7 pages released; 2 MB of growth still held"). pkg
never moved on any of the 28 workers. Changes are instantaneous steps;
the slopes are an artifact of the event-spaced axis.*

```mermaid
xychart-beta
    title "GW .244: committed MB (shm; pkg typical; pkg 3 grown workers)"
    x-axis ["23:50 boot", "00:10", "00:30", "00:57 restart", "01:17 re-cut", "01:19", "14:00"]
    y-axis "committed MB" 0 --> 70
    line [64, 48, 32, 64, 8, 24, 24]
    line [16, 16, 16, 16, 2, 2, 2]
    line [16, 16, 16, 16, 2, 18, 18]
```

*The adversarial arm. First boot (`-m 64`) runs the predicted shrink
train to 32; a restart resets to 64; then the re-cut starts shm at
**8 MB** and per-worker pkg at **2 MB**. Demand pulls shm to 24 within
two minutes and exactly three of 54 workers grow their pkg to 18 —
the other 51 stay at the 2 MB floor. Twelve hours of steady state
followed.*

Both nodes, the whole window: `grow_refused` 0, `grow_blocked` 0,
corruption counters 0, tier-1 hugetlb throughout, and the
`E_CORE_SHM_GROW_BLOCKED` event never fired.

---

### 13.9 Small footprint — KB-scale arenas for constrained hosts

Everything in the sizing grammar takes `k`/`m`/`g` suffixes — `-m`/`-M`
on either half of `INIT:CAP`, and the profile's two size positions —
with bare numbers keeping their historical MB meaning. The minimums
follow the arena's own geometry, not a fixed number:

| whole reservation (`max(INIT, CAP)`) | page grid | grow/shrink step | minimum |
|---|---|---|---|
| ≥ 1 system huge page (2 MB typical) | huge pages | 16 MB granule / whole pages | **1 huge page** |
| < 1 huge page ("small mode") | 256 KB | **256 KB** | **512 KB** |

A box where 4 MB per worker is too much to ask:

```bash
# pkg: half a megabyte per worker, growing to at most 1 MB, in 256 KB steps
opensips -f opensips.cfg -m 16:64 -M 512k:1m -a HG_MALLOC
```

```
auto_scaling_profile = MEM_PKG
    scale up to 1m on 70% for 2 cycles within 4
    scale down to 512k on 20% for 40 cycles

pkg_auto_scaling_profile = MEM_PKG
```

Small mode skips the hugetlb/THP tiers by construction (nothing
sub-huge-page can ever become a huge page), so these arenas run plain
4 K pages regardless of host configuration — the init line says so:

```
NOTICE:core:hg_malloc_init_cap: pkg HG_MALLOC_V3 arena (UDP receiver): 512 KB on plain 4K pages, 512 KB pinned from swapping
NOTICE:core:hg_malloc_init_cap: pkg arena can grow to 1 MB (512 KB headroom reserved, uncommitted)
```

**The two consequences to size against before choosing a sub-MB pkg:**

* **The reactor scales with pkg.** The core caps each process's reactor
  at 10% of pkg memory: `-M 4` gives ~10,500 fds, `-M 512k` gives
  **1,310** (the startup WARN names the number). That is also the
  effective ceiling on TCP connections when it is lower than
  `tcp_max_connections` (default 2048). Measured against a real LB's 13
  days of traffic (worst process: 125 fds), 1,310 is 10× headroom — but
  measure yours, do not assume.
* **A pkg spike above the cap fails allocations on that worker.** A
  1 MB cap is a real ceiling; the 13-day peak on the reference LB was
  680 KB (66%). Check `proc_max_used_size` per process over a long
  window before committing to a small cap.

Why the floor is 512 KB and not less: a buddy block never spans pages,
so the 256 KB page caps every contiguous allocation — and the arena's
own furniture (the ~31 KB header, slab chunks up to ~131 KB, core
startup's single ~140 KB `init_pvar_support` object) needs that
ceiling to exist at all. A 64 KB grid was built and measured failing on
exactly those.

### 13.10 Deliberate plain-4K: no huge pages at all

The opposite of 13.5: run an elastic arena on plain 4 K pages on
purpose (fleet uniformity, a host whose huge pages belong to another
tenant, or simply to measure the tier's cost). Huge pages must be
removed from every rung of the ladder, and one of the knobs is not the
obvious one:

```bash
# /etc/sysctl.d/60-opensips.conf — no hugetlb pool, no surplus:
vm.nr_hugepages=0
vm.nr_overcommit_hugepages=0
```

```bash
# THP off for anon (pkg) AND shmem (shm). shmem needs DENY, not never:
echo never > /sys/kernel/mm/transparent_hugepage/enabled
echo deny  > /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

```
# persist the sysfs pair, e.g. /etc/tmpfiles.d/opensips-thp.conf:
w /sys/kernel/mm/transparent_hugepage/enabled - - - - never
w /sys/kernel/mm/transparent_hugepage/shmem_enabled - - - - deny
```

**Why `deny`:** `never` only gates fault-time THP and khugepaged — the
`MADV_COLLAPSE` retrofit (tier 3) is an explicit request that bypasses
it by design. Measured on a 6.12 kernel: with `never`/`never` the shm
arena still came up reading `THP 2M pages via MADV_COLLAPSE (post-fill
retrofit)`; only `shmem_enabled=deny` forbids the collapse and yields
an honest `plain 4K pages`. Also mind the overcommit pool: with
`nr_overcommit_hugepages > 0`, `mmap(MAP_HUGETLB)` succeeds from
surplus even when the static pool is zero — both must be 0 to kill
tier 1.

Costs to expect: huge pages bought ~19% of the allocator's own
self-time in the isolated measurements (Section 19), and mlock pinning
now applies (tiers 2–4 pin explicitly; keep `LimitMEMLOCK` sized).
What you get back: the whole hugetlb reservation returns to general
RAM (a 790-page pool is 1.58 GB), and sizing no longer needs pool
arithmetic at all — no `vm.nr_hugepages` to fit the caps into.

### 13.11 Tuning from measurement — the full walkthrough

The method used on the reference LB after 13 days of real traffic,
start to finish. Upper limits (caps, up-targets) stay; only floors
tighten.

**1. Read the high-water marks** (both are cheap, run them on the live
node):

```bash
opensips-cli -x mi core:hg_advise    # per-arena verdict + recommendation
opensips-cli -x mi core:hg_stats     # carved_peak / live_peak / grows
```

The reference numbers: shm `peak_bytes` 8.6 MB against a 32 MB start
(25%, verdict "reasonable", recommendation 18 MB — an *upper bound*,
per the large-tier caveat in the output); pkg `carved_peak` 680 KB
against 4 MB (13%). `grows: 0` on both — the starts had never been
touched.

**2. Choose the INIT sizes**: put the measured peak at 50–70% of the
new start, so a normal day sits under the profile's up-trigger and the
arena neither flaps at startup nor carries dead headroom.
8.4 MB / 0.52 → shm `16`; 680 KB / 0.66 → pkg `1m` cap with a `512k`
start (small mode, 13.9) — or `2:16` if the cap must stay.

**3. Choose the floors**: as low as geometry allows, unless long-lived
state argues otherwise. `2m` for any huge-page-mode arena, `512k` for
small mode. A floor below the steady live size is harmless — shrink
stops at live data regardless; the floor is permission, not a demand.

**4. Apply and re-read.** `hg_advise` says it itself: some large
consumers size themselves as a fraction of the arena, so apply once,
restart, and read again rather than iterating on stale numbers.

```bash
# /etc/default/opensips — before -> after on the reference LB:
SHM_MEMORY=32:128   ->  SHM_MEMORY=16:128
PKG_MEMORY=4:16     ->  PKG_MEMORY=512k:1m
```

```
auto_scaling_profile = MEM_SHM
    scale up to 128 on 70% for 2 cycles within 4
    scale down to 2m on 20% for 20 cycles
auto_scaling_profile = MEM_PKG
    scale up to 1m on 70% for 2 cycles within 4
    scale down to 512k on 20% for 40 cycles
```

The startup NOTICEs are the confirmation the whole chain took:

```
NOTICE:core:hg_autoscale_apply: shm auto-scaling profile 'MEM_SHM': 2 MB..128 MB (start 16 MB), up at 70% for 2/4 cycles, down at 20% for 20 cycles (cooldown 200)
NOTICE:core:hg_autoscale_apply: pkg auto-scaling profile 'MEM_PKG': 512 KB..1 MB (start 512 KB), up at 70% for 2/4 cycles, down at 20% for 40 cycles (cooldown 400)
```

One relation to keep in mind while picking numbers: an unaligned floor
rounds **up** to the arena's page and the NOTICE prints the effective
value (`down to 1m` under a 16 MB cap reads back as `2 MB..16 MB`) —
a floor below 2 MB is only reachable when the whole reservation is
below one huge page, because half a huge page cannot exist.

## 14. Monitoring and alerting

### 14.1 MI

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

Field notes:

| field | meaning |
|---|---|
| `tier` | what **init** achieved |
| `tier_bytes` | per-tier byte split — appears only when >1 tier holds bytes; the honest answer to "is my grown arena still on huge pages" |
| `committed` / `cap` / `grow_headroom` | the elastic state; `committed == cap` ⇒ fixed or fully grown |
| `grows` / `grow_bytes` | successful commits and their total |
| `grow_refused` | refusals, both admin and resource — the magnitude counter behind once-per-episode logging |
| `grow_blocked` | the latched gauge (resource refusals only) |
| `shrinks` / `shrink_bytes` | successful releases and their total |
| `grows_proactive` / `grows_exhaustion` | which path asked for each grow: the profile's tick ahead of demand, or a request that found nothing to carve |
| `lock` | arena-lock timing (below) |

`lock` is the answer to "what held the arena lock, for how long, and did it
stall anyone": every `hb->lock` section is tagged with its reason —
`refill` (class refill: pool pop, chunk carve, a grow on exhaustion),
`return` (cell return, donate, `gc_class` block returns), `flush`
(per-process cell-cache flush), `large`, `region`, `policy` (grow/shrink
tick, profile apply), `stats` — and timed on both sides of the acquire:

```json
"lock": {
    "stall_threshold_us": 1000,
    "stalls": 0,
    "worst_hold_us": 412,
    "worst_reason": "refill",
    "worst_process": 12,
    "hold": { "refill": { "n": 18213, "mean_us": 3, "max_us": 412, "stalls": 0,
                          "hist_log2us": [9120, 6011, 2410, 540, 101, 24, 6, 1, 0, ...] },
              "return": { ... }, "flush": { ... }, "policy": { ... } },
    "wait": { "refill": { ... }, ... },
    "commit": { "n": 6, "mean_us": 2870, "max_us": 4102, "stalls": 0, "hist_log2us": [...] },
    "gc":     { "n": 3310, "mean_us": 2, "max_us": 57, "stalls": 0, "hist_log2us": [...] }
}
```

`commit_phases` (present once a grow has happened) splits the commit into
`meminfo`, `advise`, `mlock_populate`, `verify` and `collapse` — the
`/proc/meminfo` read, the `MADV_HUGEPAGE`, the populating `mlock`, the
backing verification (a huge-page counter delta across the populate; it
used to be a `/proc/self/smaps` walk of the whole mapping, 8–11 ms per
grow under the lock on a 3 GB arena) and the `MADV_COLLAPSE` retrofit —
so a slow grow says which part is slow. Measured on the 1M bench, per
16 MB grow: populate 14–20 ms, collapse 48 ms when the host needs it
(see 13.5), the rest microseconds.
`grow_wait` records the unlocked waits of workers that exhausted while
another worker's grow was populating; `grow_inflight`, `grow_waits` and
`grow_wait_timeouts` next to `grows` are the two-phase commit's state.
`hold` is the time the lock was held, `wait` the time spent acquiring it,
both per reason; `commit` is the `hg_mem_commit()` part of a grow
(populate + pin + collapse — the part that holds the lock for
milliseconds) and `gc` every `gc_class()` block-return pass, timed on
their own because they run *inside* a `refill`/`return`/`policy` hold.
`hist_log2us` is a log2 histogram in microseconds: bucket 0 is `< 1 us`,
bucket k is `[2^(k-1), 2^k) us`, the last bucket (index 17) is `>= 64 ms`.
A hold at or above `hg_lock_stall_us` (config, default 1000) is a *stall*:
counted per reason and in total, remembered as `worst_*`, logged at most
once per second per process, and raised as `E_CORE_HG_LOCK_STALL` from
the sweep timer. Reasons with no samples are omitted. The cost of the
instrumentation is two `clock_gettime(CLOCK_MONOTONIC)` reads per
slow-path section (~20 ns each via the vDSO) — the lock-free fast path
never comes here.

The `pkg` section reports the **answering MI process's own** arena —
stated honestly rather than pretending fleet-wide pkg visibility.

### 14.2 Statistics (Prometheus-friendly)

```
hgmem:hg_shm_committed      bytes committed right now
hgmem:hg_shm_cap            the reservation
hgmem:hg_shm_grows          counter
hgmem:hg_shm_grow_bytes     counter
hgmem:hg_shm_grow_refused   counter  — rising = demand is hitting a wall
hgmem:hg_shm_grow_blocked   GAUGE    — the one to alert on
hgmem:hg_shm_shrinks        counter
hgmem:hg_shm_shrink_bytes   counter
hgmem:hg_shm_grows_proactive   counter — grows asked for by the profile tick
hgmem:hg_shm_grows_exhaustion  counter — grows asked for by an exhausted request
hgmem:hg_shm_lock_stalls       counter — arena-lock holds >= hg_lock_stall_us
hgmem:hg_shm_lock_hold_max_us  high-water mark of the longest hold (us)
```

Suggested alerts:

* `hg_shm_grow_blocked == 1` for > 1 min → page. This is "demand
  present, host cannot supply, reclaim did not help".
* `rate(hg_shm_grow_refused[10m]) > 0` with `grow_blocked == 0` →
  ticket, not page: the admin ceiling is being hit, or refusals are
  isolated; check whether the ceiling still matches reality.
* `hg_shm_committed / hg_shm_cap > 0.9` sustained → the cap is close;
  plan a restart with a larger `:CAP` (the reservation cannot be raised
  live).
* `rate(hg_shm_lock_stalls[5m]) > 0` → a slow path held the arena lock
  for a millisecond or more; `hg_stats` → `lock.worst_reason` says which
  (`refill` with `commit.max_us` in the thousands = growth committing
  under the lock; `return` with `gc.max_us` high = block returns;
  `flush` = the sweep's cache flush).

### 14.3 The events

Section 8 — `E_CORE_SHM_GROW_BLOCKED`, params `arena`, `committed_mb`,
`cap_mb`, `grow_refused`; raised on latch and every 5 minutes while
held; with no subscriber, a WARN says so and points at the gauge.

`E_CORE_HG_LOCK_STALL`, params `arena`, `reason`, `hold_us`, `process`,
`stalls` (the running total): the most recent arena-lock hold at or above
`hg_lock_stall_us`, raised from the sweep timer (never under the lock —
raising allocates shm), at most once per sweep interval. Silent with no
subscriber; `hg_shm_lock_stalls` carries the count for pollers.

---

## 15. Log line reference

All at their exact severities; `%` values are illustrative.

| line | severity | meaning / action |
|---|---|---|
| `shm arena can grow to N MB (M MB headroom reserved, uncommitted)` | NOTICE | startup: a cap exists |
| `shm auto-scaling profile 'X'...: A..B MB (start C), up at ...` | NOTICE | profile attached; the one line that proves your config took effect |
| `... [DRY RUN - advise only]: ...` | NOTICE | ditto, advise-only |
| `shm arena grew <proactively\|on exhaustion> by 16 MB to N MB (8 new pages on <tier>; commit K us with the lock released; M MB headroom left)` | NOTICE | growth, which path asked, the delta's **verified** backing, and how long the populate took (no longer under the lock) |
| `shm arena lock held for N us by the <reason> path (threshold hg_lock_stall_us=1000; S stalls so far, worst W us by <reason> in process P)` | WARN | a stall (Section 14.1 `lock`); at most one line per second per process — the counters and the event carry the magnitude |
| `shm arena shrank by 16 MB to N MB (8 pages released to the <hugetlb pool\|host>; M MB of growth still held)` | NOTICE | shrink, with where the memory went |
| `at the N MB growth ceiling (the -m/-M reservation \| the profile scale-up target), a K byte request must fail - counting further refusals in hg_shm_grow_refused` | NOTICE | admin limb refusing; once per episode; not an incident |
| `cannot grow by 16 MB: mlock failed (...)` / `refusing to grow by 16 MB: N MB effective (xP processes) would leave the host under the F MB floor` | WARN | resource limb refusing; once per episode |
| `GROW-BLOCKED latched - the arena cannot grow and a <GC pass\|full sweep interval> did not change that (N refusals so far)` | WARN | the latch; gauge is now 1 |
| `GROW-BLOCKED cleared - <the arena grew...\|demand fell back below the floor>` | NOTICE | recovery |
| `DRY RUN - would <grow\|shrink> (...)` | NOTICE/WARN | advise-only decisions |
| `hugetlb pool cannot back a N MB cap; reserving the M MB in use instead` | NOTICE | pool < cap at startup; arena fixed on huge pages |
| `mlock of the N MB HG_MALLOC arena failed (...): continuing unpinned` | WARN | init pin failed (RLIMIT); non-fatal, arena swappable |
| `cannot release memory (MADV_... failed): shrink disabled for this arena` | WARN | kernel refused the primitive; once, permanent for the run |

---

## 16. Sizing rules that are not obvious

1. **Tier 1: the pool must fit the caps.** The whole `-m` cap is
   reserved from the hugetlb pool at map time, and every per-child pkg
   arena reserves its own `-M` cap the same way. Budget
   `shm_cap + pkg_cap × workers + ~12% margin` pages.
2. **A short pool degrades, it no longer kills.** The one arena
   children inherit copy-on-write — the attendant's pkg arena — is kept
   off hugetlb (13.1), so a pool with zero free pages at fork time
   pushes late children to THP instead of SIGBUSing them. That SIGBUS
   was measured, twice, before this rule existed.
3. **pkg caps multiply** — RAM and, on tier 1, pool reservations.
4. **Tier-1 shrink returns pages to the pool, not to RAM.** Freeing
   host memory requires shrinking `vm.nr_hugepages` as well.
5. **When verifying pool behaviour, never read `HugePages_Free`
   alone.** Per-process *private* hugetlb (each worker's pkg arena and
   its copy-on-write touches of the inherited parent arena) moves the
   same counter, in the opposite direction, at a similar pace. Compute

   ```
   object_pages = (HugePages_Total − HugePages_Free) − Σ Private_Hugetlb(all pids)
   ```

   from `/proc/meminfo` + `/proc/<pid>/smaps`. Three consecutive test
   rigs misread a *working* tier-1 shrink as broken before this was
   applied; the exact measurement showed the shared object stepping
   96 → 16 MB precisely as `committed` claimed (the 3-page remainder
   was the `shm_dbg` pool).
6. **A small negative `HugePages_Rsvd` (reads as huge unsigned) can
   appear** on nodes running per-child pkg arenas — a kernel accounting
   drift tied to the abandoned-parent-arena COW pattern, bounded to a
   few pages of overstated headroom. Known, monitored, not caused by
   v3.

---

## 17. Troubleshooting

| symptom | cause | fix |
|---|---|---|
| startup: `the arena has no growth room` | profile attached but no `:CAP` on `-m`/`-M` | add the cap: `-m 128:1024` |
| startup: `scale-up target ... exceeds the ... reservation` | profile wants more than the cap | raise `:CAP` or lower the target |
| startup: `does not name an auto_scaling_profile` | typo, or the profile block is below the `shm_auto_scaling_profile` line in a way the parser never saw | check `auto_scaling_profile = NAME` exists and parses |
| `hugetlb pool cannot back a ... cap` at init | `vm.nr_hugepages` smaller than the caps | grow the pool (rule 1), restart |
| arena grew but new pages are 4 K on a THP host | each delta negotiates independently | expected; see `tier_bytes`; consider tier 1 for guarantees |
| never shrinks | top page busy, or inside the post-grow cool-off (10× down-cycles), or usage above the down-threshold, or below-floor/blocked safety hold | check `hg_stats`; prefer-low needs time to drain the top |
| `DRY RUN - would grow` but nothing happens | `hg_autoscale_dry_run = 1` | that is the point; set 0 to act |
| `grow_refused` climbing, gauge 0 | isolated refusals; hysteresis holding | by design — the gauge latches on *sustained* refusal |
| `failed to initialize child process N` / `cannot fork tcp main` at startup, no arena line for that child | a build predating `HG_INIT_INHERITED`: the last no-script child COW-faulted the parent's hugetlb pkg arena on an empty pool | upgrade; meanwhile leave free pages in the pool at fork time |
| arena runs unpinned (`continuing unpinned`) | `RLIMIT_MEMLOCK` too low for a tier 2–4 arena | `LimitMEMLOCK=infinity` in the unit; until then the arena grows unpinned too (populating write faults instead of `mlock`) rather than refusing every grow |
| testing under `ulimit -l` shows no refusals | you are root — `CAP_IPC_LOCK` bypasses `RLIMIT_MEMLOCK` entirely | test the mlock leg as an unprivileged user (`setpriv`) |
| pool numbers "prove" shrink is broken | rule 5 | use the exact object-residency formula |

---

## 18. Testing — the rig and how to reproduce the proofs

### 18.1 hgstress

`modules/hgstress` is the throwaway stress module. Every block is
stamped per (pid, slot) in every 8-byte word, so a page served to two
processes, a lost write, or a punch that ate live data is caught and
named, not inferred.

| param / MI | purpose |
|---|---|
| `slots`, `iters`, `verify`, `large` | the classic multi-process churn soak |
| `hold_mb` | each worker allocates and HOLDS N MB of stamped 128–512 K blocks through the churn — the growth driver; sized past `-m` it forces growth with every worker live |
| `hold_pkg_mb` | same driver for each worker's private arena |
| `again_s` | a timer re-runs one hold/verify/free cycle N seconds in — the regrow-after-shrink proof (timers only run after `child_init` completes, so this is how post-startup cycles are driven) |
| MI `hgs_hold <mb>` / `hgs_release` | allocate/park and verify/free stamped shm **from a live MI process** — the only way to meet sweep ticks, since `child_init` soaks block every timer |

### 18.2 What each proof arm established

| arm | shape | result |
|---|---|---|
| A | fixed 64 MB, demand 120 MB | fail-first control: 109,582 refusals, 0 grows, 0 torn |
| B | cap 256 MB | 7 grows by four different worker pids, 0 refusals, 5/5 PASS — cells in pages committed post-fork verified by pre-fork processes |
| C | cap 96 < demand | grows to cap, ONE at-cap NOTICE (the 239 k-flood fix), refusals counted |
| D | pkg caps | five workers grew their own arenas independently on verified THP |
| E | hugetlb pool | growth on tier 1 to the cap, pool accounting exact; also the zero-margin SIGBUS reproduction |
| F/G | RLIMIT & RAM-floor | the resource limb: latch, gauge, live `event_route` delivery; the ×nproc differential |
| H | grow→free→quiet→regrow | the full ellipse incl. 12 shrinks and a timer-driven regrow INTO punched ranges, 0 torn |
| I | profile | attach line, proactive grow with zero exhaustion errors, ceiling exactly at the profile target, cool-off to the tick, shrink to a below-`-m` floor, dry-run advising with zero action |
| J | non-root, `ulimit -l` | init-unpinned path + the growth-mlock refusal root cannot drive + hysteresis in both directions |
| K | tier-1 lifecycle | pool draw / ceiling / **pool return** / re-draw — plus the instrumentation lesson of rule 5 |

### 18.3 The userspace pre-measurement rigs

`vatest.c`, `growtest.c`, `shrinktest*.c` (session scratchpad) are the
kernel-behaviour probes that chose the mechanisms before any allocator
code was written — each with a control arm that passes. Appendix A is
their output.

---

## 19. Appendix A — measured kernel facts

All on Linux 5.4 (the fleet's oldest) unless noted; every claim
re-verified rather than assumed from documentation.

| # | fact | measurement |
|---|---|---|
| 1 | post-fork `MAP_FIXED` into a `PROT_NONE` reservation is invisible to pre-forked processes | control prefix readable; delta SIGSEGV in the sibling |
| 2 | a whole-cap pre-fork `MAP_SHARED` mapping makes later commits visible everywhere | delta written by parent read correctly by pre-forked child |
| 3 | an untouched mapped span is nearly free | 64 MB span: 576 kB RSS |
| 4 | `mlock(sub-range)` populates and pins exactly that range | RSS/VmLck moved by precisely the delta; tail untouched |
| 5 | `mmap(PROT_NONE\|MAP_FIXED)` "shrink" silently corrupts a shared arena | shrinker wrote 0x77; sibling still read 0xEE |
| 6 | `MADV_REMOVE` punches the object for every mapper | sibling read zeroes; recommit visible |
| 7 | `MADV_REMOVE` frees pages held `VM_LOCKED` by another process | locker's RSS fell by the punched size |
| 8 | hugetlb `MADV_REMOVE` works on 5.4 and returns pages to the pool | `HugePages_Free` 16→20 on an 8 MB punch; re-fault drew them back |
| 9 | `MAP_HUGETLB` reserves the whole mapping from the pool at map time | `Rsvd` +32 pages for a 64 MB map, before any fault |
| 10 | `MAP_NORESERVE` hugetlb takes nothing at map time (SIGBUS risk) | `Free` unmoved |
| 11 | a hugetlb pool with zero free pages SIGBUSes forking children (COW window) | reproduced; `si_addr` inside the parent's private pkg arena |
| 12 | punch behaviour is unchanged by `MADV_DONTDUMP`, many mappers, foreign mlock, or a forked puncher | isolation arms all returned pages |
| 13 | `HugePages_Free` alone cannot judge object residency | three rigs misread a working shrink; private fault-in (63→71 pages) masked the returns; exact formula in rule 5 |
| 14 | root cannot test `RLIMIT_MEMLOCK` | `CAP_IPC_LOCK` bypasses the limit entirely; `setpriv --reuid=nobody` + `ulimit -l` drives the leg |
| 15 | `mlock` on a reservation-backed hugetlb range cannot SIGBUS mid-commit | clean errno contract held through every arm |

---

## 20. Appendix B — internals map for developers

| file | owns |
|---|---|
| `mem/hg_malloc.c` | reservation (`hg_mem_reserve`), commit (`hg_mem_commit`), release (`hg_mem_release`), the tier ladder + verification probes, RAM limb (`hg_grow_ram_refused`), profile attach (`hg_autoscale_post_cfg`/`hg_autoscale_apply`), init/destroy |
| `mem/hg_malloc.h` | `struct hg_block` incl. `hcap`/`hsize_min`/policy copy/latch state/`tier_bytes[]`; the cap globals' contract |
| `mem/hg_buddy.c` | grow (`hg_buddy_grow`) + retry contract, shrink (`hg_buddy_shrink`), the policy ticks (`hg_grow_tick`/`hg_shrink_tick`), latch helpers (`grow_resource_refused`/`hg_grow_unblock`/`hg_grow_blocked_tick`), prefer-low `fl_push` |
| `mem/hg_arena.c` | the exhaustion call sites (carve/region), `npages_cap` layout, floor-recovery unblock hook, the statistics table |
| `mem/hg_large.c` | the large tier's grow-and-retry loop |
| `core_stats.c` | the sweep timer: cache sweep, deferred event raise, shm ticks |
| `main.c` / `globals.c` / `globals.h` | `-m INIT:CAP` parsing; the six always-present globals |
| `cfg.lex` / `cfg.y` | the four config tokens; the (pre-existing, reused) profile grammar |
| `evi/evi_core.[ch]` | `E_CORE_SHM_GROW_BLOCKED` publication (id 6; carries the same `#ifdef STATISTICS` id-shift caveat as `SHM_THRESHOLD`) |
| `mem/shm_mem.c` | the `init_shm_post_yyparse()` attach call |
| `modules/hgstress/` | the proof driver (18.1) |

Development notes that cost real time, recorded so they are paid once:

* gcc 9 (build host 222) does not flag a read-before-assign introduced
  by restructuring a loop condition; the resulting garbage pointer
  (`&evi_time_str+12`) survived one full arm before the rig's detector
  caught it via a zeroed `log_level`. Build on the newer-gcc hosts for
  warning coverage; keep the rigs' detectors on.
* `child_init` work blocks every timer until the last child finishes —
  policy ticks and event raises cannot be observed from a `child_init`
  soak; drive load via MI.
* opensips under `setpriv`/`sh` wrote nothing to stderr in the non-root
  arm — judge such runs by MI counters, not logs.
* A version-stamp check (`version_control`) refuses mixed core/module
  revisions after any commit — full-tree rebuilds between rig runs.

---

## 21. Limitations

* The **pre-fork parent** pkg arena predates the config and stays fixed
  at `-M`'s initial size; profiles govern every per-child arena.
* Shrink is **top-only**; a single live cell in the top page blocks
  release until it moves or dies. Prefer-low allocation drains tops
  over time, but does not relocate live cells — relocation was
  evaluated separately and is unsound here (raw pointers, interior
  pointers, no safepoints), and address-ordered coalescing beyond the
  page was **closed by measurement**: on four production node shapes,
  free space already sat 97–99% in whole huge pages.
* The reservation (`:CAP`) cannot be raised live — it is a mapping (and
  on tier 1 a pool reservation) created before fork. Raising it is a
  restart.
* Growth granule is fixed at 16 MB; commits happen under the arena
  lock — bounded, rare, deliberate.
* Linux-only elasticity; elsewhere the arena is plain fixed memory.
* Tested back to kernel 5.4, on 4 K, THP and hugetlb backings, x86_64.
