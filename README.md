# HG_MALLOC

A memory allocator for [OpenSIPS](https://github.com/OpenSIPS/opensips), for both shared and private memory, selected at runtime with `-a HG_MALLOC`. Built for busy SIP proxies, registrars and SBCs, where the allocator sits on every message.

## Why

- **Less CPU:** 9-27% less than F_MALLOC, F_PARALLEL_MALLOC and HP_MALLOC for the whole server at 20k-60k INVITEs per second answered by tm, and about half on a registrar taking 60k registrations per second.
- **Over TLS and WSS too:** at the same rate of calls relayed by a stateful proxy, 7-11% less CPU per call (16-23% less in the TCP workers) and 54-69% fewer `sched_yield` calls than F_MALLOC, with HG_MALLOC's shared arena on plain 4 KB pages like F_MALLOC's; over UDP with the default 512 KB socket buffer the highest sustained rate rises from 10k to up to 22k relayed calls per second.
- **No drops under load:** with 8 MB socket buffers, 0 UDP drops and 0 failed INVITEs at 60k INVITEs/s, where the other allocators drop thousands to millions of datagrams and, with a 2 GB arena, F_MALLOC and F_PARALLEL_MALLOC run out of memory; and 0 drops up to 16k relayed calls/s, where F_MALLOC already drops at 12k. The workers keep the socket drained.
- **Less memory under overload:** with no drops, transactions are released on time instead of waiting in memory for retransmissions - at 60k INVITEs/s HG_MALLOC peaked at 1.5 GB against 2.0-2.5 GB for HP_MALLOC and F_MALLOC, and stayed within a 2 GB arena that F_MALLOC and F_PARALLEL_MALLOC ran out of.
- **Tight latency:** 1.2 million registrations at 60k per second with a p99 of 4 ms, against 15-28 s for the others.
- **No more guessing `-m`:** `-m 128:1024` starts at 128 MB, grows as traffic arrives, and gives memory back when it goes quiet.
- **Safe and observable:** every cell header carries the cell's state and a checksum, so a double free is refused whatever came between the two frees, two processes freeing one cell at the same moment free it once, a cell is never handed out twice, and a damaged header, a pointer into a cell or a corrupted free-list link is caught and counted instead of followed. `-a HG_MALLOC_DBG` also finds writes past the request and after free; an ASan or Valgrind build sees inside the private arenas. Every grow, shrink and lock hold is visible through MI, statistics and events.
- **Runs anywhere:** huge pages when the host has them, plain 4 KB pages when it does not; builds on x86 and ARM, 32-bit and 64-bit, glibc and musl.

Two benchmark setups, each running every allocator from one binary: INVITEs answered by tm on master plus HG_MALLOC (setup A, rates in INVITEs/s), and calls relayed by a stateful proxy over UDP, TLS and WSS on a 4.1 build with other pending patches (setup B, rates in relayed calls/s, 13 datagrams each). Their rates do not compare with each other - see [what was measured where](docs/benchmarks.md#what-was-measured-where). In production since August 2026, now on 19 SIP nodes: billing gateways and load balancers since August, SBCs and registrars since October.

## How it works

- **Lock-free common path.** 21 size classes from 64 B to 64 KB, served from a private free list per process: an allocation and the free that returns it take no lock and touch no shared cache line. Cells move between processes through a shared per-class pool, in batches.
- **Memory is reused, not stranded.** Blocks are carved from a buddy allocator over a grid of pages the size of the host's huge page (2 MB on x86-64), whichever tier backs them; a block whose cells are all free returns to the buddy and can be re-cut for any size class.
- **Four tiers of backing memory.** The arena is acquired through a four-tier ladder - `MAP_HUGETLB`, transparent huge pages by advice, by collapse, then plain 4 KB pages - each tier verified from the kernel's own counters. Huge pages are optional.
- **Elastic arenas.** `-m INIT:CAP` reserves the cap and commits pages only as needed; a maintenance process grows ahead of demand and releases quiet pages with the arena lock dropped. Growth stops at the cap, at what the backing tier can give, and at a floor of free memory - the host's, and the memory cgroup's in a container or a unit with `MemoryMax` - and when the backing or host memory keeps refusing, an alertable event is raised.
- **No OOM surprises.** Every arena's INIT - the shared `-m` and each process's `-M` - is checked against host memory before it is committed: OpenSIPS refuses a combination the host cannot hold at startup, and shared-arena growth and the auto-scaler both stop at a free-memory floor instead of running into the OOM killer - the host's (5% of RAM, at least 256 MB, or an eighth of RAM on a host under 2 GB), and in a container or a unit with `MemoryMax` the memory cgroup's, whose limit `/proc/meminfo` does not show.

The full design, with measurements behind each decision: [docs/design.md](docs/design.md).

## Code

Everything lives in the [Lt-Flash/opensips](https://github.com/Lt-Flash/opensips) fork:

| branch | what |
|---|---|
| [`feature/hg-malloc-v3-final`](https://github.com/Lt-Flash/opensips/tree/feature/hg-malloc-v3-final) | OpenSIPS master (as of 2026-10-09) plus the allocator, its review fixes, the header state and checksum, debug checks, sanitizer hooks, memory-cgroup limits, unit tests with a fuzzer, and a section for reviewers; proposed upstream as [OpenSIPS/opensips#4327](https://github.com/OpenSIPS/opensips/pull/4327) |
| [`feature/hg-malloc-v3-4.0`](https://github.com/Lt-Flash/opensips/tree/feature/hg-malloc-v3-4.0) | the same commits on the 4.0 branch; also applies to the 4.0.1 and 4.0.2 releases |
| [`hg_malloc`](https://github.com/Lt-Flash/opensips/tree/hg_malloc) | this documentation |

The branch adds the allocator (`mem/hg_*`), elastic-arena and autoscaling configuration, MI commands, statistics and events, and `modules/hgstress`, a soak and fault-injection test module excluded from the default build; it also fixes an out-of-bounds write in core `add_stats_group()` that HG_MALLOC_DBG found. Clone a branch and build it as any OpenSIPS tree, or add its commits to an existing tree:

```bash
git remote add lt-flash https://github.com/Lt-Flash/opensips.git

# a master tree
git fetch lt-flash feature/hg-malloc-v3-final
git cherry-pick 31d1c970dd^..FETCH_HEAD

# a 4.0 tree
git fetch lt-flash feature/hg-malloc-v3-4.0
git cherry-pick be2e5d9018^..FETCH_HEAD
```

In an unpacked 4.0.x release tarball (checked on 4.0.1 and 4.0.2), all 4.0 commits as one patch:

```bash
curl -L 'https://github.com/Lt-Flash/opensips/compare/be2e5d9018a982fc8439b7c6419256ab119f6d7f%5E...3753d1f1fcb90713020c20dae60e1560ccdfbdb2.patch' | patch -p1
```

## Quick start

Build: `HG_MALLOC` is in the `DEFS` of `Makefile.conf.template`; it is compiled in next to the other allocators and only used when selected. A tree that already has a `Makefile.conf` needs `DEFS+= -DHG_MALLOC` added to it.

```bash
# fixed 512 MB shared arena, 16 MB private arena per process
opensips -f opensips.cfg -a HG_MALLOC -m 512 -M 16

# elastic: start at 128 MB, allowed to grow to 1 GB; private arenas 16 -> 32 MB
opensips -f opensips.cfg -a HG_MALLOC -m 128:1024 -M 16:32
```

The arena is pinned so it cannot be swapped; give the service an unlimited memory-lock limit:

```ini
# systemd unit
[Service]
LimitMEMLOCK=infinity
```

With a lower limit, HG runs and grows unpinned on THP and 4 KB pages, but on hugetlb pages an elastic arena cannot grow past the limit. HG warns at startup, and under `opensips -C`, when the limit is too low.

**To use huge pages, enable them on the host first.** HG uses only what the host offers. With no hugetlb pool and transparent huge pages turned off, every arena drops through the ladder to plain 4 KB pages. It still works, but without the huge-page gain. Configure one or both:

```bash
# Tier 1 - hugetlb pool: shm cap + pkg cap x processes, in 2 MB pages, ~12% margin.
# e.g. -m 128:1024 -M 16:32 with 20 processes: (1024 + 32*20) / 2 = 832 pages, +12% = 932
sysctl -w vm.nr_hugepages=932            # persist in /etc/sysctl.d/; reserve early, before memory fragments

# Tiers 2-3 - transparent huge pages, used when there is no pool or it is too small
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled         # private arenas (-M)
echo advise  > /sys/kernel/mm/transparent_hugepage/shmem_enabled   # shared arena (-m)
```

Many distributions ship `shmem_enabled` as `never`, which puts the shared arena on 4 KB pages even when THP is on for private memory. `MADV_COLLAPSE` (tier 3) needs Linux 6.1 or later. The startup log names the tier each arena got, and, when THP is off by a host setting, why it fell to 4 KB pages; `core:hg_stats` shows the split in `tier_bytes` when growth landed on more than one tier.

Grow ahead of demand and give memory back with a profile - [configuration](docs/configuration.md):

```
auto_scaling_profile = MEM_SHM
    scale up to 1024 on 80% for 3 cycles within 10
    scale down to 256 on 30% for 120 cycles

shm_auto_scaling_profile = MEM_SHM
```

## Monitoring

```bash
opensips-cli -x mi core:hg_stats         # per arena: tier, committed/reserved, grows, lock holds, corruption
opensips-cli -x mi core:hg_advise        # what -m and -M should be, from the peaks actually reached
opensips-cli -x mi statistics:get hgmem:
```

Events: `E_CORE_SHM_GROW_BLOCKED` (growth refused for a sustained period), `E_CORE_HG_LOCK_STALL` (an arena-lock hold above `hg_lock_stall_us`). Parameters, examples and what to alert on: [configuration](docs/configuration.md#events).

## Costs

- Memory per object: a little more than the other allocators, from size-class rounding - +2% live memory on 400k registrations (usrloc insert-only, measured in September 2026 on an earlier HG_MALLOC build), 5-13% on tm's 1-4 KB objects at rates with no drops (setup A). Under overload the total is lower: see [memory under load](docs/benchmarks.md#memory-under-load).
- The integrity checks (header state and checksum, in every build): 0.1-2.6% more CPU for the whole server at 20k-60k INVITEs/s answered by tm (setup A, 3 runs per rate), and 0.8-2.3% per call at 12k-22k relayed calls/s over UDP (setup B, 4 runs per rate) - within the run-to-run spread at every rate but 60k INVITEs/s; drops and failed calls do not tell the two builds apart. The CPU figures above were measured before they were added: [cost of the integrity checks](docs/benchmarks.md#cost-of-the-integrity-checks).
- The committed arena is pinned, so it shows as resident from the start.
- The huge-page tiers and giving memory back to the OS are Linux-only; elsewhere the arena is plain pinned memory that grows but does not shrink.

## Status

Not yet in upstream OpenSIPS. Proposed as [OpenSIPS/opensips#4327](https://github.com/OpenSIPS/opensips/pull/4327); available as branches for master and 4.0 (see [Code](#code)). Licensed under the GNU GPL v2 or later, like OpenSIPS.
