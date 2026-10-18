# TLS / WSS call-rate rig (section 4.7)

* `srv_tls.cfg.tmpl` - the server: the stateful proxy of `../cps/srv_cps.cfg.tmpl` with
  `tls:5061` and `wss:7443` listeners and the staging `proto_tls` / `proto_wss` settings.
* `gw.cfg.tmpl`, `gwctl.sh` - the client-side gateway, one per client host: 8 UDP sockets
  (5080-5087) for the local sipp UACs, each relayed over its own TLS (5100-5107) or WSS
  (7100-7107) connection to the server.
* `run_tls.sh <TAG> <tls|wss> <F_MALLOC|HG_MALLOC> <modes...>` - descending search from
  10,000 calls/s per `tcp_dispatch_mode`, then every mode at the lowest ceiling.
  `run_cells.sh <TAG> <tls|wss> <alloc> <mode:rate ...>` - named single cells, with the exact
  count of dispatch drops from the server log.
* `sample_tls.sh` (on the server: `perf stat` of all OpenSIPS tasks plus `../probe4.py`),
  `cliprobe2.py` (on the clients: busiest sipp and gateway process), `cellsum_tls.py` (one line
  and `sum.json` per cell), `tls_table.py` (the tables).

Before a run: put `../srvctl.sh`, `../probe4.py` and `srv_tls.cfg.tmpl` in `/tmp/udpbal/` on
the server (`sample_tls.sh` is sent over ssh), have `sipp` in the PATH on the client hosts, install the build under test where `SRVPREFIX` points, install an
OpenSIPS for the gateways where `GWPREFIX` points on every client host, and put a self-signed
`cert.pem` / `key.pem` in `certs/` here, for example:

    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj /CN=bench \
        -keyout certs/key.pem -out certs/cert.pem

The gateways need `tls_async_max_postponed_chunks` 1024, and both ends need
`wss_max_msg_chunks` above 16 for WSS to connect at all (see 4.7).
