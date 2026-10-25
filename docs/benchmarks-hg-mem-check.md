# `hg_mem_check` and the pkg shrink decision: measurements

Measured on 2026-10-25 for the HG_MALLOC pull request, [OpenSIPS#4327](https://github.com/OpenSIPS/opensips/pull/4327): the tail guard and abort level `hg_mem_check` (commit 52ac3f3306) and the fix to the pkg shrink decision (a148e3e4e6). The parameter is described in [configuration](configuration.md#global-parameters) and the checks in [design](design.md#1-allocation); the summary row is in [benchmarks](benchmarks.md#the-optional-tail-guard-hg_mem_check).

## Summary

* **With `hg_mem_check = 0` the knob costs nothing measurable.** Against the build without it: -0.6% CPU per operation in the alloc/free-only test, -1.6% to +2.9% CPU per call in the tm test, 0.0% and +0.3% in the load balancer below saturation - each inside the spread of the runs.
* **With `hg_mem_check = 1`** the guard costs 4.5 ns per allocation or free in the alloc/free-only test (+7.9%), stays within the run-to-run spread of the tm test, and costs 2.0-3.4% CPU per call in the load balancer, which allocates more per call. No call failed and no datagram was dropped below saturation, with or without it. Beyond saturation, runs of the same build differed by up to 15% in completed calls, more than the builds differed from each other.
* **The checks work:** of four one-byte overruns injected, `0` found none, `1` found all four at the exact byte, `2` logged the overrun and aborted. The guard helpers pass on all three header layouts the builds select (64-bit, and both 32-bit ones under qemu).
* **The shrink fix works:** two SIP workers whose pkg arenas grew once (2 to 18 MB) and were then emptied kept 18 MB committed for six minutes with the old decision, and went back to 2 MB two minutes in with the new one.

## Setup

* **Host:** one 16-vCPU virtual machine (Xeon E5-2699 v4), 15 GB, Debian 13, kernel 6.12, transparent huge pages `always`, no hugetlb pool. As `core:hg_stats` read back from the running processes, the shared arena ran on plain 4 KB pages and the pkg arenas on THP 2 MB pages, both locked in RAM (`ulimit -l unlimited`).
* **Builds:** gcc 14.2 with the default `-O9` (the `-DCC_O0` line of `Makefile.conf.template` commented out, checked in each binary's DWARF producer string), DBG_MALLOC on as in the template, `-Werror`, both binaries from one `Makefile.conf`:
  * *no knob*: the pull request's head before this change, 196e014f4e, plus the hgstress `fill`/`max_small` parameters the alloc/free test needs;
  * *knob*: the code of 52ac3f3306 (the same hgstress parameters included), run with `hg_mem_check = 0` or `= 1` in the config. Every run asked the live process for `core:hg_stats`, and every run reported the `guard` flag its arm should have (the build without the knob has no such field).
* **Order and pinning:** the three builds in rotating order (no knob, 0, 1 / 0, 1, no knob / 1, no knob, 0), 3 runs per point and 5 for the alloc/free test. opensips pinned to 6 vCPUs and sipp to the other 10; hgstress to 9 vCPUs. All traffic over loopback; `net.core.netdev_max_backlog` raised to 10,000 for the runs; opensips `maxbuffer` and sipp `-buff_size` 8 MB.
* **Memory:** `-a HG_MALLOC -m 4096 -M 64` for the SIP tests, `-m 1024 -M 64` for hgstress.

### The tests

1. **alloc/free only:** hgstress with 8 UDP workers, which runs 9 worker processes; each does 20 million operations, an operation being one allocation or one free, sizes 8-1031 bytes, no large tier, `fill = 0`: no stamps, so the allocator alone is measured - and for the guard the worst case, since nothing else touches the cache line the guard writes. CPU is the user plus system time of every opensips process from start to the last worker's PASS; ns per operation is that CPU over all operations.
2. **tm replying 486:** sipp UACs (INVITE, 486, ACK) at 3,000 calls/s each, 65 s per point, against this config with 2 UDP workers:

<details><summary>tm config</summary>

```
log_level=2
stderror_enabled=yes
syslog_enabled=no
udp_workers=2
maxbuffer=8388608
socket=udp:127.0.0.1:5070
mpath="/path/to/modules/"
hg_mem_check=1
loadmodule "proto_udp.so"
loadmodule "signaling.so"
loadmodule "sl.so"
loadmodule "sipmsgops.so"
loadmodule "tm.so"
loadmodule "mi_fifo.so"
modparam("mi_fifo", "fifo_name", "/tmp/opensips_fifo")
modparam("tm", "fr_timeout", 5)
modparam("tm", "fr_inv_timeout", 10)
modparam("tm", "wt_timer", 2)
route {
    if (is_method("ACK")) {
        t_check_trans();
        exit;
    }
    if (!has_totag()) {
        if (is_method("INVITE")) {
            if (!t_newtran()) exit;
            t_reply(486, "Busy Here");
            exit;
        }
    }
    if (is_method("BYE|CANCEL")) {
        sl_send_reply(200, "OK");
        exit;
    }
    sl_send_reply(404, "Not Found");
}
```

</details>

3. **load balancer:** the shape of a production load balancer - dispatcher round-robin over 6 sipp UAS backends, dialog-backed topology hiding, stateful relay with failover - with 2 UDP workers. The external leg is 127.0.0.1, the internal 127.0.0.2, the backends 127.0.0.3:5081-5086. sipp UACs at 1,500 calls/s each send INVITE, receive 100/180/200, send ACK, wait 100 ms, send BYE and receive its 200; the ACK and the BYE go to the Contact the topology hiding encoded.

<details><summary>load balancer config</summary>

```
log_level=2
stderror_enabled=yes
syslog_enabled=no
udp_workers=2
maxbuffer=8388608
auto_aliases=0
socket=udp:127.0.0.2:5060
socket=udp:127.0.0.1:5060
mpath="/path/to/modules/"
hg_mem_check=1
loadmodule "proto_udp.so"
loadmodule "tm.so"
loadmodule "sl.so"
loadmodule "signaling.so"
loadmodule "sipmsgops.so"
loadmodule "maxfwd.so"
loadmodule "db_sqlite.so"
loadmodule "dispatcher.so"
loadmodule "dialog.so"
loadmodule "topology_hiding.so"
loadmodule "mi_fifo.so"
modparam("mi_fifo", "fifo_name", "/tmp/opensips_fifo")
modparam("tm", "fr_timeout", 5)
modparam("tm", "fr_inv_timeout", 30)
modparam("dispatcher", "db_url", "sqlite:///path/to/dispatcher.db")
modparam("dispatcher", "table_name", "dispatcher")
modparam("dispatcher", "ds_ping_method", "OPTIONS")
modparam("dispatcher", "ds_ping_interval", 30)
modparam("dispatcher", "ds_probing_mode", 0)
modparam("dispatcher", "ds_probing_threshhold", 3)
modparam("dispatcher", "options_reply_codes", "200,404,405,501")
modparam("dispatcher", "ds_ping_from", "sip:127.0.0.2:5060")
modparam("dialog", "default_timeout", 60)
modparam("topology_hiding", "force_dialog", 1)
modparam("topology_hiding", "th_contact_encode_param_password", "thinfo:<password>")
route {
    if (!mf_process_maxfwd_header(10)) {
        send_reply(483, "Too Many Hops");
        exit;
    }
    if (has_totag()) {
        if (topology_hiding_match()) {
            if (is_method("ACK")) {
                t_relay();
                exit;
            }
            if (!is_method("INVITE|BYE|CANCEL|UPDATE|PRACK|INFO|REFER|NOTIFY|MESSAGE|OPTIONS")) {
                send_reply(405, "Method Not Allowed");
                exit;
            }
            route(RELAY);
            exit;
        }
        if (is_method("ACK")) {
            t_relay();
            exit;
        }
        send_reply(404, "Dialog Not Found");
        exit;
    }
    if (is_method("CANCEL")) {
        if (t_check_trans())
            t_relay();
        exit;
    }
    if (t_check_trans())
        exit;
    if (!is_method("INVITE|OPTIONS")) {
        send_reply(405, "Method Not Allowed");
        exit;
    }
    if (is_method("OPTIONS")) {
        send_reply(200, "OK");
        exit;
    }
    if ($socket_in(ip) == "127.0.0.2") {
        if (!ds_is_in_list($si, 0, 1)) {
            send_reply(403, "Forbidden");
            exit;
        }
        topology_hiding("U");
        route(RELAY);
        exit;
    }
    topology_hiding("U");
    if (!ds_select_dst(1, 4)) {
        send_reply(500, "No Downstream Available");
        exit;
    }
    route(RELAY);
}
route[RELAY] {
    t_on_failure("DS_FAILOVER");
    if (!t_relay()) {
        send_reply(500, "Internal Relay Error");
    }
    exit;
}
failure_route[DS_FAILOVER] {
    if (t_was_cancelled())
        exit;
    if ($socket_in(ip) == "127.0.0.1" && t_check_status("408|5[0-9][0-9]")) {
        if (ds_next_dst()) {
            route(RELAY);
            exit;
        }
    }
}
```

</details>

### The columns

* **completed calls/s:** from tm's statistics over the CPU window: `4xx_transactions` in the tm test, `2xx_transactions` / 2 in the load balancer (the INVITE's and the BYE's).
* **CPU, cores:** user plus system time of every opensips process over a 30 s window starting 12 s into the load.
* **µs CPU per call:** cores over completed calls per second.
* **futex per call (recv per call):** syscall tracepoints over the next 8 s (`perf stat`), per completed call; recv is recvfrom plus recvmsg, the control that the counter counts.
* **HG self-time:** the share of samples (`perf record`, 8 s at 999 Hz, the window after that) in functions defined in `mem/hg_*.c`. A short window: it varies from run to run more than the other columns.
* **failed calls / sipp retransmissions:** summed over the sipp UACs of the 3 runs.
* **UDP receive-buffer drops:** `RcvbufErrors` in `/proc/net/snmp`, summed over the 3 runs.
* **shm max_used:** `shmem:max_used_size` at the end of the run.
* Means with (min-max) over the runs; "vs" compares the means.

## The build without the knob, `hg_mem_check = 0` and `= 1`

### alloc/free only

| build | runs | ns CPU per operation | vs no knob | operations per second, millions | per run, ns |
|---|---|---|---|---|---|
| no knob | 5 | 56.9 (56.1-57.3) |  | 129.3 (125.7-132.8) | 57.3, 57.1, 56.9, 57.1, 56.1 |
| `hg_mem_check = 0` | 5 | 56.6 (55.8-58.6) | -0.6% | 134.6 (129.8-138.0) | 56.0, 56.1, 55.8, 58.6, 56.3 |
| `hg_mem_check = 1` | 5 | 61.4 (60.1-62.3) | +7.9% | 125.5 (121.9-130.9) | 60.1, 62.2, 62.3, 62.2, 60.3 |

### tm replying 486

| offered calls/s | build | runs | completed calls/s | CPU, cores | µs CPU per call | vs no knob | futex per call (recv per call) | HG self-time | failed calls / sipp retransmissions | UDP receive-buffer drops | shm max_used, MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 10,000 | no knob | 3 | 10226 (10206-10237) | 0.528 (0.521-0.537) | 51.6 (50.9-52.6) |  | 0.000 (0.000-0.000) (2.03) | 5.3 (3.6-7.7)% | 0 / 0 | 0 | 243 (242-244) |
| 10,000 | `hg_mem_check = 0` | 3 | 10236 (10227-10249) | 0.543 (0.536-0.555) | 53.1 (52.4-54.2) | +2.9% | 0.000 (0.000-0.000) (2.04) | 4.9 (4.7-5.1)% | 0 / 0 | 0 | 241 (241-242) |
| 10,000 | `hg_mem_check = 1` | 3 | 10234 (10233-10236) | 0.540 (0.528-0.552) | 52.8 (51.6-53.9) | +2.3% | 0.000 (0.000-0.000) (2.04) | 6.6 (5.3-7.6)% | 0 / 0 | 0 | 241 (241-242) |
| 20,000 | no knob | 3 | 20469 (20453-20494) | 0.940 (0.916-0.961) | 45.9 (44.8-46.9) |  | 0.000 (0.000-0.000) (1.97) | 5.2 (5.0-5.5)% | 0 / 0 | 0 | 480 (479-481) |
| 20,000 | `hg_mem_check = 0` | 3 | 20464 (20457-20473) | 0.925 (0.912-0.950) | 45.2 (44.5-46.4) | -1.6% | 0.000 (0.000-0.000) (1.97) | 5.3 (4.4-6.6)% | 0 / 0 | 0 | 479 (479-480) |
| 20,000 | `hg_mem_check = 1` | 3 | 20475 (20443-20492) | 0.940 (0.888-0.969) | 45.9 (43.5-47.3) | +0.0% | 0.000 (0.000-0.000) (1.97) | 7.7 (7.4-8.4)% | 0 / 0 | 0 | 480 (479-481) |
| 60,000 | no knob | 3 | 34394 (32771-37025) | 2.111 (2.109-2.113) | 61.6 (57.0-64.5) |  | 0.000 (0.000-0.000) (1.13) | 3.6 (3.4-3.8)% | 3,672,284 / 35,809,525 | 45,415,595 | 2145 (2028-2288) |
| 60,000 | `hg_mem_check = 0` | 3 | 34073 (30986-35750) | 2.108 (2.094-2.116) | 62.1 (59.0-68.2) | +1.0% | 0.000 (0.000-0.000) (1.12) | 3.9 (3.7-4.0)% | 3,701,695 / 35,609,931 | 45,181,925 | 2134 (1985-2228) |
| 60,000 | `hg_mem_check = 1` | 3 | 32125 (30820-32984) | 2.109 (2.099-2.120) | 65.7 (64.0-68.1) | +6.8% | 0.000 (0.000-0.000) (1.12) | 4.1 (3.9-4.5)% | 4,026,875 / 37,321,219 | 47,084,343 | 2052 (1968-2145) |

µs CPU per call, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 10,000 | no knob | 52.6 | 50.9 | 51.3 |
| 10,000 | `hg_mem_check = 0` | 52.7 | 52.4 | 54.2 |
| 10,000 | `hg_mem_check = 1` | 53.9 | 51.6 | 52.8 |
| 20,000 | no knob | 46.1 | 44.8 | 46.9 |
| 20,000 | `hg_mem_check = 0` | 44.5 | 46.4 | 44.6 |
| 20,000 | `hg_mem_check = 1` | 43.5 | 47.3 | 47.0 |
| 60,000 | no knob | 64.5 | 57.0 | 63.2 |
| 60,000 | `hg_mem_check = 0` | 59.0 | 59.2 | 68.2 |
| 60,000 | `hg_mem_check = 1` | 65.1 | 68.1 | 64.0 |

Completed calls/s, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 10,000 | no knob | 10206 | 10236 | 10237 |
| 10,000 | `hg_mem_check = 0` | 10232 | 10227 | 10249 |
| 10,000 | `hg_mem_check = 1` | 10234 | 10233 | 10236 |
| 20,000 | no knob | 20461 | 20453 | 20494 |
| 20,000 | `hg_mem_check = 0` | 20461 | 20473 | 20457 |
| 20,000 | `hg_mem_check = 1` | 20443 | 20490 | 20492 |
| 60,000 | no knob | 32771 | 37025 | 33387 |
| 60,000 | `hg_mem_check = 0` | 35484 | 35750 | 30986 |
| 60,000 | `hg_mem_check = 1` | 32571 | 30820 | 32984 |

### Load balancer

| offered calls/s | build | runs | completed calls/s | CPU, cores | µs CPU per call | vs no knob | futex per call (recv per call) | HG self-time | failed calls / sipp retransmissions | UDP receive-buffer drops | shm max_used, MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4,000 | no knob | 3 | 4091 (4089-4093) | 1.122 (1.117-1.133) | 274.3 (272.9-276.8) |  | 0.000 (0.000-0.000) (5.02) | 11.7 (10.4-12.6)% | 0 / 0 | 0 | 725 (725-726) |
| 4,000 | `hg_mem_check = 0` | 3 | 4095 (4091-4100) | 1.123 (1.110-1.138) | 274.2 (270.6-277.8) | -0.0% | 0.000 (0.000-0.000) (5.01) | 10.4 (10.0-10.7)% | 0 / 0 | 0 | 725 (724-726) |
| 4,000 | `hg_mem_check = 1` | 3 | 4093 (4092-4093) | 1.161 (1.130-1.190) | 283.6 (276.0-290.7) | +3.4% | 0.000 (0.000-0.000) (5.03) | 11.5 (11.1-11.9)% | 0 / 0 | 0 | 728 (728-728) |
| 8,000 | no knob | 3 | 8185 (8181-8189) | 2.227 (2.206-2.248) | 272.2 (269.4-274.8) |  | 0.000 (0.000-0.000) (5.02) | 11.3 (11.1-11.4)% | 0 / 0 | 0 | 1447 (1447-1448) |
| 8,000 | `hg_mem_check = 0` | 3 | 8188 (8182-8191) | 2.235 (2.132-2.319) | 273.0 (260.6-283.1) | +0.3% | 0.000 (0.000-0.000) (5.00) | 11.9 (11.7-12.0)% | 0 / 0 | 0 | 1447 (1443-1449) |
| 8,000 | `hg_mem_check = 1` | 3 | 8183 (8180-8187) | 2.271 (2.198-2.387) | 277.6 (268.7-291.8) | +2.0% | 0.000 (0.000-0.000) (5.02) | 12.3 (12.0-12.5)% | 0 / 0 | 0 | 1451 (1448-1454) |
| 15,000 | no knob | 3 | 12412 (11587-12924) | 3.306 (3.271-3.336) | 266.8 (258.1-282.3) |  | 0.000 (0.000-0.000) (4.59) | 12.0 (11.7-12.1)% | 585,446 / 10,357,009 | 12,274,661 | 2667 (2532-2761) |
| 15,000 | `hg_mem_check = 0` | 3 | 11761 (11256-12527) | 3.287 (3.243-3.311) | 280.0 (264.4-288.1) | +4.9% | 0.000 (0.000-0.000) (4.56) | 12.2 (11.8-12.8)% | 647,687 / 10,886,869 | 12,854,280 | 2695 (2596-2831) |
| 15,000 | `hg_mem_check = 1` | 3 | 11953 (11230-12605) | 3.318 (3.312-3.326) | 278.2 (263.9-294.9) | +4.3% | 0.000 (0.000-0.000) (4.52) | 13.0 (12.7-13.5)% | 618,989 / 10,764,210 | 12,705,832 | 2624 (2499-2721) |

µs CPU per call, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 4,000 | no knob | 276.8 | 273.1 | 272.9 |
| 4,000 | `hg_mem_check = 0` | 274.1 | 270.6 | 277.8 |
| 4,000 | `hg_mem_check = 1` | 290.7 | 284.1 | 276.0 |
| 8,000 | no knob | 272.3 | 274.8 | 269.4 |
| 8,000 | `hg_mem_check = 0` | 283.1 | 260.6 | 275.2 |
| 8,000 | `hg_mem_check = 1` | 291.8 | 268.7 | 272.2 |
| 15,000 | no knob | 282.3 | 260.1 | 258.1 |
| 15,000 | `hg_mem_check = 0` | 288.1 | 264.4 | 287.6 |
| 15,000 | `hg_mem_check = 1` | 263.9 | 275.9 | 294.9 |

Completed calls/s, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 4,000 | no knob | 4092 | 4089 | 4093 |
| 4,000 | `hg_mem_check = 0` | 4091 | 4100 | 4095 |
| 4,000 | `hg_mem_check = 1` | 4093 | 4093 | 4092 |
| 8,000 | no knob | 8184 | 8181 | 8189 |
| 8,000 | `hg_mem_check = 0` | 8191 | 8182 | 8190 |
| 8,000 | `hg_mem_check = 1` | 8181 | 8180 | 8187 |
| 15,000 | no knob | 11587 | 12724 | 12924 |
| 15,000 | `hg_mem_check = 0` | 11256 | 12527 | 11501 |
| 15,000 | `hg_mem_check = 1` | 12605 | 12025 | 11230 |

## Two earlier runs: `hg_mem_check = 0` against `= 1`

The knob build only, the two levels alternating, 3 runs per point (5 for the first table), without pinning: opensips and sipp shared all 16 vCPUs. The tm test as above, 2 UDP workers.

**`-m 1024 -M 64`:** alloc/free only (as above), the stamped hgstress soak (sizes 8-8007 bytes and 1 in 64 above 64 KB, every block stamped at allocation and checked before its free, 9 workers x 5 million operations), and the tm test from 4,000 to 40,000 calls/s.

| build | runs | ns CPU per operation | vs `hg_mem_check = 0` | operations per second, millions | per run, ns |
|---|---|---|---|---|---|
| `hg_mem_check = 0` | 5 | 56.9 (55.7-58.6) |  | 136.4 (129.3-139.5) | 58.6, 58.3, 55.7, 55.8, 55.9 |
| `hg_mem_check = 1` | 5 | 61.6 (60.7-62.9) | +8.3% | 126.9 (120.8-132.9) | 61.6, 61.5, 60.7, 62.9, 61.3 |

Stamped soak:

| build | runs | ns CPU per operation | vs `hg_mem_check = 0` | operations per second, millions | per run, ns |
|---|---|---|---|---|---|
| `hg_mem_check = 0` | 3 | 938.8 (925.3-956.4) |  | 9.0 (8.7-9.2) | 956.4, 934.7, 925.3 |
| `hg_mem_check = 1` | 3 | 943.1 (920.4-970.7) | +0.5% | 9.0 (8.8-9.1) | 938.2, 970.7, 920.4 |

| offered calls/s | build | runs | completed calls/s | CPU, cores | µs CPU per call | vs `hg_mem_check = 0` | futex per call (recv per call) | HG self-time | failed calls / sipp retransmissions | UDP receive-buffer drops | shm max_used, MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4,000 | `hg_mem_check = 0` | 3 | 4093 (4091-4096) | 0.266 (0.261-0.270) | 65.1 (63.9-66.0) |  | 0.000 (0.000-0.000) (2.17) | 7.4 (5.6-10.4)% | 0 / 0 | 0 | 99 (99-99) |
| 4,000 | `hg_mem_check = 1` | 3 | 4093 (4090-4096) | 0.258 (0.246-0.267) | 62.9 (60.0-65.2) | -3.4% | 0.000 (0.000-0.000) (2.19) | 5.7 (3.4-7.8)% | 0 / 0 | 0 | 99 (98-100) |
| 10,000 | `hg_mem_check = 0` | 3 | 10241 (10236-10246) | 0.535 (0.530-0.545) | 52.3 (51.8-53.2) |  | 0.000 (0.000-0.000) (2.03) | 4.7 (3.7-5.5)% | 0 / 0 | 0 | 242 (241-242) |
| 10,000 | `hg_mem_check = 1` | 3 | 10233 (10218-10247) | 0.536 (0.522-0.546) | 52.4 (51.1-53.3) | +0.2% | 0.000 (0.000-0.000) (2.05) | 7.1 (4.6-8.6)% | 0 / 0 | 0 | 242 (241-243) |
| 20,000 | `hg_mem_check = 0` | 3 | 20467 (20457-20486) | 0.945 (0.907-0.974) | 46.2 (44.2-47.6) |  | 0.000 (0.000-0.000) (1.97) | 6.0 (5.6-6.9)% | 0 / 0 | 0 | 479 (478-480) |
| 20,000 | `hg_mem_check = 1` | 3 | 20469 (20459-20475) | 0.991 (0.970-1.012) | 48.4 (47.4-49.4) | +4.8% | 0.000 (0.000-0.000) (1.96) | 6.4 (6.2-6.6)% | 0 / 0 | 0 | 480 (478-482) |
| 40,000 | `hg_mem_check = 0` | 3 | 32473 (20980-40974) | 1.967 (1.877-2.045) | 66.4 (45.8-97.5) |  | 0.000 (0.000-0.000) (2.55) | 5.0 (4.4-6.0)% | 1,008,756 / 10,403,667 | 9,724,608 | 1000 (959-1020) |
| 40,000 | `hg_mem_check = 1` | 3 | 36859 (28671-41032) | 1.932 (1.848-2.044) | 54.3 (45.2-71.3) | -18.2% | 0.000 (0.000-0.000) (2.32) | 6.2 (5.4-6.6)% | 457,747 / 4,801,687 | 4,470,346 | 983 (963-1020) |

Completed calls/s, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 4,000 | `hg_mem_check = 0` | 4093 | 4096 | 4091 |
| 4,000 | `hg_mem_check = 1` | 4092 | 4090 | 4096 |
| 10,000 | `hg_mem_check = 0` | 10242 | 10236 | 10246 |
| 10,000 | `hg_mem_check = 1` | 10218 | 10233 | 10247 |
| 20,000 | `hg_mem_check = 0` | 20457 | 20458 | 20486 |
| 20,000 | `hg_mem_check = 1` | 20459 | 20473 | 20475 |
| 40,000 | `hg_mem_check = 0` | 35466 | 40974 | 20980 |
| 40,000 | `hg_mem_check = 1` | 28671 | 40874 | 41032 |

At 40,000 calls/s the 2 workers sit at their ceiling (about 41,000) and the point is bistable: a run either keeps up, or a burst of drops turns into sipp retransmissions that add load until the 1 GB shared arena fills. The run with `0` collapsed twice, the one with `1` once; the means compare which state the runs landed in, not the builds.

**`-m 4096 -M 64`:** the tm test at 20,000 calls/s again and at 60,000, clearly past the ceiling, with no allocation failing.

| offered calls/s | build | runs | completed calls/s | CPU, cores | µs CPU per call | vs `hg_mem_check = 0` | futex per call (recv per call) | HG self-time | failed calls / sipp retransmissions | UDP receive-buffer drops | shm max_used, MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 20,000 | `hg_mem_check = 0` | 3 | 20475 (20468-20489) | 0.976 (0.922-1.039) | 47.7 (45.0-50.7) |  | 0.000 (0.000-0.000) (1.97) | 5.3 (4.8-5.7)% | 0 / 0 | 0 | 481 (479-484) |
| 20,000 | `hg_mem_check = 1` | 3 | 20475 (20462-20496) | 0.972 (0.924-0.996) | 47.4 (45.1-48.7) | -0.5% | 0.000 (0.000-0.000) (1.97) | 6.7 (6.0-7.2)% | 0 / 0 | 0 | 480 (480-480) |
| 60,000 | `hg_mem_check = 0` | 3 | 34496 (32375-36045) | 2.157 (2.143-2.167) | 62.7 (60.0-66.9) |  | 0.000 (0.000-0.000) (1.09) | 3.6 (3.5-3.6)% | 3,799,644 / 36,413,422 | 46,031,370 | 2206 (2076-2367) |
| 60,000 | `hg_mem_check = 1` | 3 | 33332 (32765-34247) | 2.155 (2.151-2.160) | 64.7 (62.9-65.9) | +3.2% | 0.000 (0.000-0.000) (1.11) | 3.9 (3.7-4.0)% | 3,961,918 / 37,070,744 | 46,797,545 | 2143 (2105-2178) |

Completed calls/s, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 20,000 | `hg_mem_check = 0` | 20469 | 20468 | 20489 |
| 20,000 | `hg_mem_check = 1` | 20468 | 20462 | 20496 |
| 60,000 | `hg_mem_check = 0` | 32375 | 36045 | 35067 |
| 60,000 | `hg_mem_check = 1` | 34247 | 32985 | 32765 |

µs CPU per call, each run:

| offered calls/s | build | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 20,000 | `hg_mem_check = 0` | 47.3 | 45.0 | 50.7 |
| 20,000 | `hg_mem_check = 1` | 48.6 | 48.7 | 45.1 |
| 60,000 | `hg_mem_check = 0` | 66.9 | 60.0 | 61.1 |
| 60,000 | `hg_mem_check = 1` | 62.9 | 65.2 | 65.9 |

## One-byte overruns injected

hgstress's `hgs_fault` command, run in the MI process, writes one byte past a request and frees it: a 100-byte shm cell, a 100,000-byte large fragment, a 100-byte pkg cell, and a 100-byte shm cell first shrunk in place to 60 by realloc. Read back from `core:hg_stats` (`corruption.overflow`) and the log:

| fault | `hg_mem_check = 0` | `= 1` | `= 2` |
|---|---|---|---|
| shm cell, byte 100 of 100 | not found | found: "written past the 100 bytes asked for (byte 100)" | found, then "aborting on the overflow above (hg_mem_check = 2)" and the instance stopped |
| large fragment, byte 100000 of 100000 | not found | found: "(byte 100000)" | - |
| pkg cell, byte 100 of 100 | not found | found: "(byte 100)" | - |
| shm cell after realloc to 60, byte 60 | not found | found: "written past the 60 bytes asked for (byte 60)" | - |
| counters | overflow 0 shm, 0 pkg | overflow 3 shm, 1 pkg | - |

## The guard helpers on every header layout

A check program built against `mem/hg_cell.h`: for every request size a cell of 64, 96, 128 and 192 bytes, a 64 KB cell and a large fragment of 100,008 payload bytes can guard (for the two big ones only the sizes near each end), it sets the guard and checks that an intact guard gives the size back, that a one-byte overrun at the end of the request is caught, that a scribbled size field is caught, that nothing outside the cell is written, and that the cell is marked as guarded. Built for x86-64 and, statically, for 32-bit ARM run under qemu, for each combination of DBG_MALLOC and SHM_EXTRA_STATS:

| build | DBG_MALLOC | SHM_EXTRA_STATS | where the size is kept | header, bytes | sizes checked | failures |
|---|---|---|---|---|---|---|
| x86-64 | yes | no | spare word in the header | 32 | 612 | 0 |
| x86-64 | no | no | spare word in the header | 8 | 708 | 0 |
| x86-64 | yes | yes | spare word in the header | 40 | 580 | 0 |
| x86-64 | no | yes | spare word in the header | 16 | 676 | 0 |
| 32-bit ARM | yes | no | the cell's last 4 bytes | 16 | 660 | 0 |
| 32-bit ARM | no | no | header padding | 8 | 708 | 0 |
| 32-bit ARM | yes | yes | header padding | 24 | 644 | 0 |
| 32-bit ARM | no | yes | the cell's last 4 bytes | 8 | 692 | 0 |

The header sizes are the ones each configuration had before the change.

## The pkg shrink decision

`-m 1024 -M 2:32`, the tm config with 2 UDP workers and hgstress loaded: each SIP worker takes 12 MB of pkg at start (`hold_pkg_mb`) and frees it, so its arena grows once, from 2 MB to 18 MB with the default 16 MB step, and ends nearly empty. sipp then sends 200 calls/s for six minutes, which keeps the workers' sweeps running (a worker sweeps between jobs). Every 30 s each worker's `pkmem:N-total_size` (committed) and `real_used_size` (in use), and the `Anonymous` memory in its `smaps_rollup`; the two workers are shown as "first / second". *Old decision* = 52ac3f3306, *new* = the branch head with a148e3e4e6, same build settings.

| time after start | old decision: pkg committed, KB (in use) | anonymous memory, KB | new decision: pkg committed, KB (in use) | anonymous memory, KB |
|---|---|---|---|---|
| 0 s | not yet published¹ / 18432 (138) | 21116 / 21116 | 18432 (138) / 18432 (138) | 21112 / 21112 |
| 30 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 18432 (138) / 18432 (138) | 21112 / 21112 |
| 60 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 18432 (138) / 18432 (138) | 21112 / 21112 |
| 90 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 18432 (138) / 18432 (138) | 21112 / 21112 |
| 120 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 150 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 180 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 210 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 240 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 270 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 300 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 330 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |
| 360 s | 18432 (138) / 18432 (138) | 21116 / 21116 | 2048 (138) / 2048 (138) | 4728 / 4728 |

¹ The first worker's statistic was read before that process had published it.

## Build checks

On the pull request's branch and on the 4.0 port, with `-O9 -Werror` and gcc 14.2: the unit-test build, the default build (hgstress included) and a build with HG_MALLOC compiled out, 0 warnings each. On the pull request's tree also the HG objects with gcc and clang, for x86-64 and 32-bit ARM, in all four combinations of DBG_MALLOC and SHM_EXTRA_STATS: 32 compiles, 0 diagnostics, none new against the head before the change.

## Limits of these numbers

* One host: sipp shares the machine with opensips (pinned to separate CPUs in the main run, not in the two earlier runs), and the machine is a virtual one.
* HG self-time comes from 8-second sampling windows and varies more between runs than the CPU figures.
* Past saturation the completed-call rate depends on how retransmissions build up in each run (up to 15% between runs of the same build here); it is no measure of small differences.
