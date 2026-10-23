# Benchmarks

The figures on this page come from two setups that count different things. On setup A one unit is an INVITE answered by the server itself; on setup B it is a whole call relayed through the server, with more than four times as many datagrams and two transactions. 60,000 per second on setup A and 20,000 per second on setup B are therefore not comparable rates: compare allocators within a table, not rates across setups.

## What was measured where

| | Setup A: INVITEs answered by the server | Setup B: calls relayed by a stateful proxy |
|---|---|---|
| one unit | an INVITE answered 486 by tm, its ACK absorbed: one transaction, 3 datagrams (INVITE in, 486 out, ACK in), nothing forwarded | a call relayed to a UAS with record-routing: INVITE / 100 / 180 / 200 / ACK / BYE / 200, two transactions, 6 datagrams in and 7 out |
| sections | [INVITEs answered by tm](#setup-a-invites-answered-by-tm), [Backing tiers](#backing-tiers), [Memory under load](#memory-under-load), [Registrar](#registrar-under-a-cold-start-12-million-registers-at-60000-per-second) (REGISTERs instead of INVITEs) | [Relayed calls over UDP, TLS and WSS](#setup-b-relayed-calls-over-udp-tls-and-wss) |
| server | 16-vCPU KVM guest, Xeon E5-2699 v4, Ubuntu 24.04, kernel 6.8, virtio NIC with 16 queues | another 16-vCPU KVM guest, Xeon E5-2699 v4, Debian 13, kernel 6.12, virtio NIC with 16 queues |
| build | PR #4327 as of 2026-10-11 (`9f140e2d38`: OpenSIPS master plus HG_MALLOC), gcc 13; [Backing tiers](#backing-tiers) used later commits of the PR, see there | an OpenSIPS 4.1 tree: master plus HG_MALLOC and other pending patches, among them the UDP/TCP receive modes of [OpenSIPS PR #4365](https://github.com/OpenSIPS/opensips/pull/4365) |
| allocators | F_MALLOC, F_PARALLEL_MALLOC, HP_MALLOC, HG_MALLOC | F_MALLOC, HG_MALLOC |
| HG_MALLOC memory | a hugetlb pool (tier 1), except in [Backing tiers](#backing-tiers) | no hugetlb pool; the shared arena on plain 4 KB pages (the host's THP `shmem_enabled` is `never`), the private arenas on THP |
| the other allocators' memory | plain 4 KB pages | shared memory on plain 4 KB pages (the same host setting); private memory under the host's THP policy `always` |
| server socket buffer | 8 MB (32 MB in one table) | 512 KB, the OpenSIPS default, and 8 MB in one table |
| load | sipp on two other hosts | sipp on three other hosts; for TLS and WSS through an OpenSIPS gateway on each |
| runs | one 60 s run per point; the registrar two | 30 s cells, two repetitions |

In both setups one binary served every allocator, chosen on the command line, so within a table the allocator is the only difference:

| arm | options |
|---|---|
| F_MALLOC | `-a F_MALLOC` |
| F_PARALLEL_MALLOC | `-a F_MALLOC -s F_PARALLEL_MALLOC` (F_PARALLEL_MALLOC is shared-memory only) |
| HP_MALLOC | `-a HP_MALLOC` |
| HG_MALLOC | `-a HG_MALLOC` |

Setup A in detail:

- **Server:** 8 MB UDP socket buffers (`rmem`/`wmem` default and max 8 MB, `maxbuffer` 8 MB), a hugetlb pool large enough for the caps, `LimitMEMLOCK` unlimited. HG_MALLOC's tier (shared and private arenas) was confirmed from `core:hg_stats` in every run.
- **Load:** sipp, up to 60 instances split evenly over two other 16-vCPU hosts with 16-queue NICs on the same LAN, 1,000-2,000 INVITEs per second each.
- **CPU:** user plus system time of every OpenSIPS process over a fixed window while the load runs.
- **Latency:** sipp's response times, plus a separate probe sending 100 requests per second (INVITEs; REGISTERs on the registrar) and timing each round trip in microseconds.
- **Drops:** the kernel's UDP receive-buffer errors on the server.

## Setup A: INVITEs answered by tm

Vanilla config: proto_udp, tm, sl, signaling, sipmsgops; 8 UDP workers; each INVITE answered 486 by tm, ACK absorbed. 60 s per point. Rates are INVITEs per second.

All four arms measured in one session, `-m 2048 -M 32`; HG_MALLOC elastic `-m 128:2048 -M 16:32`.

| INVITEs/s | | F_MALLOC | F_PARALLEL_MALLOC | HP_MALLOC | HG_MALLOC |
|---|---|---|---|---|---|
| 20,000 | CPU, cores | 2.40 | 2.30 | 2.25 | **2.04** |
| | UDP drops | 0 | 0 | 0 | **0** |
| | INVITE p99 (probe) | 0.94 ms | 0.82 ms | 0.73 ms | **0.78 ms** |
| 45,000 | CPU, cores | 4.41 | 4.14 | 4.22 | **3.62** |
| | UDP drops | 31,030 | 167 | 19,342 | **0** |
| | INVITE p99 (probe) | 6.1 ms | 3.3 ms | 4.6 ms | **1.4 ms** |
| 60,000 | CPU, cores | 8.57 | 8.13 | 5.40 | **4.69** |
| | UDP drops | 7,701,938 | 3,609,825 | 247,051 | **0** |
| | INVITE p99 (probe) | 36 ms | 38 ms | 8.4 ms | **3.3 ms** |
| | failed INVITEs (sipp) | 508,165 | 122,206 | 0 | **0** |

Up to 45,000 every INVITE was answered in every arm. At 60,000 F_MALLOC ran out of the 2 GB shared memory 16 s into the run (2,022 of 2,048 MB used), and F_PARALLEL_MALLOC ran out in one of its pools after 11 s (1,747 MB used in total); tm could no longer clone requests, INVITEs failed, sipp retransmitted, and all 8 workers saturated. Both also lost 17 probe INVITEs. HP_MALLOC and HG_MALLOC answered every INVITE.

The elastic arena grew from 128 MB to 528 MB, 1,136 MB and 1,536 MB at the three rates. The longest arena-lock hold was 550 us, 1,363 us and 1,143 us; 1 hold at 45,000 and 4 at 60,000 went over the 1 ms stall threshold.

The 60,000 point again with `-m 4096 -M 32` (HG_MALLOC `-m 128:4096 -M 16:32`), so that no arm runs out of memory:

| INVITEs/s | | F_MALLOC | F_PARALLEL_MALLOC | HP_MALLOC | HG_MALLOC |
|---|---|---|---|---|---|
| 60,000 | CPU, cores | 5.98 | 5.43 | 6.31 | **4.59** |
| | UDP drops | 1,119,613 | 4,554 | 363,419 | **0** |
| | INVITE p99 (probe) | 13.8 ms | 6.5 ms | 13.4 ms | **1.9 ms** |
| | failed INVITEs (sipp) | 4,412 | 0 | 0 | **0** |

No arm logged a memory error. F_MALLOC peaked at 2,467 MB, more than the 2 GB of the first run, and still failed 4,412 INVITEs whose retransmissions were all dropped; HP_MALLOC's slowest probe INVITE took 1.3 s. HG_MALLOC grew to the same 1,536 MB as with the 2 GB cap, with no lock stall (longest hold 873 us); one client host retransmitted 8,485 times, and six probe INVITEs got their answer only from tm's reply retransmission 500 ms later, with no drop and no send error on the server.

The same point once more with the host's CPU sampled by `mpstat` on the server over the measurement window, to see whether the drops come from a lack of CPU:

| 60,000, `-m 4096` | F_MALLOC | F_PARALLEL_MALLOC | HP_MALLOC | HG_MALLOC |
|---|---|---|---|---|
| OpenSIPS CPU, cores | 5.69 | 5.19 | 5.12 | **4.45** |
| host idle | 63% (10.1 cores) | 68% (10.8) | 68% (10.9) | **72% (11.5)** |
| kernel softirq | 4.5% | 4.1% | 4.2% | 4.3% |
| UDP drops | 541,454 | 2,998 | 165,880 | **0** |
| INVITE p99 (probe) | 14.9 ms | 10.2 ms | 8.4 ms | **2.4 ms** |

The others drop with 10 or more of the 16 cores idle, so the workers are not short of CPU: they fall behind in bursts and the 8 MB socket buffer overflows. Drop counts vary between runs - F_MALLOC dropped 1,119,613 and 541,454 in the two runs at this point, HP_MALLOC 363,419 and 165,880 - while the order of the arms and HG_MALLOC's zero did not change.

Straight after it, the same point with 32 MB socket buffers: `maxbuffer` and `rmem`/`wmem` default and max on the server raised from 8 MB to 32 MB, the clients unchanged:

| 60,000, `-m 4096`, 32 MB buffers | F_MALLOC | F_PARALLEL_MALLOC | HP_MALLOC | HG_MALLOC |
|---|---|---|---|---|
| OpenSIPS CPU, cores | 5.71 | 5.29 | 5.33 | **4.44** |
| host idle | 62% (9.8 cores) | 65% (10.4) | 65% (10.5) | **71% (11.4)** |
| UDP drops (8 MB run) | 459,068 (541,454) | 0 (2,998) | 230,920 (165,880) | **0 (0)** |
| INVITE p99 (probe) | 12.1 ms | 7.6 ms | 8.4 ms | **3.1 ms** |
| slowest probe INVITE | 241 ms | 554 ms | 54 ms | **11 ms** |

Four times the buffer does not stop F_MALLOC's and HP_MALLOC's drops - the difference is within their run-to-run spread - so their workers stall for longer than 32 MB of queue covers. F_PARALLEL_MALLOC, which dropped only 2,998 with 8 MB, dropped none; one of its probe INVITEs still took 554 ms. HG_MALLOC dropped none on the server either way; with 32 MB, one client host retransmitted 3,342 times and two probe INVITEs timed out while the server's receive and send error counters stayed at zero.

## Backing tiers

Setup A. HG_MALLOC alone at 60,000 INVITEs/s on each of the four tiers, the host set up for one tier at a time (hugetlb pool; no pool and THP `advise`; the same with the advice skipped, so pages fault as 4 KB and are collapsed afterwards; no pool and THP `never`), the tier confirmed from the startup log and `core:hg_stats`. `-m 128:4096 -M 16:32`, 8 MB buffers; the other allocators ran on plain 4 KB pages in every setup A table. These runs used later commits of the PR from the same night - `6bd3352a09`, and for the tier-3 row and the budget table the commits that added the deferred collapse (`6642c73b70`, then `0c21c59eab` with retries) - built with a test-only switch that skips `MADV_HUGEPAGE` so that the host could be held on tier 3; the switch is not in the PR.

| tier | OpenSIPS CPU, cores | vs plain 4 KB | host idle | UDP drops | INVITE p99 (probe) | vs plain 4 KB |
|---|---|---|---|---|---|---|
| 1 - hugetlb pool | **4.27** | **-13%** | 71% | 0 | **1.9 ms** | **-52%** |
| 2 - THP by advice | 4.47 | -9% | 71% | 0 | 2.3 ms | -43% |
| 3 - THP by collapse | 4.69 | -4% | 68% | 0 | 5.8 ms | +43% |
| 4 - plain 4 KB, what the other allocators use | 4.90 | - | 67% | 0 | 4.0 ms | - |

Huge pages are worth about 13% of the CPU and half the p99 at this rate; on plain 4 KB pages, like the other allocators, HG_MALLOC still needs 4-14% less CPU than they do (5.12-5.69 cores, mpstat run above) and drops nothing.

Tier 3 grows the arena from 128 MB to 1.6 GB on 4 KB pages during the run and collapses it in the background, so part of its measurement runs on 4 KB pages while pages are being collapsed - hence a p99 above tier 4's; once collapsed it is the same memory as tier 2. It first ran with the `MADV_COLLAPSE` retrofit inside each grow: 36 ms per 16 MB step instead of about 5 ms, paid by the worker that ran out of memory while others waited for it, and 107,524 UDP drops in the first 18 s. Such a step now publishes its pages on 4 KB at once and the maintenance process collapses them, retrying a page the kernel cannot collapse yet. Its per-second budget:

| collapse budget per second | UDP drops | OpenSIPS CPU, cores | INVITE p99 | whole arena on huge pages after | pages retried / left on 4 KB |
|---|---|---|---|---|---|
| 50 ms | 0 | 4.82 | 6.5 ms | ~45 s | 2 / 0 |
| **100 ms (the default)** | **0** | **4.69** | **5.8 ms** | **~25 s** | **0 / 0** |
| 200 ms | 0 | 4.63 | 6.3 ms | ~15 s | 182 / 0 |
| 500 ms, first version without retries | 7,034 | 4.53 | 2.7 ms | never - 204 pages left on 4 KB | - |

## Memory under load

Setup A. Peak shared memory in the INVITE runs above, with the UDP drops of the same run:

| INVITEs/s | F_MALLOC | F_PARALLEL_MALLOC | HP_MALLOC | HG_MALLOC |
|---|---|---|---|---|
| 20,000 | 445 MB (0 drops) | 479 MB (0) | 449 MB (0) | **502 MB (0)** |
| 45,000 | 1,051 MB (31,030) | 1,038 MB (167) | 1,009 MB (19,342) | **1,118 MB (0)** |
| 60,000, `-m 2048` | ran out at 2,022 MB (7.7 M) | ran out in one pool, 1,747 MB in total (3.6 M) | 1,499 MB (247,051) | **1,489 MB (0)** |
| 60,000, `-m 4096` | 2,467 MB (1.1 M) | 1,374 MB (4,554) | 1,978 MB (363,419) | **1,486 MB (0)** |

Each INVITE is a tm transaction that stays in shared memory until its ACK arrives and the wait timer (2 s here) runs out. A server that drops packets loses replies and ACKs: tm keeps those transactions while the reply is retransmitted, and the retransmissions cause more drops. So at high rates the peak follows the drops more than the allocator's per-object cost. F_MALLOC and F_PARALLEL_MALLOC are the same allocator family with the same per-object overhead, yet at 60,000 with 4 GB F_MALLOC (1.1 M drops) needed 80% more memory than F_PARALLEL_MALLOC (4,554 drops).

Per object, HG_MALLOC uses a little more: 5-13% at 20,000 and 45,000, where there are few or no drops, from size-class rounding of tm's 1-4 KB request and reply copies. Under overload it needs less: at 60,000 its workers keep up, nothing waits in memory for a retransmission, and it peaked at 1.5 GB against 2.0 GB for HP_MALLOC and 2.5 GB for F_MALLOC. With a 2 GB cap it peaked at 73% of it, where F_MALLOC and F_PARALLEL_MALLOC ran out. Against F_PARALLEL_MALLOC with a 4 GB cap, which had almost no drops, HG_MALLOC needed 8% more.

HG_MALLOC's figure is its peak carved memory (`carved_peak` in `core:hg_stats`), which also counts idle cells in partly used blocks, so it slightly overstates what was in use; the others are `shmem:max_used_size`.

## Registrar under a cold start: 1.2 million REGISTERs at 60,000 per second

Setup A, with REGISTERs instead of INVITEs: usrloc without a database, registrar, 4 UDP workers, each REGISTER answered 200 by the server. The elastic arena starts at 128 MB and has to grow to hold every registration.

| | F_MALLOC | F_PARALLEL_MALLOC | HP_MALLOC | HG_MALLOC `-m 128:4096` |
|---|---|---|---|---|
| shm size | `-m 4096` | `-m 4096` | `-m 4096` | starts at 128 MB |
| runs | 2 | 2 | 2 | 2 |
| registrations stored | 1,200,000 | 1,200,000 | 520,033 and 515,904 | **1,200,000** |
| UDP receive drops | 2.22 M and 1.92 M | 1.65 M and 1.30 M | 6.90 M and 6.97 M | **0 in every run** |
| sipp retransmissions | 2.34 M and 2.00 M | 1.69 M and 1.33 M | 8.65 M and 8.71 M | **0 in every run** |
| response p50 | 596 ms and 588 ms | 580 ms and 84 ms | 11.9 s and 12.0 s | **under 1 ms** |
| response p99 | 19.6 s and 19.5 s | 15.6 s and 15.5 s | 28.0 s | **4 ms** |
| worst response | 27.5 s | 27.5 s and 23.5 s | 28.0 s | **8-12 ms** |
| probe p99.9 | 87-90 ms | 73-74 ms | no reply (stopped after 20 timeouts in a row) | **5.6-6.0 ms** |
| CPU, cores | 4.00 (saturated) | 3.84-3.97 | 4.00 (saturated) | **1.90-2.05** |

With HP_MALLOC more than half the registrations failed: the 4 workers were saturated, and a profile of this test put 81% of their CPU inside `hp_shm_malloc`.

In both HG runs the arena grew from 128 MB to 1,696 MB in 98 steps with no lock stalls; the longest lock hold was 334 us and 971 us, and each grow's commit (up to 7.1 ms) ran with the lock released.

## Setup B: relayed calls over UDP, TLS and WSS

HG_MALLOC against F_MALLOC only. Rates are relayed calls per second (13 datagrams each), not comparable with the INVITE rates of setup A.

- **Proxy:** stateful and record-routing (tm, rr, `t_relay()` for every request), 8 workers per transport, `log_level=1`. HG_MALLOC `-m 512:4096 -M 64:512`, F_MALLOC `-m 2048 -M 32`. HG_MALLOC's startup log on this host names the tiers: the shared arena on plain 4 KB pages, because the host's `shmem_enabled` is `never`, and every private arena on THP; so both allocators kept their shared memory on 4 KB pages.
- **Load:** sipp on three hosts, four UAC/UAS pairs each, every call relayed to a UAS on the caller's host. TLS and WSS arrive over 24 long-lived connections from an OpenSIPS gateway on each client host, so TLS handshake cost is not measured.
- **Method:** 30-second cells, two repetitions per rate in each of the three receive modes of PR #4365 (`shared` being the current code), the server restarted for every cell. A rate holds when both repetitions complete at least 98% of the offered calls with at most 0.1% failed. Ranges below cover the three modes. Drops are the kernel's UDP receive-buffer errors on the server.

### Same rate, 512 KB socket buffer

| same rate, F_MALLOC -> HG_MALLOC | UDP 12,000/s | TLS 6,000/s | WSS 5,000/s |
|---|---|---|---|
| CPU per call | -25 to -32% | -9 to -11% | -7 to -8% |
| TCP workers' CPU | - | -18 to -23% | -16 to -21% |
| syscalls per call | -69 to -72% | -14 to -20% | -10 to -12% |
| `sched_yield` per call | - | -54 to -69% | -59 to -64% |
| failed calls | 0.2-0.8% -> 0 | 0 -> 0 | 0 -> 0 |

With the default 512 KB socket buffer the highest sustained rate rises from 10,000 relayed calls/s to 20,000 on the current receive path and 22,000 with PR #4365's `auto` (F_MALLOC: 13,000 at best, with an 8 MB socket buffer). F_MALLOC hits the buffer limit before the CPU limit: at 12,000/s the kernel drops datagrams while its workers are 63-84% busy. Its extra syscalls are `sched_yield` from the shared-memory lock's adaptive wait and grow with load (57 to 92 per call from 5,000 to 12,000/s), while HG_MALLOC's stay at 28-30 per call from 12,000 to 20,000/s: its common path takes no lock. Over TLS and WSS the TCP workers, which run the script, use 16-23% less CPU; TCP main, which does the TLS I/O, is within 3% with either allocator and sets the ceiling (5,000-8,000 calls/s), so there HG_MALLOC lowers the cost per call rather than raising the ceiling.

### UDP drops with an 8 MB socket buffer

The same proxy with the socket buffer of setup A, to see whether HG_MALLOC's zero drops hold for relayed calls. Measured on 2026-10-19: `maxbuffer` 4 MB, which the kernel doubles to 8 MB, read back from the live socket in every cell; drops counted over the whole cell; six cells per rate (three receive modes, two repetitions); one binary for both allocators.

| relayed calls/s | HG_MALLOC cells without a drop | drops per cell | failed calls per cell |
|---|---|---|---|
| 12,000 | 6 of 6 | 0 | 0 |
| 16,000 | 6 of 6 | 0 | 0 |
| 18,000 | 3 of 6 | 0-6,661 | 0 |
| 20,000 | 0 of 6 | 793-42,174 | 0 |
| 22,000 | 0 of 6 | 51,303-221,282 | 2-667; one cell completed 92% of its calls |

In the same run F_MALLOC with the same 8 MB buffer dropped 57,198 and 230,321 datagrams at 12,000/s (`shared`; 0 and 1,525 failed calls), and HG_MALLOC with the default 512 KB buffer dropped 9,827 (0 failed). With 8 MB, HG_MALLOC drops nothing up to 16,000 relayed calls/s (about 96,000 datagrams in per second) and starts to drop at 18,000, where F_MALLOC already drops at 12,000; up to 20,000 the retransmissions recover every drop.

Full tables, per-syscall counts and the rig: [udp-rx-mode-measurements, section 6](https://github.com/Lt-Flash/opensips/blob/udp-rx-mode-measurements/README.md#6-hg_malloc-vs-f_malloc-data-for-the-hg_malloc-pull-request-4327).

## Cost of the integrity checks

Round 3 of the review gave every cell header a state and a checksum, and a free now changes that state atomically; the CPU figures above were measured before. The two setups again, on 2026-10-22, HG_MALLOC alone: *before* is `bec2c23960`, the pull request before round 3; *round 3* is `82db4baa8d` in setup A and `96f536bc48` in setup B. Between those commits, and after them, only startup and grow checks changed (the memory cgroup's limit, counting above 4 GB on 32-bit builds), not the allocation path. The two builds of a table were built the same way with the same toolchain on their host, optimised, and alternate run by run.

**Setup A**, INVITEs answered by tm, with the settings of the hugetlb tier in [Backing tiers](#backing-tiers) (`-m 128:4096 -M 16:32`, 8 MB socket buffers), 60 s per point, three runs per rate:

| INVITEs/s | build | CPU, cores: mean (min-max) | vs before | failed INVITEs | UDP drops | sipp retransmissions, mean | INVITE p99 (probe), mean |
|---|---|---|---|---|---|---|---|
| 20,000 | before | 2.113 (2.046-2.153) | | 0 | 0 | 0 | 0.76 ms |
| | round 3 | 2.155 (2.110-2.227) | +2.0% | 0 | 0 | 0 | 0.68 ms |
| 45,000 | before | 3.832 (3.722-3.938) | | 0 | 0 | 30 | 1.28 ms |
| | round 3 | 3.836 (3.777-3.890) | +0.1% | 0 | 0 | 10 | 1.37 ms |
| 60,000 | before | 4.602 (4.573-4.640) | | 0 | 0 | 442 | 3.43 ms |
| | round 3 | 4.720 (4.649-4.763) | +2.6% | 0 | 0 | 64 | 3.30 ms |

**Setup B**, relayed calls over UDP: the proxy, load and 30 s cells of [setup B](#setup-b-relayed-calls-over-udp-tls-and-wss), HG_MALLOC `-m 512:4096 -M 64:512`, the 8 MB socket buffer (read back from the live socket in every cell), four runs per rate. The build is the pull request's own tree - OpenSIPS master plus HG_MALLOC, so the current receive path, without PR #4365. CPU is that of every OpenSIPS process over 15 s of each cell, per 1,000 calls/s relayed in those 15 s, in percent of one core; drops are counted over the whole cell. The load generators split each rate over 12 instances, so the offered total is rounded.

| relayed calls/s | build | CPU per 1,000 calls/s: mean (min-max) | vs before | calls completed per run (offered) | failed calls per run | UDP drops per run |
|---|---|---|---|---|---|---|
| 12,000 | before | 34.15% (33.84-34.54) | | 360,000 of 360,000 | 0 | 0 |
| | round 3 | 34.42% (34.05-34.71) | +0.8% | 360,000 | 0 | 0 |
| 16,000 | before | 31.62% (31.40-31.91) | | 479,880 of 479,880 | 0 | 0 |
| | round 3 | 32.02% (31.75-32.31) | +1.3% | 479,880 | 0 | 0 |
| 20,000 | before | 29.78% (29.64-29.94) | | 600,120 of 600,120 | 0 | 0-6,231 |
| | round 3 | 30.17% (29.75-30.79) | +1.3% | 600,120 | 0 | 0-29,903 |
| 22,000 | before | 29.17% (28.39-29.69) | | 659,765-659,880 of 659,880 | 0-115 | 40,176-107,225 |
| | round 3 | 29.85% (29.22-30.20) | +2.3% | 659,767-659,880 | 0-113 | 43,371-96,785 |

The checks cost 0.1-2.6% of the server's CPU. The two builds' ranges overlap at every rate but 60,000 INVITEs/s. Neither build drops a datagram or fails a call up to 16,000 relayed calls/s. At 20,000, past the 18,000 where this proxy starts to drop with an 8 MB buffer (above), drops vary from run to run in both builds: the two builds' third runs, back to back, dropped the most (6,231 and 29,903), the others 0-2,011 and 0-233. At 22,000 both are past the buffer's limit and drop alike. Four runs per rate do not separate the two builds on drops.

## Integrity

- **Soak:** hgstress, 9 workers each doing 2 million stamped alloc/free pairs across every size class and the large tier, verifying stamps while the others churn: 0 torn words, 0 corruption, under fixed and elastic arenas.
- **Fault injection:** a double free and a scribbled free-list link injected into a running process are both caught and counted (`double_free`, `foreign_ptr`), and the process keeps serving.
- **Memory lock limit:** with `CAP_IPC_LOCK` dropped and `RLIMIT_MEMLOCK` at 64 MB, an elastic arena on 4 KB pages pins its 32 MB start, warns once, and keeps growing unpinned - to 368 MB under hgstress, with no failed allocation. On hugetlb pages, where growth needs the lock, refused grows name the limit; with the limit lifted the arena grows normally.

## Builds

| target | result |
|---|---|
| x86_64, gcc 14 | clean |
| x86_64, clang, `-Werror` | clean |
| HG_MALLOC compiled out | clean |
| arm32, arm64 (cross) | compile and link |
| i386 (container) | clean; hgstress soak passes |
| Alpine / musl (container) | clean; hgstress soak passes |
| OpenSIPS CI on the PR (all compilers, architectures, unit tests, fuzzing) | passes |
