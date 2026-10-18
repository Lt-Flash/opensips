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

### 4.7 Call-rate capacity over TLS and WSS

**Question:** the same as 4.6, with the calls arriving over TLS or secure WebSocket, where the
receive mode in use is `tcp_dispatch_mode` (one shared socketpair versus one per TCP worker)
rather than the UDP sockets.

**Setup.** Same server, build, proxy script and allocator settings as in 4.6, with
`udp_workers=8`, `tcp_workers=8` and listeners `tls:5061` and `wss:7443`
(`tools/tls/srv_tls.cfg.tmpl`). sipp cannot drive TLS or WSS at these rates, so every client host
runs a local OpenSIPS gateway (`tools/tls/gw.cfg.tmpl`, HG_MALLOC): the eight sipp UACs on the
host send UDP to eight gateway sockets, and each gateway socket relays over its own TLS (or WSS)
connection to the server. The server relays the call to the UAS over UDP, so every call puts
three requests and four replies through the server's TLS or WSS connections. Each client host
therefore holds 8 long-lived TLS or WSS connections, 24 in all: few connections, many calls on
each.

Two sets of `proto_tls` / `proto_wss` settings were used, on both ends:

* **defaults**, for the first two TLS runs;
* **the settings of our staging SBCs**, for everything else:
  `tls_max_msg_chunks` / `wss_max_msg_chunks` 32, handshake and send timeouts 1,500 ms, TLS 1.2
  to 1.3. The gateways also need `tls_async_max_postponed_chunks` 1024, or their own write queue
  overflows first.

WSS did not connect at all with the default `wss_max_msg_chunks` (4): an OpenSIPS WSS client
writes its upgrade request as 16 TLS records, one per iovec element, and the server gives up
after 4 read attempts. All WSS results are therefore with the staging settings.

**Method.** As in 4.6 (30-second cells, interleaved modes, two repetitions at the rates that held,
mode verified on the live server in every cell), starting at 10,000 calls/s and stepping down
through 8,000, 6,000 and 5,000. The UDP receive mode stays `auto` on the server in every cell;
only `tcp_dispatch_mode` changes: `shared` (one socketpair for all TCP workers, as before),
`auto` (one socketpair per TCP worker) and `exclusive` (the one shared socketpair, with every TCP
worker registered on it with `EPOLLEXCLUSIVE`). Each cell checked the socketpair count and the
eight exclusive registrations on the live server.

**Highest sustained rate (calls/s):**

| transport, settings | allocator | shared (before) | auto = per-worker | exclusive |
|---|---|---|---|---|
| TLS, defaults | F_MALLOC | 6,000 | 6,000 | 6,000 |
| TLS, defaults | HG_MALLOC | 6,000 | **8,000** | 6,000 |
| TLS, staging | HG_MALLOC | 6,000 | 6,000 (8,000 failed once, held once) | 6,000 |
| WSS, staging | F_MALLOC | 6,000 | 5,000 | 6,000 (failed at 5,000 once) |
| WSS, staging | HG_MALLOC | 5,000 | 5,000 (6,000 held in 3 of 5 cells) | 5,000 |

The steps are 2,000 calls/s apart above 6,000, so these figures are coarse. Three of them sit on
an edge: TLS staging `auto` failed at 8,000 in the ladder and held in a later cell with the same
build, WSS HG_MALLOC `auto` held 6,000 in 3 of 5 cells, and WSS F_MALLOC `exclusive` held 6,000
twice but failed at 5,000. Within this resolution the dispatch mode does not move the ceiling.

**Cost per call at a rate every mode holds:**

| | shared (before) | auto = per-worker | exclusive |
|---|---|---|---|
| TLS F_MALLOC, 6,000/s: CPU % per 1,000 calls/s | 128.9 | 119.3 (-7%) | 113.6 (-12%) |
| TLS F_MALLOC, 6,000/s: context switches per call | 28.6 | 22.4 (-22%) | 21.5 (-25%) |
| TLS HG_MALLOC, 6,000/s: CPU % per 1,000 calls/s | 116.1 | 106.0 (-9%) | 103.5 (-11%) |
| TLS HG_MALLOC, 6,000/s: context switches per call | 28.7 | 21.0 (-27%) | 21.1 (-27%) |
| TLS HG_MALLOC, 6,000/s: TCP main CPU % | 358 | 333 (-7%) | 336 (-6%) |
| WSS F_MALLOC, 5,000/s: CPU % per 1,000 calls/s | 160.6 | 149.2 (-7%) | (cell failed) |
| WSS F_MALLOC, 5,000/s: context switches per call | 36.4 | 29.0 (-20%) | (cell failed) |
| WSS HG_MALLOC, 5,000/s: CPU % per 1,000 calls/s | 149.4 | 137.3 (-8%) | 130.8 (-12%) |
| WSS HG_MALLOC, 5,000/s: context switches per call | 36.1 | 28.5 (-21%) | 27.8 (-23%) |
| WSS HG_MALLOC, 5,000/s: TCP main CPU % | 446 | 424 (-5%) | 404 (-9%) |
| all rows above: empty reads (`EAGAIN`) per call | 1.2-1.4 | 0 | 0.19-0.24 |
| all rows above: TCP worker CPU spread, coefficient of variation | 24-29% | 0-1% | 62-74% |

