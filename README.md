# udp_rx_mode / tcp_dispatch_mode - what was measured and how

Companion to the OpenSIPS pull request "core: udp_rx_mode / tcp_dispatch_mode - one wakeup per
packet instead of a thundering herd". Everything here was measured in October 2026 on
OpenSIPS 4.1.0-dev builds of this patch on a 4.1 tree that merges upstream master ee74a3123e with
other work in progress (notably the HG_MALLOC allocator of #4327); on Linux the receive-path code
under test is the same as in the pull request. The tools are in `tools/`.

## 1. What was observed in production

A registrar with 8 UDP workers (16 vCPU KVM guest, Linux 5.4, ~900 SIP packets/s) showed in its
process-load statistics a fixed ladder: worker 14 at ~27%, 13 at ~19%, 12 at ~13%, ... worker 7 at
~1%. Read-only probes of `/proc/<pid>/status` showed every one of the 8 UDP workers making
1000-1250 voluntary context switches per second, i.e. each packet woke all 8 workers (13
rescheduling IPIs per packet), while CPU share rose monotonically with fork order. Three SBCs
(4 UDP + 4 TCP workers each) showed the same ladder on their UDP workers and on their TCP workers.

## 2. Why (kernel side)

OpenSIPS UDP workers do not block in `recvfrom()`; each runs a reactor and registers the one
inherited, non-blocking listen socket in its private epoll with plain `EPOLLIN`.

* epoll registers on the socket's wait queue with `add_wait_queue()` = non-exclusive, inserted at
  the head of the list (`kernel/sched/wait.c`), so the worker that registered last (forked last)
  is first.
* A datagram arrival runs `__wake_up_common` over that list and calls every non-exclusive entry:
  every idle worker wakes, one wins `recvfrom()`, the others get EAGAIN. That is the herd.
* The wakeups happen head-to-tail, so the last-forked worker usually wins; when it is busy the
  next one wins, and so on - the geometric ladder.
* `EPOLLEXCLUSIVE` was tried upstream (e51a5f067d) and disabled (3596efdc00): before Linux 6.0
  (`ep_autoremove_wake_function`, fs/eventpoll.c) a woken but not yet running worker keeps
  absorbing the following events, so bursts serialize on one worker.
* The TCP workers share `tcp_dispatch_sock` (a SOCK_DGRAM socketpair) the same way.

The kernel sources checked: v5.4 and v6.12 `fs/eventpoll.c` (`ep_ptable_queue_proc`,
`ep_poll_callback`, `ep_poll`) and `kernel/sched/wait.c` (`add_wait_queue`, `__wake_up_common`).

## 3. How it was measured

* **Per-process wakeups and CPU**: `tools/probe3.py` / `tools/probe4.py` sample every OpenSIPS
  process over a window (12 s unless stated): utime+stime from `/proc/<pid>/stat`,
  `voluntary_ctxt_switches` from `/proc/<pid>/status` (= wakeups), UDP datagrams in from
  `/proc/net/snmp`, rescheduling IPIs from `/proc/interrupts`, softirq time from `/proc/stat`.
  "wakeups per packet" = sum of all OpenSIPS voluntary context switches / incoming datagrams.
  `probe4.py` names the processes through MI `ps`.
* **Spread** = per-worker CPU% of one listener's workers in fork order, and its coefficient of
  variation (population stdev / mean).
* **Load**: sipp OPTIONS -> `sl_send_reply(200)` (`tools/options.xml`), one source socket per
  call (`-t un`) or one socket (`-t u1`); `tools/pace.c` for single-datagram Poisson arrivals from
  256 source ports (the production arrival shape - sipp batches its sends, which hides the herd);
  `tools/burst.c` for N simultaneous datagrams from N sockets, measuring per-reply latency and
  burst completion time.
* **TCP main syscalls**: `strace -c -f` on the TCP main process for 8 s under load.
* Every arm passes `-a F_MALLOC` (or the node's own allocator flags) explicitly; the binary's
  md5 and the live `/proc/<pid>/cmdline` were recorded per arm.

## 4. Results

### 4.1 Bench, 8 UDP workers, paced Poisson arrivals at 1000 datagrams/s (Debian 13, kernel 6.12, 16 vCPU; client on another host)

| mode | wakeups / packet | OpenSIPS CPU | softirq per packet | worker CPU% by fork order | spread cv |
|---|---|---|---|---|---|
| shared (before) | 7.17 | 15% | 54 us | 1.2 1.4 1.1 1.2 1.4 2.2 2.3 4.3 | 55% |
| reuseport (after, `auto`) | 1.09 | 8% | 24 us | 1.0 0.8 1.0 1.0 1.0 0.9 0.9 0.9 | 7% |
| exclusive | 1.12 | 6% | 17 us | 5.4 0.8 0 0 0 0 0 0 | first-idle-in-fixed-order |
| reuseport with the kernel hash instead of the cBPF selector | 1.09 | 8% | 12 us | flat with many sources; with ONE source 9.9 0 0 0 0 0 0 0 | - |

At 3000/s: shared 6.5 wakeups/packet, 32% CPU, ladder 2.7..8.9%; reuseport 1.0, 20%, flat 2.4-2.6%.
Every UDP worker in "shared" made ~890 (1000/s) and ~2450 (3000/s) context switches per second,
the same one-wakeup-per-packet-per-worker pattern as production.

### 4.2 Kernel 5.4 (production's kernel; server and client on the same 16 vCPU host)

Paced 1000/s: shared ~8 wakeups per request, ladder 4.7..7.0%; reuseport 1 per request, flat
1.2-1.3%; exclusive 1 per request but all on one worker.

Bursts of 32 simultaneous datagrams (200 bursts), reply latency / burst completion, us:

| mode | p50 / p99 / max | completion p50 / p90 |
|---|---|---|
| shared | 258 / 1343 / 1954 | 578 / 1427 |
| exclusive | 205 / 438 / 634 | 500 / 612 (the burst is served by ONE worker - pre-6.0 epoll) |
| reuseport | 125 / 314 / 470 | 321 / 423 |

### 4.3 Six-node test cluster with the real configurations (3 SBC: 2 vCPU KVM, kernel 5.4; 3 registrars: LXC on a 7.0 kernel)

Deployed with the parameters at their defaults (`auto`): every UDP listener got one SO_REUSEPORT
socket per possible worker (64 on the SBCs, the scaling profile's maximum), TCP dispatch
per worker. Canary soaks clean, 100% of calls completed and 0 retransmissions in every cell.

With the private listener fixed at 8 workers (same config in both arms), 1000 OPTIONS/s per SBC
from three load hosts, 12 s windows:

| SBC | wakeups / packet before -> after | OpenSIPS CPU before -> after | 8-worker spread cv before -> after | before: last-forked workers |
|---|---|---|---|---|
| 1 | 4.28 -> 3.09 (-28%) | 22.5% -> 22.2% | 5% -> 2% | 3.0, 3.0 vs 2.6-2.7 |
| 2 | 6.37 -> 3.21 (-50%) | 25.7% -> 25.2% | 19% -> 5% | 4.7, 3.2 vs 2.8-2.9 |
| 3 | 4.28 -> 3.09 (-28%) | 24.9% -> 23.3% | 8% -> 4% | 3.7, 3.2 vs 2.9-3.0 |

At 2000/s: 3.1/4.3/3.1 -> 2.8/2.9/2.7 wakeups per packet, cv 3/11/3% -> 1/2/2%, CPU -2..-5%.
The CPU saving is small on these SBCs because every OPTIONS does a synchronous cache lookup and
several log lines, which dwarf the wakeup cost; the remaining ~3 wakeups per packet are that
lookup's own sleep/wake plus timers. As configured (2 workers + auto-scaling) the spread was
already flat (cv 0-5%) because the workers sleep inside the lookup rather than in epoll.

### 4.4 TCP dispatch (bench, 4 TCP workers, 3 repeats per mode, strace of TCP main)

| cell | mode | TCP main CPU (3 reps) | TCP workers CPU% | spread cv | worker wakeups/s each | TCP main syscalls in 8 s (futex / epoll_wait / sendto / write / read) |
|---|---|---|---|---|---|---|
| 400 connections/s, one per call | shared | 13.2 13.4 13.1 | 1.2 1.3 1.6 1.9 | 18% | 670-684 | 27560 / 12701 / 6380 / 12760 / 19911 |
| | per_worker | 13.0 12.8 12.8 | 1.1 1.3 1.3 1.3 | 7% | 497-503 | 27611 / 11773 / 6398 / 12794 / 19281 |
| one connection, 2000 msg/s | shared | 7.3 7.0 7.4 | 2.1 2.1 2.2 2.2 | 2% | 257-290 | 16783 / 6238 / 31984 / 5641 / 11182 |
| | per_worker | 7.3 7.4 8.0 | 2.1 2.3 2.1 2.0 | 5% | 257-267 | 16832 / 6218 / 31984 / 5652 / 11012 |

TCP main's cost and syscall mix do not change; under connection churn the workers flatten and
each makes ~28% fewer wakeups.

### 4.5 Auto-scaling, allocators

Profile 2 -> up to 6 workers: scaled 2 -> 6 under load, each dynamic worker reading its own
socket (epoll target fd -> distinct UDP inodes), ripped back to 2 when idle; after that a burst
of 32 x 50 datagrams got 1600/1600 replies and all six socket queues were empty (no stranded
datagrams). Static and dynamic workers share evenly (4.0-5.2% each, 275-285 wakeups/s each at
8000/s). Q_MALLOC_DBG, HP_MALLOC_DBG and F_MALLOC_DBG start and serve with the per-worker sockets.

### 4.6 Call-rate capacity (CPS) through a stateful proxy

**Question:** how many calls per second does OpenSIPS sustain in each mode, and do the saved
context switches and syscalls turn into capacity?

**Setup.** Server: KVM guest, 16 vCPU (Xeon E5-2699 v4), Debian 13, Linux 6.12 (so `exclusive` is
genuinely available). OpenSIPS 4.1 with this patch, built from a 4.1 tree that also carries the
HG_MALLOC allocator proposed in #4327 (not in master), as a stateful record-routing proxy (`tm`,
`rr`, `t_relay()` for every request; `tools/cps/srv_cps.cfg.tmpl`), 8 UDP workers, `log_level=1`.
Each call is INVITE / 100 / 180 / 200 / ACK / BYE / 200 through the proxy: 6 datagrams in and 7
out per call. Load: sipp on three hosts, four UAC/UAS pairs each; every UAC calls its own host's
UAS through the proxy (`-rsa`), with 100 ms between ACK and BYE (`tools/cps/uac_rr.xml`). Late
180s and late ACKs are ignored (`-default_behaviors all,-abortunexp`), because a multi-worker
proxy may legally forward back-to-back messages out of order. Client sipp CPU stayed below 40%.

**Method.** 30-second cells; at each rate the three modes are interleaved, ABC then CBA (two
repetitions), with a server restart per cell, and the mode is verified on the live server in every
cell (sockets on the port, `EPOLLEXCLUSIVE` registrations, socket buffer sizes, allocator).
A 15-second window in the middle of each cell records per-process CPU and wakeups from `/proc`,
and `perf stat` on all OpenSIPS tasks (context switches, all syscalls, `recvfrom` returning
`EAGAIN`). A rate **holds** when both repetitions complete at least 98% of the offered calls
within the cell with at most 0.1% failed calls. Driver and analysis: `tools/cps/`.

**Highest sustained rate (calls/s):**

| configuration | shared (before) | auto = reuseport | exclusive |
|---|---|---|---|
| F_MALLOC (`-m 2048 -M 32`), default `maxbuffer` (256 KB) | 10,000 | 10,000 | 10,000 |
| F_MALLOC, `maxbuffer` 4 MB (8 MB socket buffers) | 10,000 | **13,000 (+30%)** | 12,000 (+20%) |
| HG_MALLOC (`-m 512:4096 -M 64:512`, transparent huge pages), default `maxbuffer` | 20,000 | **22,000 (+10%)** | 18,000 (-10%) |

**Cost per call at a rate every mode holds:**

| | shared (before) | auto = reuseport | exclusive |
|---|---|---|---|
| HG_MALLOC, 14,000/s: CPU % per 1,000 calls/s | 34.4 | 30.3 (-12%) | 31.8 (-8%) |
| HG_MALLOC, 14,000/s: context switches per call | 5.54 | 1.86 (-66%) | 2.46 (-56%) |
| HG_MALLOC, 14,000/s: syscalls per call | 28.4 | 24.1 (-15%) | 25.9 (-9%) |
| HG_MALLOC, 14,000/s: `recvfrom` returning `EAGAIN`, per call | 0.94 | 0 | 0.34 |
| F_MALLOC, 10,000/s: CPU % per 1,000 calls/s | 49.3 | 45.1 (-9%) | 46.1 (-6%) |
| F_MALLOC, 10,000/s: context switches per call | 6.50 | 1.97 (-70%) | 2.83 (-56%) |
| F_MALLOC, 10,000/s: syscalls per call | 79.7 | 64.4 (-19%) | 79.7 (0%) |

**What limits each configuration.**

* F_MALLOC with the default buffer: UDP receive-buffer overflow rather than CPU. At 12,000/s the
  workers ran at 63-78% while the kernel dropped tens of thousands of datagrams per 15-second
  window, and the dropped datagrams are the failed calls. `auto` failed the fewest calls (0.17% versus
  0.57-0.69%), but no mode held 12,000.
* With 4 MB buffers the modes separate: `auto` holds 13,000/s (37 failed calls out of 780,000),
  `shared` already misses at 12,000 (0.20% failed) and `exclusive` at 13,000 (1.1%). At 14,000/s
  every mode is CPU-bound: context switches per call fall to almost zero because the workers never
  sleep.
* Under F_MALLOC most syscalls are `sched_yield` from contention on the shared-memory lock (a
  per-syscall count at 10,000/s: 45 per call in `shared`, 43 in `auto`, 70 in `exclusive`, out of
  62-89 in total). HG_MALLOC's lock-free fast path brings that down to 5-7 per call, which is why it
  doubles capacity and also drains the default buffer fast enough.
* `exclusive` hands work to the first idle workers in a fixed order (per-worker CPU spread with a
  coefficient of variation of 11-22% versus 2-5% for `auto`), so its busiest workers contend more
  (+53% `sched_yield` under F_MALLOC) and it saturates first under HG_MALLOC.

**Reading.** Fewer context switches and syscalls do turn into capacity: `auto` (one SO_REUSEPORT
socket per worker) sustains the most calls per second in every configuration tested, +30% over
`shared` with F_MALLOC and large buffers and +10% with HG_MALLOC, and it costs 9-17% less CPU per
call wherever all modes hold. `exclusive` saves wakeups but loses capacity to its uneven
distribution. The allocator matters more than the receive mode: HG_MALLOC alone takes `shared` from
10,000 to 20,000 calls/s on the same server.

**Caveats.** One server class with a single virtio NIC, and 30-second cells, so this measures
sustained short-run capacity, not an hours-long soak. Rates were stepped 1,000-2,000 calls/s apart,
so each figure means "holds here, fails at the next step". The per-syscall counts come from
separate cells with every syscall tracepoint enabled, which slows the workers; they show which
syscall dominates, not absolute context-switch rates (the ladder cells are the reference for
those). The completed rate is successful calls divided by 30 s; a datagram-based rate inflates near
overload because of retransmissions and was not used for the verdicts.

## 5. Trade-off

Per-worker queues are not work-conserving: a worker blocked in a slow synchronous step keeps its
1/N share of new packets waiting while others are idle, where the shared socket let any idle
worker take them. `udp_rx_mode = shared` keeps the old behaviour; `exclusive` (6.0+ kernels)
removes the herd while keeping the shared queue, at the price of a first-idle-in-fixed-order
distribution.
