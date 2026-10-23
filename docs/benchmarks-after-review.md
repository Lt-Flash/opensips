# Benchmarks after the review fixes

Measured on 2026-10-23 against the head of [OpenSIPS/opensips#4327](https://github.com/OpenSIPS/opensips/pull/4327), `63046ceca0`, which carries both rounds of review fixes: process exit and double frees in the first; in the second a state and a checksum in every cell header with an atomic free, the HG_MALLOC_DBG checks, sanitizer hooks, the host free-memory floor and the memory cgroup's limit, and 32-bit counting. Same hosts, same drivers and the same method as [benchmarks.md](benchmarks.md); the allocators compared are the same too: F_MALLOC, F_PARALLEL_MALLOC, HP_MALLOC and HG_MALLOC on setup A, F_MALLOC and HG_MALLOC on setup B.

HG_MALLOC is still ahead in every test: the least CPU, no UDP drops and no failed requests where the other allocators drop or fail, and the highest sustained call rates (equal to F_MALLOC's in two of the three TLS modes). Run alongside on setup A, the binary behind the original tables - HG_MALLOC before the review fixes - is within 3.2% of the current head in CPU, in both directions.

**Reading the setup A tables:** `a → b (c, d)` is the run behind the original tables (one run) → the mean of today's two runs (the two runs). The second run took the allocators in reverse order. Setup B cells say *original → now* where both exist.

## Setup A: INVITEs answered by tm

- **Server:** the setup A host of [benchmarks.md](benchmarks.md#what-was-measured-where) (16-vCPU KVM guest, Xeon E5-2699 v4, Ubuntu 24.04, 16-queue virtio NIC), 8 UDP workers, 8 MB socket buffers unless a table says otherwise; HG_MALLOC on a hugetlb pool (tier 1, confirmed from `core:hg_stats` in all 28 HG_MALLOC runs, registrar included), the other allocators on plain 4 KB pages.
- **Build:** the pull request's head `63046ceca0`, gcc 13, optimised, every allocator in one binary and chosen with `-a` (`-a F_MALLOC -s F_PARALLEL_MALLOC` for F_PARALLEL_MALLOC). An extra arm, *HG_MALLOC before the review fixes*, runs the binary behind the original tables (`9f140e2d38`).
- **Method:** as in [setup A in detail](benchmarks.md#setup-a-invites-answered-by-tm): sipp from two other hosts, 60 s per point, CPU is user plus system time of every OpenSIPS process, the probe sends 100 INVITEs/s and times each round trip, drops are the kernel's UDP receive-buffer errors, peak shared memory is the arena's peak carved for HG_MALLOC and `shmem:max_used_size` for the others.
- **Difference from the original runs:** the original 20,000-60,000 table was measured while 6 of the server's 16 transmit queues were still on `fq_codel` (a queueing-discipline setup error, fixed before the later tables); every run here has all 16 on `pfifo_fast`. Every allocator used 1-3% fewer cores at 20,000 than in the original table.

#### 20,000-60,000 INVITEs/s, `-m 2048` (HG_MALLOC `-m 128:2048`)

| INVITEs/s | allocator | cores | UDP drops | probe p99 | peak shared memory, MB | failed INVITEs |
|---|---|---|---|---|---|---|
| 20,000 | F_MALLOC | 2.40 → 2.35 (2.35, 2.35) | 0 → 0 (0, 0) | 0.94 ms → 0.94 ms (0.85 ms, 1.03 ms) | 445 → 446 (445, 446) | 0 → 0 (0, 0) |
| 20,000 | F_PARALLEL_MALLOC | 2.30 → 2.28 (2.27, 2.28) | 0 → 0 (0, 0) | 0.82 ms → 0.82 ms (0.81 ms, 0.84 ms) | 479 → 478 (477, 478) | 0 → 0 (0, 0) |
| 20,000 | HP_MALLOC | 2.25 → 2.20 (2.22, 2.19) | 0 → 0 (0, 0) | 0.73 ms → 0.90 ms (0.94 ms, 0.86 ms) | 449 → 449 (449, 449) | 0 → 0 (0, 0) |
| 20,000 | HG_MALLOC | 2.04 → 1.99 (2.00, 1.98) | 0 → 0 (0, 0) | 0.78 ms → 0.77 ms (0.74 ms, 0.81 ms) | 502 → 502 (502, 501) | 0 → 0 (0, 0) |
| 20,000 | HG_MALLOC before the review fixes | 2.04 → 2.00 (1.95, 2.05) | 0 → 0 (0, 0) | 0.78 ms → 0.79 ms (0.68 ms, 0.90 ms) | 502 → 501 (500, 502) | 0 → 0 (0, 0) |
| 45,000 | F_MALLOC | 4.41 → 4.55 (4.55, 4.56) | 31,030 → 36,622 (42,831, 30,412) | 6.06 ms → 7.66 ms (7.80 ms, 7.53 ms) | 1,051 → 1,048 (1,046, 1,050) | 0 → 0 (0, 0) |
| 45,000 | F_PARALLEL_MALLOC | 4.14 → 4.41 (4.48, 4.34) | 167 → 368 (737, 0) | 3.29 ms → 5.04 ms (5.00 ms, 5.09 ms) | 1,038 → 1,032 (1,031, 1,033) | 0 → 0 (0, 0) |
| 45,000 | HP_MALLOC | 4.22 → 4.38 (4.38, 4.37) | 19,342 → 43,062 (37,146, 48,978) | 4.61 ms → 6.59 ms (7.37 ms, 5.81 ms) | 1,009 → 1,049 (1,061, 1,037) | 0 → 0 (0, 0) |
| 45,000 | HG_MALLOC | 3.62 → 3.68 (3.69, 3.67) | 0 → 0 (0, 0) | 1.36 ms → 1.47 ms (1.29 ms, 1.66 ms) | 1,118 → 1,118 (1,118, 1,119) | 0 → 0 (0, 0) |
| 45,000 | HG_MALLOC before the review fixes | 3.62 → 3.62 (3.61, 3.62) | 0 → 0 (0, 0) | 1.36 ms → 1.66 ms (1.45 ms, 1.88 ms) | 1,118 → 1,114 (1,113, 1,115) | 0 → 0 (0, 0) |
| 60,000 | F_MALLOC | 8.57 → 8.65 (8.59, 8.71) | 7,701,938 → 8,263,318 (7,681,562, 8,845,073) | 36.2 ms → 39.1 ms (50.9 ms, 27.3 ms) | 2,022 → 2,023 (2,023, 2,023) | 508,165 → 834,048 (716,521, 951,576) |
| 60,000 | F_PARALLEL_MALLOC | 8.13 → 7.24 (6.36, 8.11) | 3,609,825 → 1,779,334 (258,841, 3,299,828) | 37.7 ms → 20.4 ms (14.2 ms, 26.6 ms) | 1,747 → 1,655 (1,483, 1,827) | 122,206 → 37,726 (0, 75,451) |
| 60,000 | HP_MALLOC | 5.40 → 6.82 (6.82, 6.82) | 247,051 → 883,200 (661,310, 1,105,090) | 8.36 ms → 14.9 ms (13.8 ms, 16.1 ms) | 1,499 → 2,029 (2,029, 2,029) | 0 → 1,387 (0, 2,774) |
| 60,000 | HG_MALLOC | 4.69 → 4.65 (4.63, 4.67) | 0 → 0 (0, 0) | 3.30 ms → 4.12 ms (3.54 ms, 4.70 ms) | 1,489 → 1,486 (1,486, 1,486) | 0 → 0 (0, 0) |
| 60,000 | HG_MALLOC before the review fixes | 4.69 → 4.51 (4.44, 4.58) | 0 → 0 (0, 0) | 3.30 ms → 3.41 ms (2.36 ms, 4.47 ms) | 1,489 → 1,486 (1,487, 1,485) | 0 → 0 (0, 0) |

#### 60,000 INVITEs/s, `-m 4096` (HG_MALLOC `-m 128:4096`)

| INVITEs/s | allocator | cores | UDP drops | probe p99 | peak shared memory, MB | failed INVITEs |
|---|---|---|---|---|---|---|
| 60,000 | F_MALLOC | 5.98 → 6.24 (6.60, 5.88) | 1,119,613 → 1,027,800 (1,557,921, 497,679) | 13.8 ms → 15.0 ms (16.5 ms, 13.5 ms) | 2,467 → 2,258 (2,577, 1,938) | 4,412 → 6,088 (11,995, 182) |
| 60,000 | F_PARALLEL_MALLOC | 5.43 → 5.63 (5.72, 5.54) | 4,554 → 2,380 (2,892, 1,869) | 6.51 ms → 9.91 ms (10.2 ms, 9.58 ms) | 1,374 → 1,368 (1,362, 1,373) | 0 → 0 (0, 0) |
| 60,000 | HP_MALLOC | 6.31 → 6.57 (6.34, 6.80) | 363,419 → 756,258 (315,301, 1,197,216) | 13.4 ms → 14.5 ms (13.4 ms, 15.5 ms) | 1,978 → 2,055 (1,719, 2,391) | 0 → 2,854 (0, 5,708) |
| 60,000 | HG_MALLOC | 4.59 → 4.66 (4.69, 4.62) | 0 → 0 (0, 0) | 1.95 ms → 2.71 ms (2.15 ms, 3.27 ms) | 1,486 → 1,492 (1,496, 1,488) | 0 → 0 (0, 0) |
| 60,000 | HG_MALLOC before the review fixes | 4.59 → 4.53 (4.56, 4.50) | 0 → 0 (0, 0) | 1.95 ms → 2.45 ms (2.58 ms, 2.33 ms) | 1,486 → 1,491 (1,490, 1,492) | 0 → 0 (0, 0) |

#### 60,000 INVITEs/s, `-m 4096`, 8 MB socket buffers, host idle from mpstat

| INVITEs/s | allocator | cores | UDP drops | probe p99 | peak shared memory, MB | failed INVITEs | host idle |
|---|---|---|---|---|---|---|---|
| 60,000 | F_MALLOC | 5.69 → 6.49 (6.24, 6.74) | 541,454 → 1,626,072 (864,639, 2,387,506) | 14.9 ms → 16.3 ms (16.9 ms, 15.7 ms) | 1,893 → 2,585 (2,251, 2,919) | 368 → 16,389 (1,023, 31,755) | 63% → 56% (59%, 53%) |
| 60,000 | F_PARALLEL_MALLOC | 5.19 → 5.78 (5.82, 5.73) | 2,998 → 6,364 (6,569, 6,158) | 10.2 ms → 9.14 ms (9.39 ms, 8.88 ms) | 1,375 → 1,373 (1,379, 1,367) | 0 → 0 (0, 0) | 68% → 61% (61%, 61%) |
| 60,000 | HP_MALLOC | 5.12 → 6.43 (6.33, 6.53) | 165,880 → 576,240 (482,627, 669,854) | 8.45 ms → 13.9 ms (14.4 ms, 13.3 ms) | 1,527 → 2,118 (2,011, 2,224) | 0 → 3 (0, 6) | 68% → 58% (59%, 57%) |
| 60,000 | HG_MALLOC | 4.45 → 4.62 (4.58, 4.67) | 0 → 0 (0, 0) | 2.42 ms → 3.44 ms (2.71 ms, 4.16 ms) | 1,491 → 1,487 (1,488, 1,486) | 0 → 0 (0, 0) | 72% → 70% (70%, 70%) |
| 60,000 | HG_MALLOC before the review fixes | 4.45 → 4.60 (4.48, 4.72) | 0 → 0 (0, 0) | 2.42 ms → 2.72 ms (2.34 ms, 3.10 ms) | 1,491 → 1,492 (1,489, 1,494) | 0 → 0 (0, 0) | 72% → 70% (70%, 70%) |

#### 60,000 INVITEs/s, `-m 4096`, 32 MB socket buffers, host idle from mpstat

| INVITEs/s | allocator | cores | UDP drops | probe p99 | peak shared memory, MB | failed INVITEs | host idle |
|---|---|---|---|---|---|---|---|
| 60,000 | F_MALLOC | 5.71 → 6.51 (6.30, 6.72) | 459,068 → 1,212,386 (863,909, 1,560,864) | 12.1 ms → 18.0 ms (16.4 ms, 19.5 ms) | 1,980 → 2,509 (2,371, 2,647) | 8 → 2,346 (195, 4,496) | 62% → 57% (59%, 56%) |
| 60,000 | F_PARALLEL_MALLOC | 5.29 → 5.57 (5.57, 5.57) | 0 → 0 (0, 0) | 7.61 ms → 10.3 ms (11.3 ms, 9.35 ms) | 1,369 → 1,366 (1,365, 1,367) | 0 → 0 (0, 0) | 65% → 64% (65%, 63%) |
| 60,000 | HP_MALLOC | 5.33 → 6.23 (5.55, 6.91) | 230,920 → 275,176 (52,332, 498,020) | 8.36 ms → 19.2 ms (10.9 ms, 27.6 ms) | 1,588 → 1,746 (1,396, 2,096) | 0 → 0 (0, 0) | 65% → 59% (63%, 56%) |
| 60,000 | HG_MALLOC | 4.44 → 4.59 (4.58, 4.60) | 0 → 0 (0, 0) | 3.13 ms → 2.79 ms (2.80 ms, 2.79 ms) | 1,489 → 1,490 (1,492, 1,488) | 0 → 0 (0, 0) | 71% → 69% (69%, 69%) |
| 60,000 | HG_MALLOC before the review fixes | 4.44 → 4.74 (4.77, 4.72) | 0 → 0 (0, 0) | 3.13 ms → 2.76 ms (2.64 ms, 2.89 ms) | 1,489 → 1,488 (1,489, 1,486) | 0 → 0 (0, 0) | 71% → 69% (70%, 67%) |

#### Registrar: 1.2 M REGISTERs at 60,000/s, `-m 4096` (HG_MALLOC `-m 128:4096`), runs 1 / 2

| allocator | registered | UDP drops | sipp p99 | cores | peak shared memory, MB |
|---|---|---|---|---|---|
| F_MALLOC | 1,200,000 / 1,200,000 → 1,199,991 / 1,200,000 | 2,218,973 / 1,920,821 → 2,473,243 / 1,993,767 | 19,596 ms / 19,528 ms → 23,532 ms / 19,564 ms | 4.00 / 4.00 → 4.00 / 4.00 | 1,459 / 1,459 → 1,466 / 1,466 |
| F_PARALLEL_MALLOC | 1,200,000 / 1,200,000 → 1,200,000 / 1,200,000 | 1,649,387 / 1,304,564 → 1,321,971 / 1,674,134 | 15,596 ms / 15,524 ms → 15,524 ms / 15,592 ms | 3.97 / 3.84 → 3.82 / 3.98 | 1,484 / 1,484 → 1,491 / 1,491 |
| HP_MALLOC | 520,033 / 515,904 → 533,321 / 534,455 | 6,901,970 / 6,973,834 → 6,820,732 / 6,807,607 | 27,952 ms / 27,960 ms → 27,952 ms / 27,940 ms | 4.00 / 4.00 → 4.00 / 4.00 | 596 / 591 → 614 / 615 |
| HG_MALLOC | 1,200,000 / 1,200,000 → 1,200,000 / 1,200,000 | 0 / 0 → 0 / 0 | 4 ms / 4 ms → 4 ms / 4 ms | 2.05 / 1.90 → 1.88 / 1.94 | 1,481 / 1,481 → 1,508 / 1,508 |
| HG_MALLOC before the review fixes | 1,200,000 / 1,200,000 → 1,200,000 / 1,200,000 | 0 / 0 → 0 / 0 | 4 ms / 4 ms → 4 ms / 4 ms | 2.05 / 1.90 → 1.84 / 2.05 | 1,481 / 1,481 → 1,508 / 1,508 |

- HG_MALLOC: 0 UDP drops and 0 failed INVITEs in every run, and the fewest cores at every point. Against F_MALLOC: -15%, -19% and -46% at 20,000, 45,000 and 60,000 (originally -15%, -18%, -45%); -25% at 60,000 with `-m 4096` (-23%); -29% with 8 MB and with 32 MB buffers (-22% both).
- Near overload the other allocators vary from run to run: F_MALLOC with 8 MB buffers at 60,000 dropped 864,639 and 2,387,506 datagrams in its two runs, HP_MALLOC used up to 1.4 cores more than originally at 60,000 and failed some INVITEs this time, F_PARALLEL_MALLOC failed no INVITE in one of its two `-m 2048` runs at 60,000 and 75,451 in the other.
- HG_MALLOC before and after the review fixes: -0.4%, +1.8% and +3.1% in cores at 20,000, 45,000 and 60,000, +2.8% with `-m 4096`, +0.5% and -3.2% with 8 MB and 32 MB buffers; probe p99 ranges overlap.

## Setup B: calls relayed by a stateful proxy

- **Server:** the setup B host of [benchmarks.md](benchmarks.md#what-was-measured-where) (16-vCPU KVM guest, Xeon E5-2699 v4, Debian 13, Linux 6.12, no hugetlb pool, THP `shmem_enabled` = `never`), 8 workers per transport; HG_MALLOC `-m 512:4096 -M 64:512`, F_MALLOC `-m 2048 -M 32`.
- **Build:** as before, an OpenSIPS 4.1 tree: master plus HG_MALLOC - now with the review fixes - and other pending patches, among them the receive modes of [OpenSIPS PR #4365](https://github.com/OpenSIPS/opensips/pull/4365) (`shared` is the current code).
- **Method:** as in [setup B](benchmarks.md#setup-b-relayed-calls-over-udp-tls-and-wss): sipp on three other hosts, 30 s cells, two repetitions per rate and mode, the server restarted for every cell. A rate holds when both repetitions complete at least 98% of the offered calls with at most 0.1% failed and every load generator reported. The load generators split a rate over 12 instances (24 for TLS and WSS), so the offered total is rounded: 16,000/s offers 479,880 calls in 30 s.
- **TLS and WSS:** TLS with default settings, WSS with the staging settings (WSS does not connect with the default `wss_max_msg_chunks`), as before. The original default-settings TLS templates were not kept; they were rebuilt by removing the staging settings (`tls_max_msg_chunks` / `wss_max_msg_chunks` 32, handshake and send timeouts 1,500 ms, TLS 1.2-1.3, and on the gateways `tls_async_max_postponed_chunks` 1024).
- **Two passes for TLS and WSS:** the ceilings come from the first pass. In it the per-process sampler could not tell TCP main from the TCP workers (its CLI configuration was missing on the server), so the same-rate cells were measured again, with the sampler fixed, in a second pass that held in every cell.

### Highest sustained calls/s (shared / auto / exclusive), original → now

| transport (socket buffers / settings) | F_MALLOC | HG_MALLOC |
|---|---|---|
| UDP, default 512 KB | 10,000 / 10,000 / 10,000 → 10,000 / 10,000 / 10,000 | 20,000 / 22,000 / 18,000 → **20,000 / 22,000 / 20,000** |
| UDP, 8 MB | 10,000 / 13,000 / 12,000 → 13,000 / 14,000 / 12,000 | not reported → **22,000 / 22,000 / 22,000** |
| TLS, defaults | 6,000 / 6,000 / 6,000 → 6,000 / 6,000 / 6,000 | 6,000 / 8,000 / 6,000 → **6,000 / 8,000 / 6,000** |
| WSS, staging settings | 6,000 / 5,000 / 6,000 → 5,000 / 5,000 / 6,000 | 5,000 / 5,000 / 5,000 → **6,000 / 6,000 / 6,000** |

UDP rates tried: F_MALLOC 8,000, 10,000, 12,000 (512 KB) and 10,000, 12,000, 13,000, 14,000, 16,000 (8 MB); HG_MALLOC 12,000, 16,000, 18,000, 20,000, 22,000, 24,000 (both buffers). At 16,000 with 8 MB, no F_MALLOC load generator wrote its summary before its timeout while the server dropped about a million datagrams per sampling window; those cells count as not holding. At 24,000 HG_MALLOC fails in every mode with either buffer. TLS and WSS search down from 10,000 (10,000, 8,000, 6,000, 5,000) per mode.

### Same rate, UDP 12,000/s, default 512 KB buffer: F_MALLOC → HG_MALLOC

| | shared | auto | exclusive |
|---|---|---|---|
| failed calls (2 cells), original | 4,116 → 0 | 1,238 → 0 | 4,990 → 0 |
| failed calls (2 cells), now | 2,725 → 0 | 1,252 → 0 | 3,229 → 0 |
| UDP receive-buffer drops, original | 56,100 → 7,351 | 43,041 → 9,856 | 76,754 → 6,486 |
| UDP receive-buffer drops, now | 28,970 → 6,744 | 35,623 → 9,365 | 45,016 → 5,303 |
| CPU % per 1,000 calls/s, original | 48.2 → 36.2 (-25%) | 44.1 → 29.9 (-32%) | 47.4 → 32.7 (-31%) |
| CPU % per 1,000 calls/s, now | 44.4 → 35.0 (-21%) | 41.8 → 31.4 (-25%) | 43.1 → 32.2 (-25%) |
| syscalls per call, original | 92.3 → 29.0 (-69%) | 76.2 → 23.4 (-69%) | 97.6 → 27.1 (-72%) |
| syscalls per call, now | 81.5 → 27.0 (-67%) | 70.9 → 23.0 (-68%) | 84.3 → 25.0 (-70%) |

HG_MALLOC's own figures are close to the original ones; the smaller CPU difference comes from F_MALLOC, which at 12,000/s is past its limit (it fails calls and retransmits heavily) and costs less per completed call in some runs than in others.

### Same rate, TLS and WSS
#### TLS 6,000/s, default settings: F_MALLOC → HG_MALLOC, mean of two cells

| | shared | auto | exclusive |
|---|---|---|---|
| CPU % per 1,000 calls/s | 132.9 → 122.4 (-8%) | 121.1 → 112.6 (-7%) | 120.1 → 106.5 (-11%) |
| TCP workers' CPU % | 280 → 235 (-16%) | 241 → 195 (-19%) | 227 → 171 (-24%) |
| TCP main CPU % | 381 → 377 (-1%) | 350 → 354 (+1%) | 357 → 345 (-3%) |
| syscalls per call | 115.8 → 102.4 (-12%) | 114.8 → 99.2 (-14%) | 119.0 → 102.3 (-14%) |
| `sched_yield` per call | 26.4 → 12.5 (-53%) | 27.4 → 11.5 (-58%) | 31.3 → 14.1 (-55%) |
| failed calls (2 cells) | 0 → 0 | 0 → 0 | 0 → 0 |

#### WSS 5,000/s, staging settings: F_MALLOC → HG_MALLOC, mean of two cells

| | shared | auto | exclusive |
|---|---|---|---|
| CPU % per 1,000 calls/s | 156.1 → 146.8 (-6%) | 145.3 → 132.8 (-9%) | 140.4 → 129.8 (-8%) |
| TCP workers' CPU % | 238 → 203 (-15%) | 202 → 160 (-21%) | 186 → 147 (-21%) |
| TCP main CPU % | 426 → 424 (-0%) | 405 → 396 (-2%) | 398 → 395 (-1%) |
| syscalls per call | 144.5 → 135.0 (-7%) | 145.2 → 133.8 (-8%) | 146.2 → 136.2 (-7%) |
| `sched_yield` per call | 18.2 → 7.9 (-57%) | 21.4 → 9.3 (-56%) | 22.0 → 11.4 (-48%) |
| failed calls (2 cells) | 0 → 0 | 0 → 0 | 0 → 0 |

The description's ranges: TLS -9 to -11% CPU per call, -18 to -23% TCP workers' CPU, -14 to -20% syscalls, -54 to -69% `sched_yield`; WSS -7 to -8%, -16 to -21%, -10 to -12%, -59 to -64%.

### UDP with an 8 MB socket buffer: HG_MALLOC's drops

Drops counted over the whole cell from the kernel's counters, `maxbuffer` 4 MB (8 MB on the live socket, read back in every cell); six cells per rate (three modes, two repetitions).

| relayed calls/s | cells without a drop, original → now | drops per cell, original → now | failed calls per cell, original → now |
|---|---|---|---|
| 12,000 | 6 of 6 → 6 of 6 | 0 → 0 | 0 → 0 |
| 16,000 | 6 of 6 → 6 of 6 | 0 → 0 | 0 → 0 |
| 18,000 | 3 of 6 → 5 of 6 | 0-6,661 → 0-515 | 0 → 0 |
| 20,000 | 0 of 6 → 2 of 6 | 793-42,174 → 0-12,084 | 0 → 0 |
| 22,000 | 0 of 6 → 0 of 6 | 51,303-221,282 → 40,143-84,735 | 2-667, one cell completed 92% of its calls → 0-28 |
| 24,000 | not measured → 0 of 6 | 84,232-406,202 | 0-3,569; one cell is missing one load generator's report |

Controls in the same run at 12,000/s, `shared`: F_MALLOC with the same 8 MB buffer dropped 132,470 and 55,814 datagrams (280 and 0 failed calls; originally 57,198 and 230,321, with 0 and 1,525 failed), and HG_MALLOC with the default 512 KB buffer dropped 11,403 (0 failed; originally 9,827).

