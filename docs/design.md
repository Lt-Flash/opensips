# HG_MALLOC design

How HG_MALLOC works, what it guarantees, and how to run and monitor it. For the configuration reference see [configuration.md](configuration.md); for measurements, [benchmarks.md](benchmarks.md).

## 1. Allocation

**Size classes.** Requests up to 64 KB (including an 8-byte cell header) are served from 21 fixed size classes, 64 B to 64 KB. Every cell carries a small hidden header with its class, so a free needs no lookup. Larger requests go to a coalescing large tier that packs into whole pages.

**The common path takes no lock.** Each process (and each thread of a threaded process) keeps a private free list per class. An allocation pops from it and a free pushes to it - no lock, no shared cache line. When a private list runs dry it refills a batch from a shared per-class pool; when it grows past its threshold it donates a batch back. Only those batch transfers take the arena lock.

**Blocks and the buddy allocator.** The arena is a grid of pages the size of the host's huge page - 2 MB on x86-64, larger on arm64 kernels with 16 KB or 64 KB base pages - whichever of the four tiers backs them. A buddy allocator inside each page hands out blocks, and a block is carved into cells of one class. The shared pool keeps each block's free cells on that block and refills from the fullest partially-used block, so emptier blocks drain completely. A block whose cells are all free goes back to the buddy and can be re-cut for any class - memory used by a burst of one size is not stranded there.

**Small arenas.** An arena with an INIT under one huge page settles its page size by the backing it gets. On plain 4 KB memory it runs in small mode, on 256 KB pages: at least 256 KB with a cap, 512 KB without. On huge pages its INIT is raised to one huge page, and an arena whose cap leaves no room above that page is refused at startup.

**Idle caches are swept.** Every 30 s each process flushes what its private lists hold back to the shared pool (staggered, a process or two per second), keeping a small residue for the classes it is actively using; TCP main's I/O threads, which IPC cannot reach, flush at their next job after a sweep. A process that leaves for good - an auto-scaled worker going away - first hands back everything it holds, the unused rest of the blocks it carved included. Without these, a worker that went idle or left would keep its cells forever; one that crashes still does, until restart.

**Integrity checks.** Every cell header carries the cell's state (fresh, live or free) and a 16-bit checksum over its address, class and state, seeded per run - a check against accidents, not a security mechanism. A free needs an intact header in the live state and moves it to free in one atomic step (under the arena lock for a large fragment). For a double free that leaves three cases: two frees of one cell at the same moment, in two processes - only one takes effect; a second free before the cell is handed out again - refused, whatever came between the two; a second free after the cell was handed out again - not detectable (see Known limits). A pointer into the middle of a cell, or one whose header a neighbour's overflow overwrote, is refused as well; an allocation takes only an intact fresh or free cell, so a cell is never handed out twice. Each block also keeps a map of which of its cells are in the shared pool, and every free-list link the pool or a private list follows is checked against the arena, so a scribbled link stops the walk instead of leading it off the arena. HG_MALLOC_DBG adds two checks. Every allocation gets a red zone and a trailer after the size asked for, checked at free and at realloc, so a write past the request is found. A freed cell is filled with a poison pattern by the freeing thread and held in that thread's quarantine (up to 256 cells and about a thousandth of the arena's reservation, 16 KB to 1 MB, per thread) before it can be reused; the same thread checks the poison when it lets the cell go - when the quarantine is full, when an allocation would otherwise fail, and when the process leaves for good. A cell in quarantine is on no free list, so it cannot be handed out before that check, and the poison is in the memory itself, so a write through a dangling pointer is found whichever process made it; it is reported with the site of the free, and the cell is leaked. All are counted in `core:hg_stats` (`corruption`) and logged at CRITICAL on each detection; a refused cell is leaked, never reused, and the process keeps running.