The `exclusive` ranges leave out the failed WSS F_MALLOC cell. The TLS rows are from the runs with default settings; with the staging settings the HG_MALLOC
TLS figures are within 3% of these. The WSS HG_MALLOC figures for `shared` and `auto` come from
one repetition each, because the `/proc` sampler returned nothing in the other.

**What limits TLS and WSS.**

* TCP main, not the workers. TCP main (with its I/O threads) runs at 3.3-5.1 CPUs at these rates,
  while each TCP worker stays at 3-57% of one CPU. Above the TLS ceiling there are no dispatch
  drops at all; instead the server's log fills with `no TLS client domain found` from TCP main.
  TCP main is trying to open new outgoing TLS connections, which means replies to the client
  gateways no longer found the connections their requests came in on. In the one failed WSS
  cell whose full server log was kept (6,000/s), the sequence starts with the gateways resetting
  their connections (64 `Connection reset by peer`, 75 failed writes), followed by 26,624 replies
  that found no connection. Whether a gateway gave up because it was itself overloaded or because
  the server read too slowly (the gateways' send timeout is 1,500 ms) was not established; the
  gateways' logs were not kept.
* WSS additionally overflows the hand-off from TCP main to the workers (`failed to dispatch TCP
  message to worker socket: Resource temporarily unavailable`). The hand-off is an AF_UNIX
  datagram socket whose queue is capped by `net.unix.max_dgram_qlen` (512 here), and a message
  that finds it full is dropped. With the one shared queue this already happens in cells that
  still hold: 243-1,400 dropped messages per cell in `shared` and `exclusive` at 6,000/s with
  F_MALLOC, 54 in one `exclusive` cell at 5,000/s with HG_MALLOC (the clients' retransmissions
  recover them). `auto` dropped nothing in any cell that held. One of its two failed cells at
  6,000/s (HG_MALLOC) dropped 8,659 messages on individual workers' queues, the other none, and
  above 6,000 every mode drops tens of thousands.

**Reading.** Over TLS and WSS the dispatch mode changes the cost, not the ceiling. One socketpair
per TCP worker (`auto`) removes the empty reads, cuts context switches per call by 20-27% and CPU
per call by 7-9%, spreads the work evenly over the TCP workers, and is the only mode that
dropped no WSS messages below its ceiling. Some of the saving is in TCP main itself (5-7%),
consistent with the sender paying for waking every idle worker on the shared socketpair.
`exclusive` costs as little or less (11-12% below `shared`) but loads the workers in a fixed
order (one worker at 50%, the last at 3%) and still drops WSS messages from its one queue. The
ceiling is set by TCP main and, for WSS, by the dispatch queue. HG_MALLOC lowers the cost per
call here too (section 6) without raising the ceiling, apart from the one TLS `auto` result at
8,000.

**Caveats.** As in 4.6, plus: the client side is itself OpenSIPS, so a gateway limit can look like
a server limit. The gateways' busiest process used 1.5-1.8 CPUs, but their logs were not
collected. Connections are few and long-lived, so TLS handshake cost is
not measured. Driver and analysis: `tools/tls/`.

## 5. Trade-off

Per-worker queues are not work-conserving: a worker blocked in a slow synchronous step keeps its
1/N share of new packets waiting while others are idle, where the shared socket let any idle
worker take them. `udp_rx_mode = shared` keeps the old behaviour; `exclusive` (6.0+ kernels)
removes the herd while keeping the shared queue, at the price of a first-idle-in-fixed-order
distribution.

## 6. HG_MALLOC vs F_MALLOC (data for the HG_MALLOC pull request, #4327)

Every allocator comparison made while measuring the receive modes, gathered in one place. Same
server (KVM guest, 16 vCPU Xeon E5-2699 v4, Debian 13, Linux 6.12), same build (the 4.1 tree that
carries #4327, plus this patch), same proxy script and load as in 4.6/4.7, so the allocator is the
only difference between the arms. HG_MALLOC ran as `-a HG_MALLOC -m 512:4096 -M 64:512` with
`ulimit -l unlimited`; the host has no hugetlb pool, so HG used its transparent-huge-page tier
(about 940 MB of THP in use under load). F_MALLOC ran as `-a F_MALLOC -m 2048 -M 32`.

**Highest sustained calls/s (stateful proxy, 8 workers per transport):**

| transport (receive buffers / tuning) | allocator | shared | auto | exclusive |
|---|---|---|---|---|
| UDP, default buffers | F_MALLOC | 10,000 | 10,000 | 10,000 |
| UDP, default buffers | HG_MALLOC | **20,000** | **22,000** | **18,000** |
| UDP, 4 MB buffers | F_MALLOC | 10,000 | 13,000 | 12,000 |
| TLS, default proto_tls settings | F_MALLOC | 6,000 | 6,000 | 6,000 |
| TLS, default proto_tls settings | HG_MALLOC | 6,000 | 8,000 | 6,000 |
| TLS, staging tuning (see 4.7) | HG_MALLOC | 6,000 | 6,000 | 6,000 |
| WSS, staging tuning | F_MALLOC | 6,000 | 5,000 | 6,000 (failed once at 5,000) |
| WSS, staging tuning | HG_MALLOC | 5,000 | 5,000 | 5,000 |

**Same offered rate, F_MALLOC -> HG_MALLOC:**

| | shared | auto | exclusive |
|---|---|---|---|
| UDP 12,000/s: failed calls (2 x 30 s) | 4,116 -> 0 | 1,238 -> 0 | 4,990 -> 0 |
| UDP 12,000/s: UDP receive-buffer drops | 56,100 -> 7,351 | 43,041 -> 9,856 | 76,754 -> 6,486 |
| UDP 12,000/s: CPU % per 1,000 calls/s | 48.2 -> 36.2 (-25%) | 44.1 -> 29.9 (-32%) | 47.4 -> 32.7 (-31%) |
| UDP 12,000/s: syscalls per call | 92.3 -> 29.0 (-69%) | 76.2 -> 23.4 (-69%) | 97.6 -> 27.1 (-72%) |
| TLS 6,000/s: CPU % per 1,000 calls/s | 128.9 -> 116.1 (-10%) | 119.3 -> 106.0 (-11%) | 113.6 -> 103.5 (-9%) |
| TLS 6,000/s: syscalls per call | 119.0 -> 99.3 (-17%) | 123.9 -> 98.9 (-20%) | 117.8 -> 101.4 (-14%) |
| TLS 6,000/s: `sched_yield` per call | 29.7 -> 9.5 (-68%) | 36.7 -> 11.2 (-69%) | 30.1 -> 13.7 (-54%) |
| WSS 5,000/s: CPU % per 1,000 calls/s | 160.6 -> 149.4 (-7%) | 149.2 -> 137.3 (-8%) | n/a (F_MALLOC cell failed) |
| WSS 5,000/s: syscalls per call | 152.8 -> 137.5 (-10%) | 158.5 -> 139.1 (-12%) | n/a |
| WSS 5,000/s: `sched_yield` per call | 23.0 -> 9.4 (-59%) | 30.7 -> 11.0 (-64%) | n/a |

The UDP rows compare F_MALLOC already past its limit with HG_MALLOC still holding at the same
rate. In the TLS rows (both with default `proto_tls` settings) and the WSS rows both allocators
hold, except WSS `exclusive`: its F_MALLOC cell at 5,000/s failed although it held 6,000 twice,
so there is no valid pair for it. Context switches per call are left out of the UDP rows on
purpose: overloaded F_MALLOC workers stop sleeping, which makes that count look lower.

**Why.** A per-syscall count of the OpenSIPS processes (every syscall tracepoint, separate cells)
shows where F_MALLOC's extra syscalls come from: `sched_yield`, 45 / 43 / 70 per call
(shared / auto / exclusive) out of 65 / 62 / 89 in total at 10,000 UDP calls/s - the shared-memory
lock's adaptive wait yielding under contention. With HG_MALLOC it is 6.8 / 5.1 / 5.9 per call out
of 26 / 24 / 25 at 14,000 calls/s. With F_MALLOC the syscalls per call also grow with load
(57 -> 80 -> 92 for `shared` at 5,000 -> 10,000 -> 12,000 calls/s); with HG_MALLOC they stay flat
(28-30 from 12,000 to 20,000). The lock-free fast path removes the contention, the workers drain
their sockets faster, and the same default receive buffer stops overflowing - hence the doubled UDP
ceiling. TLS and WSS add the TCP I/O threads to the contending set, so the `sched_yield` saving is
just as large there (54-69%). Their ceilings are set elsewhere, though: by TCP main and, for WSS,
by the dispatch queue between TCP main and the workers (see 4.7). So HG_MALLOC lowers the cost per
call there without raising the ceiling, apart from TLS `auto`, which held 8,000 calls/s only with
HG_MALLOC.

**Observation to look at.** The one server log read in full (a TLS cell at 8,000 calls/s,
HG_MALLOC) contained one HG warning: `hg_lock_stall_note: shm arena lock held for 1014 us by the
refill path (threshold hg_lock_stall_us=1000; 1 stalls so far, worst 1014 us by refill in
process 22)`. The other cells' logs were only summarised for errors, so how often this occurs is
not known.

The drivers are in `tools/cps/` (UDP) and `tools/tls/` (TLS/WSS); per-cell raw data (sipp
screens, `perf stat` counters, `/proc` samples) is kept with the author.
