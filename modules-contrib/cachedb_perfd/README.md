---
title: "cachedb_perfd Module"
description: "CacheDB backend for perfcached - a multithreaded, self-clustering cache daemon reached over a Noise-authenticated binary protocol."
---

## Admin Guide


### Overview


This module is OpenSIPS's gateway to [perfcached](https://github.com/Lt-Flash/perfcached),
a standalone cache daemon.  It serves the core Key-Value interface
(`cache_store()`, `cache_fetch()`, `cache_add()` and friends) from a
single perfcached node or a perfcached cluster, the same way
`cachedb_redis` or `cachedb_memcached` serve theirs.  The module holds
no data of its own.

It talks to the daemon through **libperfd**, perfcached's client
library: a binary protocol inside a **Noise** channel (libsodium; no
TLS stack and no certificates - a shared client secret is the whole
handshake).  libperfd spreads the load over the whole cluster: given one seed node it
learns the members, keeps a connection open to each of them, sends each
key straight to the node that owns it and, on every node, places its
connections evenly over the daemon's worker threads.  When a node dies
it fails over to an already-open standby connection.  See the
[libperfd documentation](https://github.com/Lt-Flash/perfcached/blob/master/lib/README.md).


### About perfcached


perfcached is a general-purpose cache daemon (GPL-2.0-or-later; the
client library is MIT).  The parts that matter to an OpenSIPS
deployment:

- *multithreaded: one node uses all its cores*
- *self-clustering: a single node is a complete deployment; run the
  same binary on several and they find each other (multicast on one
  network segment, or seed names where there is no multicast), elect a
  master and share the keyspace*
- *one mode per cluster, from every node holding every key (`eager`)
  to one owner per key (`shard`), with `spread`, `proxy` and `store`
  in between*
- *optional write-ahead log plus snapshots, so the data survives a
  daemon restart*
- *a Redis protocol (RESP2) listener for compatibility, so `redis-cli`
  and Redis tooling can read the keys OpenSIPS wrote*
- *its own CLI (`perfcli`), a dashboard, and parallel backup and
  restore tools*

Documentation: [overview](https://github.com/Lt-Flash/perfcached/blob/master/README.md),
[configuration guide](https://github.com/Lt-Flash/perfcached/blob/master/GUIDE.md),
[production notes](https://github.com/Lt-Flash/perfcached/blob/master/PRODUCTION.md).
Releases and packages (Debian, Ubuntu, Alpine):
<https://github.com/Lt-Flash/perfcached/releases>.


### Advantages


- *one cache shared by any number of OpenSIPS instances - a key written
  by one is readable by all*
- *the cache survives OpenSIPS restarts*
- *counters (`cache_add()` / `cache_sub()`) are atomic at the owning
  node, so several proxies count into one value (an operation whose reply
  is lost with its connection may still have been applied)*
- *one round trip for many keys (`MGET`) and an asynchronous fetch
  (`perfd_fetch()`) that does not block the SIP worker*
- *JSON documents (`JSON.SET` / `JSON.GET` / `JSON.DEL`) through
  `cache_raw_query()`*


### Limitations


- *`cache_raw_query()` accepts a fixed set of verbs (see below); other
  daemon commands are reached with `perfcli` or a Redis client*
- *only `perfd_fetch()` is asynchronous; the other operations block the
  worker for one round trip*


### Fork Safety and Lazy Connections


Connections are **lazy and per-process**: nothing dials at startup.
The first cache operation in a worker opens that worker's own TCP+Noise
connection; a handle inherited across `fork()` is detected by pid and
discarded without touching the parent's connection.

The Noise key of every secret in this module's `cachedb_url` values is
derived once, in the parent (Argon2id - about 150 ms of CPU and a 64 MiB
mapping), and every worker inherits it.  A `perfd://` URL given to
another module (usrloc, dialog, b2b_logic...) shares that key when it
uses the same secret; with a secret of its own, each worker derives it
at its first dial.

Consequences:

- a daemon that is down when OpenSIPS starts does not fail the startup;
  the first operation reports the error;
- idle workers hold no sockets (what `cachedb_redis` calls
  `lazy_connect=1` is always on here);
- the first operation in a worker is slower than the rest (TCP and
  Noise handshakes, plus member discovery when routing).  A throwaway
  `cache_fetch()` in a startup route warms it if that matters;
- when no seed answers, a dial costs up to `connect_timeout` per seed.
  The worker then waits 1 second before dialling again, doubling to 30
  seconds while the outage lasts; operations in between fail at once.


### Dependencies


#### OpenSIPS Modules


None.


#### External Libraries or Applications


- **libperfd** 0.7.6 or later, the perfcached client library (MIT).
  Use the libperfd from your perfcached release: libperfd 0.7.6 sends
  each key to its owner only with perfcached 0.7.5 or later, and an
  older libperfd cannot do so with 0.7.5 (requests then take the
  daemon's forward hop, which cannot carry values over 58,000 bytes: a
  write of one fails and a read of one comes back as a miss).
  It ships in perfcached release packages from v0.7.5:
  `apt-get install libperfd-dev` (Debian/Ubuntu), `apk add
  libperfd-dev` (Alpine), or `make install-lib` in a perfcached source
  tree.  The module links the shared `libperfd.so.0`, found with
  pkg-config; build with `PERFD_STATIC=1` to link `libperfd.a` instead,
  and set `PKG_CONFIG_PATH` for a libperfd in another prefix.
- **libsodium**, which libperfd depends on: `apt-get install
  libsodium-dev` (Debian/Ubuntu), `apk add libsodium-dev` (Alpine) or
  `dnf install libsodium-devel` (RHEL family).

The module is in `modules-contrib/` and is not built by default:

    make modules=modules-contrib/cachedb_perfd modules-contrib
    make modules=modules-contrib/cachedb_perfd install-modules-all

The Debian and RPM packages build it as
`opensips-module-contrib-cachedb-perfd` only with the `perfd` build
profile (`DEB_BUILD_PROFILES=perfd`) or `--with perfd`.


### Exported Parameters


#### cachedb_url (string)


The URL of a perfcached node or cluster.  May be set more than once;
the group part of the URL is the name used from the script.

```
perfd[:group]://[:secret@]host:port[,host:port...]/collection[?opt,opt,...]
```

- the **password field is the Noise client secret** (the daemon's
  `[secrets] client` value); a URL with only a user field
  (`perfd://secret@host:port/col`) uses that as the secret.  An empty secret means plaintext, which
  perfcached accepts only on localhost (`plaintext = loopback`).
- the **port is required**: perfcached has no registered default port.
- **several seeds**, comma separated, each with its port (up to 8): the
  members are learned from the first seed that answers, so a node that
  is down when a worker first dials costs one refused connect.  Each
  worker starts at a different seed (rotated by pid).  A failing seed is
  logged once per outage; only "no seed answered" is an error.  IPv6
  literals keep their brackets: `[fd00::5]:6479`.
- the **path is the collection** (the daemon's `[collection X]`), also
  required.
- **options**, comma separated after `?`; an unknown option or a bad
  value refuses the URL:
  - `route=0|1` (default `1`) - `1` sends each key to its owning node
    (one hop); `0` sends everything to the connected node and lets the
    daemon forward (no value over 58,000 bytes through a forward);
  - `spares=N` (default `-1`: a standby connection to every other
    member, up to libperfd's limit of 8) - standby connections kept open
    so a node failure costs a swap, not a new handshake.  `0` disables
    them;
  - `encrypt=0|1` (default `1`) - `1` keeps every request inside the
    Noise channel; `0` lets a node that allows it take requests in the
    clear after the handshake (the handshake still authenticates);
  - `policy=failover|rr|least|weighted` (default `failover`) - how the
    active connection is picked among the members: `failover` stays on
    the node it connected to, `rr` round-robins from a random start per
    client, `least` picks the member reporting the most free memory,
    `weighted` picks at random weighted by free memory.  With `route=1`
    keyed operations go to the key's owner whatever the policy, so it
    matters most with `route=0` against an `eager` cluster.

Connections show on the daemon as `opensips` (perfcached's `clients`
command).

```opensips title="Set cachedb_url parameter"
...
# one node on loopback, no secret
modparam("cachedb_perfd", "cachedb_url",
    "perfd://127.0.0.1:6479/subs")

# a cluster: secret, three seeds, routed (the defaults)
modparam("cachedb_perfd", "cachedb_url",
    "perfd://:s3cr3t@10.0.0.5:6479,10.0.0.6:6479,10.0.0.7:6479/subs")

# two groups on two clusters - the group name selects from the script
modparam("cachedb_perfd", "cachedb_url",
    "perfd:sessions://:s3cr3t@10.0.0.5:6479/sess")
modparam("cachedb_perfd", "cachedb_url",
    "perfd:counters://:s3cr3t@10.0.1.9:6479/ctr")

# an eager cluster behind round-robin, no per-key routing
modparam("cachedb_perfd", "cachedb_url",
    "perfd://:s3cr3t@10.0.0.5:6479/subs?route=0,policy=rr")
...
```

Keys are 1 to 511 bytes and cannot hold a NUL byte.  `cache_store()`
with an expiry of `0` (or less) stores the value without one, as
`cachedb_redis` does; `cache_add()` and `cache_sub()` with `0` keep the
counter's current expiry, as Redis `INCRBY` does.

```opensips title="Use perfcached from the script"
...
# store/fetch with a 5-minute TTL
cache_store("perfd:sessions", "sess_$ci", "$fu", 300);
cache_fetch("perfd:sessions", "sess_$ci", $avp(sess));

cache_remove("perfd:sessions", "sess_$ci");

# a shared rate counter
cache_add("perfd:counters", "calls_$fU", 1, 60, $avp(n));
if ($avp(n) > 100) {
    send_reply(503, "Rate limited");
    exit;
}

# read a counter without changing it
cache_counter_fetch("perfd:counters", "calls_$fU", $avp(now));
...
```


#### connect_timeout (integer)


Milliseconds to wait for the TCP connection and the Noise handshake to
a node.

*Default value is "5000" (0 keeps the default).*

```opensips title="Set connect_timeout parameter"
...
modparam("cachedb_perfd", "connect_timeout", 1000)
...
```


#### query_timeout (integer)


Milliseconds to wait for each send or receive during an operation.  A
dead node surfaces as an error after this long, and libperfd fails over
to a standby when it has one.

*Default value is "5000" (0 keeps the default).*

```opensips title="Set query_timeout parameter"
...
modparam("cachedb_perfd", "query_timeout", 500)
...
```


#### fleet_refresh (integer)


How often, in milliseconds, a connection that knows the cluster's
members (it routes or holds standbys) refreshes its member list.  Changes are also learned on failures; this is the
upper bound.

*Default value is "30000" (0 keeps the default).*

```opensips title="Set fleet_refresh parameter"
...
modparam("cachedb_perfd", "fleet_refresh", 5000)
...
```


#### keepalive (integer)


TCP keepalive idle time, in seconds, on every connection.  `-1` turns
keepalive off.

*Default value is "30" (0 keeps the default).*

```opensips title="Set keepalive parameter"
...
modparam("cachedb_perfd", "keepalive", 10)
...
```


#### idle_ping (integer)


Milliseconds after which an idle standby connection is pinged, so a
dead standby is found before it is needed.  `-1` turns it off.

*Default value is "30000" (0 keeps the default).*

```opensips title="Set idle_ping parameter"
...
modparam("cachedb_perfd", "idle_ping", 10000)
...
```


#### async_conns (integer)


Size of the per-process connection pool used by `perfd_fetch()`, from
1 to 1024.

*Default value is "8".*

```opensips title="Set async_conns parameter"
...
modparam("cachedb_perfd", "async_conns", 16)
...
```


### Exported Functions


#### perfd_fetch(group, key, var)


Asynchronous `cache_fetch()`: the worker goes back to the reactor while
the reply is in flight.  Use it from an `async()` statement.

- `group` - the group of the `cachedb_url` to use (the `grp` in
  `perfd:grp://...`; an unnamed URL is `""`);
- `key` - the key to fetch;
- `var` - the variable that receives the value.

Return codes match `cache_fetch()`: `1` on a hit, `-2` on a miss (the
variable is left untouched), `-1` on error.  A reply that does not come
within `query_timeout` is an error (`-1`).

Each request in flight needs a connection of its own (OpenSIPS watches
one descriptor per request), so a process keeps a pool of
`async_conns` connections, opened as needed.  When the cluster routes
by key, a fetch goes on a connection to the key's owner, as
`cache_fetch()` does.  With every connection busy, or none reachable,
the call falls back to a blocking fetch, with the same return codes, and
logs a warning at most once a minute; raise `async_conns` if it repeats.
A reply that has already arrived is delivered without suspending.  Where
OpenSIPS cannot suspend (an ACK, a failure or branch route), the fetch
waits for its reply, at most `query_timeout`.

```opensips title="perfd_fetch usage"
...
route {
    async(perfd_fetch("grp", "$fU", $avp(profile)), resume_lookup);
}

route[resume_lookup] {
    if ($avp(profile) == NULL)
        xlog("no profile for $fU\n");
}
...
```


### Raw Queries


`cache_raw_query()` accepts these verbs:

- `MGET k1 k2 ...` - many keys in one round trip (up to 256).  One
  result row per key, in the order asked; a miss is a NULL row, never a
  skipped one;
- `EXISTS k`; `EXPIRE k seconds` - as in Redis, `0` or a negative
  number deletes the key;
- `JSON.SET k path json [NX|XX] [EX seconds]`, `JSON.GET k [path]`,
  `JSON.DEL k [path]` - path syntax is the daemon's (`$`, `.name`,
  `[index]`).  `NX` sets only where the path holds nothing yet, `XX`
  only where it does; the daemon decides atomically, and a declined set
  returns a NULL row.  With `NX` or `XX` the value must be one JSON
  value.  A path below the root needs the key to exist.

In an OpenSIPS script string a literal `$` is written `$$`.  Arguments
are split on spaces; single or double quotes keep an argument with
spaces together, as in `cachedb_redis`.  A query is at most 4095 bytes.
Other verbs are refused.

```opensips title="cache_raw_query usage"
...
cache_raw_query("perfd:grp", "MGET sess_a sess_b sess_c", "$avp(vals)");
# a document per user, then a field in it that is set only once
cache_raw_query("perfd:grp", "JSON.SET acct_$fU $$ {} NX EX 3600");
cache_raw_query("perfd:grp", "JSON.SET acct_$fU $$.calls 0 NX");
...
```


### Inspecting the Cache from outside OpenSIPS


The collections this module writes can be read through the daemon's
other interfaces:

```bash title="Read OpenSIPS-written keys"
# the daemon's RESP listener (stock Redis clients)
redis-cli -h 10.0.0.5 -p 6379 GET sess_abc123

# perfcached's own CLI, over the Noise channel
perfcli -h 10.0.0.5 -p 6479 -a s3cr3t get subs sess_abc123
```

<!-- CONTRIBUTORS -->

### License

All documentation files (i.e. .md extension) are licensed under the Creative Common License 4.0