**Sanitizers.** An arena is one mapping, so on its own ASan or Valgrind sees every byte of it as valid. Built with ASan (or with Valgrind's headers and `-DHG_VALGRIND`), HG tells the tool which bytes of a pkg cell the caller may touch: the bytes asked for, not the rest of the cell, and nothing of a freed cell but its free-list link - so ASan stops a pkg use after free, or an overflow into the rest of the cell, at the faulting instruction (an overflow that reaches the next cell lands on its header, which stays open for the allocator, and is caught by the header's checksum instead). Shared arenas are left unmarked: both tools keep that state per process, so a cell freed in one process and handed out again in another would be reported by the first; HG_MALLOC_DBG's in-band poison covers them instead. Under ThreadSanitizer the arena lock is marked as the acquire and release it is (`mem/hg_san.h`).

## 2. Backing memory

The arena is acquired through four tiers, best first, each verified from the kernel's own counters rather than assumed:

| tier | mechanism | notes |
|---|---|---|
| 1 | `mmap(MAP_HUGETLB)` | huge pages from the hugetlb pool; unswappable |
| 2 | THP: `MADV_HUGEPAGE` before first touch | huge pages at fault, from general memory |
| 3 | THP: `MADV_COLLAPSE` after fill | pages collapsed into huge pages afterwards |
| 4 | plain 4 KB pages | always available |

Tiers 2-4 pin the committed arena with `mlock()`. The startup log names the tier achieved, and says why when THP is turned off by a host setting. Huge pages make the allocator's own work cheaper but are not required; an arena on 4 KB pages behaves the same.

**Each growth step negotiates its own backing.** A step may land on a different tier than the memory before it, so `core:hg_stats` reports `tier_bytes` - the split by tier - whenever more than one tier holds bytes. On tier 3 a step taken by a worker that ran out of memory is published on 4 KB pages at once and collapsed by the maintenance process afterwards - one page at a time, with the arena lock released, for up to 100 ms a second, retrying a page the kernel cannot collapse yet up to 16 times - so the worker never waits for `MADV_COLLAPSE` (about 36 ms per 16 MB step); a step taken ahead of demand collapses straight away. `core:hg_stats` counts these pages as `collapse_deferred`, `collapsed_late`, `collapse_retries` and `collapse_late_failed`.

**Tier 1 reserves the whole cap from the pool when the arena is created.** Growth on tier 1 therefore cannot fail half-way, and the pool must be sized for the caps (section 7). Pages the arena gives back when it shrinks return to the pool for anyone to take, so growing back into them can fail if something else took them meanwhile. If the pool can hold the initial size but not the cap, the arena keeps huge pages at its initial size, cannot grow, and says so at startup. If it cannot hold even the initial size, the arena starts on THP with its full cap and stays elastic.

## 3. Elastic arenas

`-m INIT:CAP` (and `-M INIT:CAP` for the private arenas) makes an arena elastic. Without a cap it is fixed at its size. For the smallest INIT see Small arenas (section 1).

**One mapping, created before fork.** The whole cap is mapped once, shared, before any worker forks. Growth and shrink never create or change mappings - they populate and release ranges of that one shared object, which every process sees immediately. An untouched reserved range costs only page-table entries.

**Committed and reserved.** `committed` is what is backed, pinned and usable; `cap` is the size of the reservation. The buddy's page descriptors are laid out for the whole cap at startup, so a grow only publishes pages - it never needs to find room for metadata in an arena that is full.

### 3.1 Growth

Three triggers, one mechanism:

- **Exhaustion** - always armed once a cap exists. An allocation that finds no memory grows the arena and retries once.
- **Headroom** (`hg_grow_ahead`, default on) - when the free part of the arena drops below twice its reserve floor, one step is grown ahead of demand.
- **Profile** - with an `auto_scaling_profile`, usage is sampled once per cycle; at the end of each block of W cycles, if it was at or above the up-threshold in at least C of them, one step is grown before anything fails.

**A grow is a three-phase commit.** The step is reserved under the arena lock; the expensive part - faulting the pages in, pinning them, verifying their backing - runs with the lock released; the new pages are published under the lock again, in constant time. Other allocations keep running throughout. One grow is in flight at a time: a process that runs out while it is in progress waits for the publish without holding the lock (up to 2 s, then the request fails and `grow_wait_timeouts` counts it).

