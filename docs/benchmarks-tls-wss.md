# Benchmarks: calls relayed over TLS and WSS

Highest rate held in both runs:

| allocator | TLS, calls/s | WSS, calls/s |
|---|---|---|
| F_MALLOC | 8,000 | 8,000 |
| F_PARALLEL_MALLOC | 12,000 | 12,000 |
| HP_MALLOC | 8,000 | 8,000 |
| **HG_MALLOC** | **14,000** | **12,000** |

Where both held the same rate, HG_MALLOC used 14-23% fewer cores than the other allocator, and its TCP workers 30-38% less CPU. At 16,000 calls/s, which neither held, HG_MALLOC failed 0.07-0.19% of the calls over TLS and 6.6-9.2% over WSS, F_PARALLEL_MALLOC 8.8-9.6% and 42.9-47.8%.

## How it was measured

Measured on 2026-11-07 and 08 on this PR's code (built from `6293c4afa6` with the TLS and WSS modules, optimised, every allocator in one binary). Server: bare metal, 2 x Xeon E5-2690 v2 (40 threads, 2 NUMA nodes), Debian 13, Linux 6.12; OpenSIPS pinned to 16 CPUs and the memory of NUMA node 0, the NIC interrupts on four other CPUs of that node; 8 UDP workers and 8 TCP workers; a fixed 64 GB shared arena and 64 MB private arenas for every allocator, all on 4 KB pages (F_PARALLEL_MALLOC as `-a F_MALLOC -s F_PARALLEL_MALLOC`; HG_MALLOC's tier read from `core:hg_stats` in every run). The server is a record-routing stateful proxy: `record_route()` and `t_relay()`.

Load: sipp on four 16-vCPU VMs, one UAC per 100 calls/s, each with its own ports, and ten UAS per VM. Calls: INVITE, 180, 200, ACK and a BYE 100 ms later. Each VM runs an OpenSIPS gateway that takes its UACs' calls over UDP and carries them to the server over eight TLS or eight WSS connections; the server relays every call over UDP to a UAS on the caller's VM. The load generators, gateways included, stayed at or below 60% CPU in every run that held.

