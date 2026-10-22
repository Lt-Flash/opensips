# Configuration

## Command line

| option | meaning |
|---|---|
| `-a HG_MALLOC` | use HG_MALLOC for shared and private memory (`-s` / `-k` select them separately) |
| `-a HG_MALLOC_DBG` | the same, recording file, function and line for every allocation, which `mem:pkg_dump` and `mem:shm_dump` print per site; it also puts a red zone after every request, checked at free and realloc, and holds freed cells poisoned in a per-thread quarantine, checked when they leave it - see [Integrity checks](design.md#1-allocation) |
| `-m INIT[:CAP]` | shared arena: initial size, and the cap it may grow to |
| `-M INIT[:CAP]` | private (per-process) arena, same form |

Restart-persistent memory (`-r`) is not served by HG_MALLOC: it lives in a file mapped at a fixed address, and HG_MALLOC reserves its own arenas. With `-a HG_MALLOC` it uses F_MALLOC (F_MALLOC_DBG with `-a HG_MALLOC_DBG`); `-r HG_MALLOC` is refused at startup.

Sizes are MB, or take a `k`/`m`/`g` suffix (either case): `-m 128:1g`, `-M 512k:16m`. Without a cap the arena is fixed.

The smallest INIT depends on the backing the arena gets:

- **Huge pages:** an arena holds at least one whole huge page (2 MB on x86-64). A smaller INIT is raised to it, with a notice. If the cap leaves no room above that page (`-M 1m:2m`), OpenSIPS refuses to start.
- **Plain 4 KB pages:** the arena runs on 256 KB pages. INIT can be 256 KB when a cap gives it room to grow (`-M 256k:2m`), 512 KB without one.

These are the allocator's limits, not OpenSIPS's needs: the shared arena must also fit core startup, which makes one allocation of about 540 KB, so give `-m` a cap of 2 MB or more. With a cap, an allocation that would fail commits one more grow step and retries, up to the cap. The cap is on the command line because the reservation is made before the configuration is parsed.

## Memory autoscaling profiles

The worker autoscaler's `auto_scaling_profile` grammar also sizes memory arenas. For an arena the numbers are MB (or k/m/g):

```
auto_scaling_profile = MEM_SHM
    scale up to 1024 on 80% for 3 cycles within 10
    scale down to 256 on 30% for 120 cycles

auto_scaling_profile = MEM_PKG
    scale up to 32 on 80% for 2 cycles within 5
    scale down to 8 on 20% for 60 cycles

shm_auto_scaling_profile = MEM_SHM
pkg_auto_scaling_profile = MEM_PKG
```

| element | meaning for an arena |
|---|---|
| `up to N` | growth ceiling: above the initial size and within the `-m`/`-M` cap |
| `on P% for C cycles within W` | usage is sampled once per cycle; at the end of each block of W cycles, if it was at or above P% of the committed arena in at least C of them, one step is grown |
| `down to M` | shrink floor - may be below the initial size, must be below `up to N` |
| `on Q% for C cycles` | release memory after C consecutive cycles at or below Q%; after any grow, shrink counting waits 10 x C cycles |

Usage is live usage - what is handed out - not the carved footprint. A shared-arena cycle is `hg_scaling_cycle` seconds; a pkg profile runs in each process's 30 s cache sweep and governs every process's private arena separately, so its numbers are per process. A profile without a `scale down` clause never shrinks the arena; an elastic arena with no profile at all still releases memory after 2 quiet minutes. A profile written with size suffixes, or with a target of 1000 or more, is refused if it is attached to a process group, so a memory profile cannot be mistaken for a worker count.

Choosing the numbers: `P` is the occupancy you want the arena to run at (70-80% is the usual shape) and `Q` should sit well below it (20-40%), so the band between them absorbs normal swings without growing and shrinking back and forth.

## Global parameters

All optional.

| parameter | default | meaning |
|---|---|---|
| `shm_auto_scaling_profile` | none | profile that drives the shared arena |
| `pkg_auto_scaling_profile` | none | profile that drives each process's private arena |
| `shm_grow_granule` | 16m (one page on a small arena) | size of one grow step of the shared arena, rounded to the arena page |
| `pkg_grow_granule` | 16m (one page on a small arena) | the same for every process's private arena |
| `hg_ram_floor_mb` | 0 = MemTotal/20, at least the smaller of 256 MB and MemTotal/8 (16 MB on a 128 MB host, 128 MB on 1 GB, 256 MB from 2 GB to 5 GB) | host free memory no commit may take the host below: checked at startup, before each new process is forked, and on every grow. The two arenas created before the config is read (the shared arena and the first process's private arena) are checked against the default |
| `hg_autoscale_dry_run` | 0 | 1 = advise only: no growth of any kind and no shrink; logs what it would have done once per episode and counts suppressed grows in `hg_shm_grow_refused`. Allocations beyond the initial size fail |
| `hg_grow_ahead` | 1 | keep free headroom by growing one step before an allocation has to |
| `hg_scaling_cycle` | 30 | seconds per policy cycle of shared arenas (the profile's "cycles"); private arenas use the fixed 30 s sweep |
| `hg_shrink_step` | 64m | most a shared arena releases from its top per maintenance tick (plus up to as much again from inside with `hg_interior_release`); a private arena releases one step per quiet window |
| `hg_interior_release` | 0 | 1 = give back whole free pages from anywhere in the arena, not only the top |
| `hg_lock_stall_us` | 1000 | an arena-lock hold at or above this is a stall: counted per arena, and raises `E_CORE_HG_LOCK_STALL` for the shared arena; 0 disables |

Example:

```
shm_auto_scaling_profile = MEM_SHM
shm_grow_granule = 32m
hg_ram_floor_mb = 1024
hg_lock_stall_us = 2000
```

## Events

Both events describe the shared arena and are raised from the 30 s cache-sweep timer, never while the arena lock is held. Catch them in the script with `event_route` or send them out with any event transport (`event_datagram`, `event_stream`, `event_rabbitmq`, ...).

### E_CORE_SHM_GROW_BLOCKED

The shared arena needs to grow and a resource refuses: on any tier the host floor (`hg_ram_floor_mb`) or the kernel failing to back the memory, and on hugetlb pages also the memory-lock limit or the pool. Reaching the `-m` cap or the profile's scale-up target does not raise it - that is logged once as a notice and counted in `hg_shm_grow_refused`. A single refusal does not raise it either: the first refusal arms the state, which latches at the next 30 s sweep tick if refusals continued (or sooner if a reclaim pass did not help). The event is raised at the sweep tick after the state latches and again periodically while it lasts. A successful grow, or free space recovering above twice the reserve floor, clears it.

| parameter | meaning |
|---|---|
| `arena` | `shm` |
| `committed_mb` | memory the arena holds now |
| `cap_mb` | the `-m` cap - committed is below it, since a full arena does not raise this event |
| `grow_refused` | refused grows so far |

What to do: free or add host memory, or on hugetlb pages raise the memory-lock limit or the pool - whichever the log names - or find what is filling the arena (`core:hg_stats`, `core:hg_advise`). With no subscriber the same moment is logged as a warning, and the `hgmem:hg_shm_grow_blocked` statistic reads 1 while it lasts.

### E_CORE_HG_LOCK_STALL

A process held the shared arena's lock for `hg_lock_stall_us` (default 1000 us) or longer. Raised at most once per 30 s, carrying the most recent stall; `hg_lock_stall_us = 0` turns it off.

| parameter | meaning |
|---|---|
| `arena` | `shm` |
| `reason` | what the lock was held for: `refill` (taking cells or a new block), `return` (giving cells or a block back), `flush` (a process emptying its private cache), `large` (an allocation too big for the 64 KB class), `policy` (a grow or shrink decision), `stats` (a statistics walk), `other` |
| `hold_us` | how long it was held |
| `process` | OpenSIPS process number that held it, as listed by `opensips-cli -x mi ps` |
| `stalls` | stalls since startup |

What to do: an occasional stall during a growth spurt is normal. A steady stream, or one `reason` dominating, is worth a look at the per-reason hold histogram in `core:hg_stats`.

### Examples

In the script:

```
loadmodule "event_route.so"

event_route[E_CORE_SHM_GROW_BLOCKED] {
    xlog("L_CRIT", "HG: shm arena cannot grow - $param(committed_mb) of $param(cap_mb) MB committed, $param(grow_refused) refusals\n");
}

event_route[E_CORE_HG_LOCK_STALL] {
    xlog("L_WARN", "HG: shm lock held $param(hold_us) us by process $param(process) for $param(reason), $param(stalls) stalls so far\n");
}
```

which logs, for example:

```
CRITICAL:HG: shm arena cannot grow - 896 of 1024 MB committed, 37 refusals
WARNING:HG: shm lock held 1262 us by process 9 for return, 2 stalls so far
```

To another system, with `event_datagram` loaded:

```bash
opensips-cli -x mi event_subscribe E_CORE_SHM_GROW_BLOCKED udp:10.0.0.10:9999
```

## Monitoring

```bash
opensips-cli -x mi core:hg_stats      # per arena: tier, committed and cap, grows and shrinks, lock holds per reason, corruption counters
opensips-cli -x mi core:hg_advise     # what -m and -M should be, from the peak each arena actually reached
opensips-cli -x mi statistics:get hgmem:
```

`core:hg_stats` answers for the process that receives the MI command, so its `pkg` section is that process's private arena; `core:hg_advise` covers the worst process.

The `hgmem:` statistics group holds the shared arena's figures, plus two that sum every process's private arena (each process publishes its own at its 30 s cache sweep). The ones worth alerting on:

| statistic | alert when |
|---|---|
| `hg_shm_grow_blocked` | 1 - growth refused for a sustained period |
| `hg_pkg_grow_blocked` | above 0 - that many processes' private arenas cannot grow (no event for these: alert on this) |
| `hg_pkg_grow_refused` | rising - private-arena grows refused, all processes together |
| `hg_shm_corruption` | above 0 - memory was corrupted and the allocator caught it |
| `hg_shm_repin_failed` | above 0 - memory taken back from the host could not be pinned again |
| `hg_shm_lock_stalls` | rising steadily |
| `hg_shm_committed` against `hg_shm_cap` | committed close to the cap |

Others for graphs: `hg_shm_grows`, `hg_shm_grows_proactive`, `hg_shm_grows_exhaustion`, `hg_shm_grow_refused`, `hg_shm_shrinks`, `hg_shm_shrink_bytes`, `hg_shm_live`, `hg_shm_live_peak`, `hg_shm_carved`, `hg_shm_free_to_carve`, `hg_shm_lock_hold_max_us`, `hg_shm_tier`.

Note that `shmem:free_size` counts only memory not yet carved into size-class blocks; free cells already carved are reusable for their class but not counted, so it reads lower than what is reusable. Alert on the figures above, not on `free_size`.

## Host requirements

- `LimitMEMLOCK=infinity` in the service unit, or `ulimit -l unlimited`, so the arena stays pinned. Required for an elastic arena on hugetlb pages: every grow there is an `mlock()`, which checks the limit on hugetlb mappings too, and a refused grow names the limit or the hugetlb pool, whichever ran out. Recommended on tiers 2-4: without it the arena runs and grows unpinned (swappable) instead of refusing. HG warns at startup when the limit is too low.
- Huge pages are optional; without them every arena runs on plain 4 KB pages. To use them, provide either or both of:
  - a hugetlb pool (tier 1): `vm.nr_hugepages` large enough for the shared cap plus the private arenas, in huge pages (2 MB on x86-64). On this tier the whole cap is reserved from the pool when the arena is created. A pool that can hold the initial size but not the cap gives a fixed arena on huge pages instead of an elastic one, and says so; a pool that cannot hold even the initial size leaves the arena on THP, still elastic.
  - transparent huge pages (tiers 2-3): `/sys/kernel/mm/transparent_hugepage/enabled` set to `madvise` or `always` for the private arenas, and `/sys/kernel/mm/transparent_hugepage/shmem_enabled` set to `advise` or `always` for the shared arena. Many distributions ship `shmem_enabled=never`. `MADV_COLLAPSE` (tier 3) needs Linux 6.1 or later.

## Host memory checks

The shared arena commits its `-m` INIT at startup, and every process then creates its own private arena and commits its `-M` INIT, so the number of processes matters as much as the sizes. HG checks before it commits, on every tier: THP and 4 KB memory against `MemAvailable` above `hg_ram_floor_mb`, and memory on hugetlb pages against the pool. A process in a memory-limited cgroup - a container started with a memory limit, a systemd unit with `MemoryMax` - is also held to its cgroup: the tightest limit on its own cgroup or any ancestor, less what that cgroup holds (its inactive file cache counted as free, as `MemAvailable` counts it on the host), must stay above the same floor computed on that limit (cgroup v1 and v2). `/proc/meminfo` shows the whole host inside such a container, so without this an arena would grow until the cgroup's OOM killer stepped in. The shared arena is checked first (against the default floor - the config is not read yet), and every later check counts what is left after it.

- **Shared arena:** its INIT is refused if the host cannot hold it; its growth is refused at the floor (GROW-BLOCKED, `hg_shm_grow_blocked`).
- **Private arenas at startup, before anything forks:** every process's private arena is simulated against RAM above the floor, after whatever the hugetlb pool (if the host has one) can take. OpenSIPS refuses to start if the processes started at startup do not fit, and warns if the auto-scaling maximum does not, or if the `-m` cap plus `-M` cap x processes exceeds `MemTotal`. An INFO line gives the numbers.
- **Each new process, auto-scaler forks included:** not forked if its INIT does not fit. The auto-scaler keeps the processes it has and tries again on its next cycle, so a profile that allows more processes than the host holds stops short instead of running into the OOM killer.
- **Every arena:** an INIT the host cannot hold is refused when the arena is created.
- **Every grow:** refused if it would cross the floor; a private-arena step is counted once per process. Private-arena refusals show in `hg_pkg_grow_refused` and `hg_pkg_grow_blocked`.

```
INFO:  pkg: 14 processes at startup, up to 16 with auto-scaling, -M INIT 32 MB: 192 MB from the hugetlb pool, 224 MB (288 MB at the maximum) from RAM; 0 MB of RAM available above the 11778 MB floor
ERROR: pkg: the 14 processes started now need 224 MB of RAM for their -M INIT of 32 MB (after 192 MB from the hugetlb pool), but 0 MB is available above the 11778 MB floor. ...
ERROR: new process's pkg arena: committing 32 MB now does not fit - ... not forking more processes until memory frees up
```

More detail - every tier, the growth and shrink mechanics, deployment recipes, troubleshooting: [design.md](design.md).