**The maintenance process.** Elastic shared arenas (the shm arena and module arenas) get a dedicated core process, `HG maintenance`. It serves no requests; every second it applies the headroom rule, every `hg_scaling_cycle` seconds it evaluates the profile and the shrink policy, and it does the populate of any step it grows itself - so no SIP worker waits on a proactive grow. Private arenas have no maintenance process: each process runs its own arena's policy in its 30 s cache sweep.

**Step size.** `shm_grow_granule` / `pkg_grow_granule`, default 16 MB, rounded to the arena page. Smaller steps track demand more closely and make each commit shorter.

### 3.2 Limits

Growth stops at the smallest of three limits:

| limit | what | refusal |
|---|---|---|
| admin | the profile's scale-up target, else the cap | logged as a notice - a limit doing its job |
| backing | the kernel failing to back the memory; on tier 1 also the hugetlb pool (checked at startup) or the memory-lock limit | resource refusal |
| host memory | `MemAvailable` must stay above `hg_ram_floor_mb` (default MemTotal/20, at least the smaller of 256 MB and MemTotal/8); in a memory-limited cgroup (a container, a unit's `MemoryMax`) the cgroup's limit less what it holds must also stay above the same floor computed on the limit | resource refusal |

The host-memory check charges a private-arena step once per OpenSIPS process, since every process grows its own under the same load. Tier 1 skips it: hugetlb pages were taken from host memory when the pool was created.

The same floor guards the INITs: at startup every process's private arena is checked against RAM (and the hugetlb pool, if the host has one) before anything forks, and each later fork - the auto-scaler's included - is skipped if its INIT does not fit (see [Host memory checks](configuration.md#host-memory-checks)).

On hugetlb pages every grow is an `mlock()` of the new range, and `mlock()` checks `RLIMIT_MEMLOCK` on hugetlb mappings too, so there a low limit refuses growth: the arena warns at startup, and a refused grow names the resource that ran out - the limit, or the hugetlb pool. On tiers 2-4 the pin is a latency guarantee, not a requirement: an arena that could not pin its initial size runs unpinned, and a grow the limit cannot cover continues unpinned instead of being refused (section 8).

### 3.3 GROW-BLOCKED

Only resource refusals count: on any tier the host floor or the kernel failing to back the memory, and on hugetlb pages also the memory-lock limit or the pool. Reaching the cap or the profile's scale-up target is a limit doing its job: it is logged once as a notice and counted in `grow_refused`, and never latches.

A resource refusal that persists is an incident; a single one is not. The first refusal arms the state. It latches at the next 30 s sweep tick if refusals continued, or sooner if a reclaim pass did not help. It clears on the next successful grow, or when free space recovers above twice the reserve floor. Isolated refusals never latch.

When latched: the `hg_shm_grow_blocked` statistic is 1, one warning is logged, and the `E_CORE_SHM_GROW_BLOCKED` event is raised at the next sweep tick - from the sweep timer, never under the arena lock - with `arena`, `committed_mb`, `cap_mb` and `grow_refused`. While it lasts the event is raised again periodically.

```
event_route[E_CORE_SHM_GROW_BLOCKED] {
    xlog("L_CRIT", "shm arena cannot grow: $param(committed_mb) of $param(cap_mb) MB committed, $param(grow_refused) refusals\n");
}
```

Refusal details are logged once per episode; `grow_refused` carries the count, and `last_grow_refusal` in `core:hg_stats` names what refused the latest grow: `cap`, `profile_ceiling`, `host_ram_floor`, `cgroup_limit`, `memlock_limit`, `hugetlb_pool`, `mlock_failed`, `hole_repin_failed`, `grow_wait_timeout` or `dry_run` (`none` until the first refusal). It stays after growth works again; `grow_blocked` says whether it is current.

### 3.4 Shrink

**Only whole free pages are released.** A page is released only when the buddy has merged it back into one free top-level block - which means no cell in it is in use or sitting in any process's private cache. No coordination with other processes is needed: nothing can hold a pointer into it.

**The drain.** The shrink decision is made per policy cycle. With a profile, the arena starts draining after usage has stayed at or below the down-threshold for the profile's C consecutive cycles; a profile without a `scale down` clause never shrinks. Without a profile, an elastic arena drains after 4 quiet sweeps (2 minutes) in which its free space exceeds one grow step plus four times the reserve floor. A shared arena then releases up to `hg_shrink_step` (default 64 MB) per maintenance tick from the top of the arena, and with `hg_interior_release = 1` up to another `hg_shrink_step` from inside it; a private arena releases one step per quiet window. The gates are re-checked every tick, and the drain stops at the headroom line, so a release never triggers the grow that would undo it. Any grow ends the drain, and shrink counting resumes only after a cool-off of 10 x C cycles (C = the profile's scale-down cycle count).

**The release.** Pages leave the free list first, under the lock; the release itself (`MADV_REMOVE` on the shared arena, `MADV_DONTNEED` on a private one) runs with the lock dropped; if the kernel refuses, the pages go straight back. On tier 1 released pages return to the hugetlb pool. A kernel that does not support the advice leaves the arena grown and is noted once; no further release is attempted.

## 4. Private (pkg) arenas

- Each process creates its own private arena at fork, after the configuration is parsed, so `-M INIT:CAP` and `pkg_auto_scaling_profile` apply to every process individually. The pre-fork parent arena never uses hugetlb, so no child can fault on the pool through a copy-on-write page, and never gets the profile; it keeps the `-M` cap and can still grow on exhaustion.
- A process grows and shrinks only its own arena; its policy, and its GROW-BLOCKED latch, run in its own 30 s sweep, whatever `hg_scaling_cycle` is set to. Each process publishes its refusals and GROW-BLOCKED state at that sweep, summed as `hg_pkg_grow_refused` and `hg_pkg_grow_blocked`.
- Host cost multiplies with the process count - and on tier 1, so does the pool reservation, since each process's arena reserves its own cap.

## 5. Module arenas

A module can ask for its own HG-managed arena with `mem/mem_arena.h` - for a cache whose size and lifetime have nothing to do with transactions. It gets the same size classes, caches, reclaim and elastic growth, independent of the core `-a` choice, and appears in `core:hg_stats` (not in the `hgmem:` statistics, which describe the core shm arena). When HG_MALLOC is not built, the API compiles to stubs that return NULL, so the module must fall back to ordinary shm itself.

## 6. Monitoring

**MI:** `core:hg_stats` returns, per arena (the shared arena, every module arena, and the calling process's private one):

| field | meaning |
|---|---|
| `tier`, `tier_bytes` | backing achieved at startup; per-tier split when growth mixed tiers |
| `committed`, `cap`, `grow_headroom` | elastic state |
| `pinned_mb`, `unpinned` | memory locked in RAM; `unpinned` is 1 when the arena runs swappable because init could not lock it or the memory-lock limit could not cover a grow |
| `grows`, `grows_proactive`, `grows_exhaustion`, `grow_refused`, `last_grow_refusal`, `grow_blocked` | growth history, what refused the latest grow (section 3.3) and the latched gauge |
| `shrinks`, `punches`, `draining`, `whole_free_pages` | release history and what could be released now |
| `live_committed`, per-class carve and reuse | what is in use and where |
| `lock` | hold and wait histograms per reason (refill, return, flush, large, policy, stats, other), stalls, the worst hold and its reason, commit and release times |
| `corruption` | double frees, foreign pointers, bad classes, damaged headers and - under HG_MALLOC_DBG - writes past the request and after free, counted per kind |

**Statistics:** `hg_shm_*` counters in the `hgmem:` group (growth, refusals, the GROW-BLOCKED gauge), plus `hg_pkg_grow_refused` and `hg_pkg_grow_blocked` summed over every process's private arena, suitable for Prometheus.

**Events:** `E_CORE_SHM_GROW_BLOCKED` (section 3.3) and `E_CORE_HG_LOCK_STALL`, raised for a shared-arena lock hold at or above `hg_lock_stall_us` (default 1000 us), with `reason`, `hold_us`, `process` and `stalls`.

**Note on `shmem:free_size`.** It counts only memory not yet carved into size-class blocks. Free cells already carved - in private caches or the shared pool - are reusable for their class but not counted as free, so free memory reads lower than what is reusable. Alert on `grow_blocked` and on the lock statistics, not on `free_size`.

## 7. Sizing

1. **Memory lock limit:** `LimitMEMLOCK=infinity` in the service unit (or `ulimit -l unlimited`). Required for an elastic arena on hugetlb pages; recommended on the other tiers, where without it the arena runs unpinned and can be swapped.
2. **Tier 1 pool:** reserve `shm_cap + pkg_cap x processes`, in 2 MB pages, plus a margin of about 12%. A pool that cannot fit the caps gives fixed arenas on huge pages, or pushes late processes to THP.
   **Tiers 2-3 instead of a pool:** THP `enabled` = `madvise` or `always` (private arenas) and `shmem_enabled` = `advise` or `always` (shared arena). With neither a pool nor THP, every arena runs on 4 KB pages.
3. **Private caps multiply:** `-M 16:64` on 30 processes is up to 1.9 GB of potential pinned growth.
4. **Tier 1 releases go back to the pool,** not to general memory; shrink `vm.nr_hugepages` to give them to the host.
5. **Measuring the pool:** `HugePages_Free` alone also moves with private hugetlb use; the shared arena's pages are `(HugePages_Total - HugePages_Free) - sum of Private_Hugetlb over all processes`.
6. **Memory per object:** a little more than the other allocators, from size-class rounding - +2% live memory on 400k registrations (usrloc insert-only, measured in September 2026 on an earlier HG_MALLOC build), 5-13% on tm's 1-4 KB objects at rates with no drops (setup A). Under overload the total is lower, because nothing waits in memory for retransmissions: see [memory under load](benchmarks.md#memory-under-load). `used_size` counts each object at its class size, so it reads higher than the bytes requested.

## 8. Troubleshooting

| symptom | cause | fix |
|---|---|---|
| startup: `RLIMIT_MEMLOCK is N KB, but growing on hugetlb pages needs M KB locked` | memory lock limit too low for growth on hugetlb pages | `LimitMEMLOCK=infinity` in the unit |
| `cannot grow ...: RLIMIT_MEMLOCK is N KB` | the same, at grow time | the same |
| `hugetlb pool cannot back a ... cap` at startup | `vm.nr_hugepages` smaller than the caps | grow the pool (section 7), restart |
| startup: `below the 512 KB minimum viable arena` | a fixed arena (no cap) under 512 KB | give a cap (`-M 256k:2m`) or INIT of at least 512 KB |
| startup: `on huge pages INIT is at least one huge page ... no room to grow` | INIT under 2 MB on huge pages with a cap of 2 MB or less | raise the cap, or give INIT as 2 MB for a fixed arena |
| notice: `INIT raised from ... to 2 MB` | INIT under 2 MB on huge pages | expected; give INIT as 2 MB to silence it |
| startup: `the N processes started now need ... MB of RAM` | `-M` INIT x processes does not fit in RAM above the floor (plus the hugetlb pool, if any) | lower `-M` INIT or the worker counts, or add memory or huge pages |
| `not forking more processes until memory frees up` | the auto-scaler wants a process whose INIT does not fit | lower the scale-up maximum or `-M` INIT; OpenSIPS keeps running meanwhile |
| `no more HG_MALLOC arena memory ...`, then `N allocations failed in the last 30 s` | the arena is full and cannot grow (cap reached, or growth refused) | raise the cap or the host's free memory; `core:hg_stats` shows what fills it. The first failure of an episode is logged in full, the rest are summarised every 30 s |
| `the arena has no growth room` | a profile is attached but `-m`/`-M` has no cap | add the cap: `-m 128:1024` |
| `scale-up target ... exceeds the ... reservation` | the profile wants more than the cap | raise the cap or lower the target |
| startup: `RLIMIT_MEMLOCK is N KB, less than the ... cap`, `continuing unpinned`, or `cannot pin a ... grow` | memory lock limit too low on tiers 2-4 | `LimitMEMLOCK=infinity` to keep the arena pinned; meanwhile it grows unpinned (swappable) rather than refusing |
| grown pages on 4 KB on a THP host | each step negotiates its own backing | expected; see `tier_bytes`, or use tier 1 |
| never shrinks | usage above the down-threshold, inside the post-grow cool-off, a profile without `scale down`, or the top page in use | check `core:hg_stats`; enable `hg_interior_release` to release from inside the arena |
| `DRY RUN - would grow` and allocations fail past the initial size | `hg_autoscale_dry_run = 1` blocks all growth | set 0 to act |
| testing under `ulimit -l` shows no refusals | running as root: `CAP_IPC_LOCK` lifts the limit | test as an unprivileged user (`setpriv`) |

## 9. Testing

`modules/hgstress` (excluded from the default build; `include_modules= hgstress`) is a multi-process soak: every worker churns stamped cells across all classes and the large tier, verifies the stamps while the others churn, and reports `PASS`/`FAIL` per worker. Its MI commands park and release stamped memory (`hgstress:hgs_hold`, `hgstress:hgs_release`, to drive grow and shrink) and inject a double free or a scribbled link (`hgstress:hgs_fault`, to exercise the integrity checks). It runs under any `-a` allocator.

The unit tests run under HG_MALLOC with `make test UT_ALLOC=HG_MALLOC UT_MEM="-m 64:1024 -M 16:256"` (`UT_ALLOC=HG_MALLOC_DBG` for the debug variant): the core suites, the allocator stress suite (`mem/test/test_malloc.c`, enabled when HG serves shm and pkg) and the HG cases in `mem/test/test_hg_malloc.c` - pkg memory from before `fork()` realloc'ed and freed in a child, cells freed by another process, the large tier, growth past INIT, a process leaving for good, double frees, a damaged header, a pointer into a cell, a realloc of freed memory, and - under HG_MALLOC_DBG - writes past the request and after free. CI runs both variants on every compiler and architecture of the unit-test matrix.

## 10. For reviewers

### Reading order

| file | what it holds |
|---|---|
| `mem/hg_malloc.h` | the interface to the core: init, status, statistics, the ownership tests, the lock wrappers; it includes the two below |
| `mem/hg_cell.h` | the cell header and its states, the size classes, rounding |
| `mem/hg_block.h` | the data structures: `struct hg_block` (one arena), blocks, the per-thread caches, stats slots, corruption kinds, lock instrumentation |
| `mem/hg_malloc_dyn.h` | the public entry points (`hg_malloc`/`hg_free`/`hg_realloc`, debug and normal) |
| `mem/hg_arena.c` | the fast path: per-thread caches, cell alloc and free, carving blocks into cells; arena init and per-child reset; flushing this thread's caches; the live-cell walk |
| `mem/hg_pool.c` | the shared per-class pool and its integrity checks, block fullness lists, drained-block reclaim |
| `mem/hg_cache.c` | flushing cached cells back to the pool, the sweep's due check, the reserve floor |
| `mem/hg_stats.c` | statistics, per-thread stats slots, the core statistics; the lock contract check and stall notes |
| `mem/hg_arena_int.h` | helpers shared by the four files above |
| `mem/hg_buddy.c` | the page grid and the buddy allocator |
| `mem/hg_grow.c` | growth: the three-phase grow, re-growth into holes, deferred collapse, the GROW-BLOCKED latch |
| `mem/hg_shrink.c` | shrinking: trim from the top, interior release, the shrink and drain ticks |
| `mem/hg_buddy_int.h` | helpers shared by the three files above |
| `mem/hg_large.c` | requests above 64 KB: a coalescing tier carved from buddy blocks |
| `mem/hg_backing.c` | backing memory: the four-tier ladder and its verification, commit, release, re-pin, memlock diagnosis |
| `mem/hg_capacity.c` | host and cgroup memory room, capacity checks at init, fork and grow, the pkg growth figures |
| `mem/hg_backing.h` | helpers shared by `hg_malloc.c`, `hg_backing.c` and `hg_capacity.c` |
| `mem/hg_malloc.c` | init and per-child init, the arena registry, auto-scaling profiles, status and info, allocation-failure reports |
| `mem/hg_maint.c` | the maintenance process |
| `mem/hg_memlimit.c`, `mem/hg_dbg.c`, `mem/hg_san.h` | cgroup limits, the debug checks, sanitizer hooks |
| integration | `mem/mem.c`, `mem/shm_mem.c`, `mem/common.c`, `mem/rpm_mem.c`, `pt.c`, `pt_scaling.c`, `main.c`, `cfg.y`, `core_stats.c`, `mi/mi_core.c`, `evi/evi_core.c` |
| tests | `mem/test/test_hg_malloc.c`, `modules/hgstress` |

### Invariants

1. **One lock per arena.** `hb->lock` (a `gen_lock_t` on its own cache line) guards all shared state of an arena: pools, block lists, the buddy tree, the large tier. No path holds two arena locks, the lock is never re-entered, and the slow system calls - commit, release, re-pin, collapse - run with it released.
2. **The common path takes no lock.** Each thread keeps, per class, a private free list and a bump region. A hit touches only thread-local state, the cell's header word and link word, fields fixed at init, and the process's statistics slot. The lock is taken to refill (32 cells), to give back half of a full cache, or to carve a block.
3. **Ownership is by address.** A pointer belongs to the arena whose reserved range holds it (`hg_owns()`, then a registry of at most 8 arenas inherited across `fork()` - `hg_owner()`); its class is one header byte; its block is computed from the address only when the cell enters the shared pool. A pointer of another arena - pre-fork pkg memory in a child, a module arena - bypasses the private cache and goes to its owner's pool.
4. **A block is reused only when every cell is back.** A block returns to the buddy, and can be re-cut for another class or the large tier, only when all its cells are in the shared pool (`in_gpool == cells`). A cell in any thread's private cache, bump region or quarantine still counts as live, so caching can delay a reclaim but never cause a premature one.
5. **`fork()` never shares private state.** Before allocating anything, a child discards the private cache state it inherited for the shared arenas - otherwise parent and child would hand out the same cells - and gets a pkg arena of its own; pre-fork pkg pointers stay valid and are freed into the child's copy of the inherited arena (`hg_arena_child_init()`, `pt.c`).
6. **One range operation per arena at a time.** Grow, re-grow, trim and punch take a single in-flight token (`grow_inflight`) under the lock. New pages are published only after they are committed; pages to release are first taken off the free lists and marked `releasing`; a page is released only when it is wholly free; the arena never shrinks below INIT. A grower that finds the token taken waits without the lock and retries only if a grow published pages (`grow_gen`).
7. **Host memory is checked before it is committed.** Every arena's INIT at creation; all startup processes before the first fork (`hg_capacity_check()`); each child's INIT before its fork - forks are serialized; every grow, under the arena lock, with a pkg grow charged for every process (`hg_grow_ram_refused()`). Concurrent pkg grows therefore cannot together cross the floor. Tier 1 skips the RAM check: its pool pages are reserved at creation.
8. **The maintenance process owns proactive work** - headroom grows, the profile and shrink gates, deferred collapse - and a worker that runs out still grows the arena itself.
9. **A cell's header says whether a caller holds it.** Fresh from the carve, live from an allocation, free from a free - by any process; those are the only transitions. Moving a cell between a private cache, the shared pool, the quarantine or the exit drain is not one. A small cell goes from live to free in one compare-and-swap of its header word, a large fragment under the arena lock, so of two frees of one cell exactly one takes effect; an allocation takes only a fresh or free cell (`hg_hdr_claim_free()`, `free_check()`, `hg_large_free()`).

### Known limits

- A stale double free - a second free that arrives after the cell was handed out again - frees the new owner's cell: the header says live, as it would for a legitimate free, and a plain pointer carries nothing that tells the two allocations apart (a generation in the header alone would not help, the stale pointer reaches the same header). Under HG_MALLOC_DBG the quarantine delays the reuse, so most such frees land while the cell is still free and are refused; it narrows the window, it does not close it.
- The header checksum is 16 bits: a random overwrite of a header passes it once in 65,536. Writes past the request and after a free are found only under HG_MALLOC_DBG - past the request only within the cell's own slack (a write that reaches the next cell is found by that cell's header), after a free only while the cell is in quarantine - not once it has left it, not for the quarantine of a process that crashed, and not for a free that bypasses the private cache (a cell of another arena, or a process with no private slot), which goes straight to the shared pool.
- A process that crashes keeps its cached cells counted as live until restart; one that exits normally or is scaled down hands them back.
- The floor checks are not reservations: independent arenas (shm, module arenas, a child's pkg INIT) growing at the same moment can together pass the floor by one step each.
- If the maintenance process exits, OpenSIPS shuts down. If it hung in the middle of a grow, a worker that runs out would wait 2 s and fail its allocation.
- TCP main's I/O threads share one statistics slot, so their counters can drift. `E_CORE_HG_LOCK_STALL` is raised for the core shm arena only.

### Properties and the tests that show them

| property | shown by | runs | gap |
|---|---|---|---|
| a cell is never handed to two processes at once | `hgstress`: every worker stamps each word of its blocks and re-verifies them; a foreign stamp is a double hand-out | by hand | not in CI |
| alloc / free / realloc under load, many processes, mixed sizes | the allocator stress suite under HG_MALLOC and HG_MALLOC_DBG | CI | checks alignment and the used-size statistic, not content |
| a cell freed by another process comes back intact | `test_hg_malloc.c` | CI | |
| pkg memory from before `fork()` can be realloc'ed and freed in a child | `test_hg_malloc.c`, normal and debug builds | CI | |
| a process leaving for good gives its cells back | `test_hg_malloc.c`: a child leaves through `dynamic_process_final_exit()` and the arena returns to its baseline | CI | |
| a double free is refused and counted, not handed out twice, with or without frees in between | `test_hg_malloc.c` (twice in a row; with a free in between; large fragment; realloc of freed memory); `hgstress` fault injection | CI; by hand | |
| a damaged header or a pointer into a cell is refused, not followed | `test_hg_malloc.c` | CI | |
| two frees of one cell at the same moment: exactly one takes effect | - | - | by construction (one compare-and-swap; the large tier's re-check under its lock); no test can force the interleaving |
| HG_MALLOC_DBG: a write past the request (small cell, large fragment, realloc in place) and a write after free are found | `test_hg_malloc.c`, HG_MALLOC_DBG run | CI | |
| alloc, free and realloc against an independent model: no address handed out twice, every block keeps its bytes, realloc keeps the prefix, the arena's counters follow the model; a capped arena fails at its cap and serves again | `test_hg_fuzz.c`: 3 seeds of 20,000 steps on shm, one capped module arena; `HG_FUZZ_SEED` / `HG_FUZZ_OPS` replay a failure | CI | single process; cross-process frees are in the other tests |
| ASan sees pkg cells: open for the bytes asked for, closed after them and once freed; shm cells left alone | `test_hg_malloc.c` in an ASan build | fork CI | the upstream workflows have no sanitizer job |
| large tier (> 64 KB) | `test_hg_malloc.c` (128 KB - 8 MB, realloc), `hgstress` (1 in 64 allocations) | CI; by hand | |
| an elastic arena grows past INIT on demand | `test_hg_malloc.c` (one process); `hgstress hold_mb` (every worker, growth seen by all) | CI; by hand | multi-process growth not in CI |
| the debug status dump walks only valid cells | the HG_MALLOC_DBG unit-test run ends with the shm status dump | CI | |
| shrink and interior release never free memory still in use | - | by hand, production | no automated test |
| backing ladder: hugetlb, THP advice, THP collapse, 4 KB | startup log, `core:hg_stats` | by hand | not in CI (runners have no hugetlb pool) |
| memlock limit: unpinned growth on 4 KB / THP, refusal on hugetlb | - | by hand | not in CI |
| the host floor and the memory cgroup's limit: the default floor, the cgroup's room read from fake cgroup v1 and v2 trees (a limit on an ancestor, on the process's own level, above 4 GB), host memory above 4 GB counted whole on a 32-bit build, a grow refused at the floor | `test_hg_memlimit.c` | CI | |
| growth refused at the cap or the host floor returns NULL and raises the event | - | - | no automated test |
| builds: gcc and clang `-Werror`, x86-64, i386, arm32/arm64, glibc and musl, HG compiled out | upstream CI matrix; i386 and musl by hand | CI; by hand | |