Gateways: for the TLS runs of 2026-11-07, OpenSIPS built from `151e418b3e`; for the runs of 2026-11-08, upstream master `61faa5d141` with the fix from [#4434](https://github.com/OpenSIPS/opensips/pull/4434) and a send queue of 1,024 chunks per WSS connection instead of the fixed 32 (a local patch adding `wss_async_max_postponed_chunks`). Without #4434 the gateways failed some of their own WebSocket handshakes; with 32 chunks their WSS send queues overflowed at 12,000 calls/s. Each run of 2026-11-08 started with an unmeasured warm-up, 8 UACs per VM at 25 calls/s for 2 s, so that the gateways' connections were open before the measured load.

Two 30 s runs per allocator and rate, the allocators' order reversed for the second; an allocator leaves the ladder at the first rate where neither run holds. A run holds when every UAC reported, at least 98% of the offered calls completed, at most 0.1% failed, the OpenSIPS log has no ERROR or CRITICAL line and no gateway logged one. CPU is the user and system time of every OpenSIPS process over a 15 s window inside each run, mean of the two runs; the TCP workers column is the same for the 8 TCP worker processes. UDP drops are the kernel's receive-buffer errors over the whole run, mean of the two. The server log column counts the OpenSIPS log's ERROR and CRITICAL lines and, after the comma, the memory errors among them, per run; the gateway column counts the ERROR and CRITICAL lines of all four gateways, per run.

## TLS, all four allocators

Measured on 2026-11-07.

| calls/s | allocator | cores | TCP workers, cores | UDP drops | failed calls, per run | server log ERROR, memory errors, per run | gateway errors, per run | runs held |
|---|---|---|---|---|---|---|---|---|
| 4,000 | F_MALLOC | 3.15 | 1.13 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 4,000 | F_PARALLEL_MALLOC | 3.17 | 1.12 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 4,000 | HP_MALLOC | 3.18 | 1.17 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 4,000 | HG_MALLOC | 2.61 | 0.75 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | F_MALLOC | 7.31 | 2.63 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | F_PARALLEL_MALLOC | 6.88 | 2.46 | 1 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | HP_MALLOC | 6.99 | 2.59 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | HG_MALLOC | 5.61 | 1.64 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 12,000 | F_MALLOC | 11.97 | 3.63 | 2,790 | 0.664% / 0.580% | 291,029, 0 / 237,032, 0 | 626 / 514 | 0 of 2 |
| 12,000 | F_PARALLEL_MALLOC | 11.42 | 4.02 | 3,095 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 12,000 | HP_MALLOC | 11.83 | 3.77 | 6,899 | 0.892% / 0.910% | 264,847, 0 / 277,835, 0 | 663 / 806 | 0 of 2 |
| 12,000 | HG_MALLOC | 9.32 | 2.71 | 1,789 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 16,000 | F_PARALLEL_MALLOC | 11.44 | 3.29 | 9,070 | 8.755% / 9.555% | 614,484, 0 / 593,029, 0 | 2,015 / 2,046 | 0 of 2 |
| 16,000 | HG_MALLOC | 11.30 | 3.04 | 5,544 | 0.067% / 0.188% | 176,962, 0 / 190,038, 0 | 315 / 442 | 0 of 2 |

## TLS, F_PARALLEL_MALLOC and HG_MALLOC from 12,000 calls/s

Measured on 2026-11-08.

| calls/s | allocator | cores | TCP workers, cores | UDP drops | failed calls, per run | server log ERROR, memory errors, per run | gateway errors, per run | runs held |
|---|---|---|---|---|---|---|---|---|
| 12,000 | F_PARALLEL_MALLOC | 11.27 | 3.88 | 3,419 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 12,000 | HG_MALLOC | 9.35 | 2.72 | 1,975 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 13,000 | F_PARALLEL_MALLOC | 11.91 | 4.12 | 3,979 | 0.001% / 0.009% | 9,870, 0 / 11,396, 0 | 5 / 8 | 0 of 2 |
| 13,000 | HG_MALLOC | 10.32 | 3.00 | 3,024 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 14,000 | HG_MALLOC | 11.16 | 3.25 | 4,012 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 15,000 | HG_MALLOC | 11.33 | 3.23 | 5,679 | 0.003% / 0.002% | 9,621, 0 / 25,526, 0 | 4 / 8 | 0 of 2 |

## WSS, all four allocators

Measured on 2026-11-08.

| calls/s | allocator | cores | TCP workers, cores | UDP drops | failed calls, per run | server log ERROR, memory errors, per run | gateway errors, per run | runs held |
|---|---|---|---|---|---|---|---|---|
| 4,000 | F_MALLOC | 3.13 | 1.11 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 4,000 | F_PARALLEL_MALLOC | 3.13 | 1.09 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 4,000 | HP_MALLOC | 3.07 | 1.09 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 4,000 | HG_MALLOC | 2.63 | 0.75 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | F_MALLOC | 7.08 | 2.51 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | F_PARALLEL_MALLOC | 6.94 | 2.42 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | HP_MALLOC | 6.74 | 2.38 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 8,000 | HG_MALLOC | 5.68 | 1.64 | 0 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 12,000 | F_MALLOC | 13.77 | 3.74 | 789,750 | 26.964% / 30.076% | 1,135,941, 0 / 1,308,636, 0 | 375 / 478 | 0 of 2 |
| 12,000 | F_PARALLEL_MALLOC | 11.42 | 3.95 | 2,646 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 12,000 | HP_MALLOC | 13.26 | 3.73 | 627,812 | 19.915% / 22.115% | 1,034,356, 0 / 1,010,731, 0 | 747 / 1,218 | 0 of 2 |
| 12,000 | HG_MALLOC | 9.31 | 2.70 | 1,872 | 0.000% / 0.000% | 0, 0 / 0, 0 | 0 / 0 | 2 of 2 |
| 16,000 | F_PARALLEL_MALLOC | 14.29 | 3.71 | 1,185,962 | 47.788% / 42.923% | 1,415,735, 0 / 1,243,790, 3 | 1,473 / 2,259 | 0 of 2 |
| 16,000 | HG_MALLOC | 11.65 | 3.05 | 573,636 | 9.199% / 6.557% | 713,988, 0 / 639,217, 0 | 455 / 535 | 0 of 2 |
