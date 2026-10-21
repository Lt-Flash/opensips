---
title: "CLUSTERER_CONTROLLER Module"
description: "Zero-config high availability for the clusterer module: nodes discover each other over encrypted UDP multicast, elect a master, receive node IDs and BIN addresses, and fail sharing tags over automatically."
---

# clusterer_controller

Self-forming high availability for the *clusterer* module. Nodes that share an IPv4 multicast group, a cluster id and a password find each other, elect a master, receive their clusterer node ids and BIN addresses, keep one active holder per sharing tag, and fail over on their own - with the same configuration on every node.

**Needs** Linux, IPv4 multicast between the nodes, and libsodium. Without multicast, configure *clusterer* with its own *my_node_info* / *neighbor_node_info* or its database instead - see [When to use it](doc/when-to-use.md).

> This is the *clusterer_controller* branch of my OpenSIPS fork, on top of OpenSIPS master (4.1.0-dev); the module is proposed upstream as [OpenSIPS#4074](https://github.com/OpenSIPS/opensips/pull/4074). The design, protocol diagrams and measurements are on their own pages - see [Documentation](#documentation) below. The pages that come from the admin guide are generated from the module README on the PR branch by [doc/gen_pages.py](doc/gen_pages.py); the quick start, when-to-use, protocol, failure-detection and measurement pages are written here.

## Quick start

### 1. Build

```bash
apt install libsodium-dev        # on the build host; the nodes need the runtime (libsodium23)
```

In *Makefile.conf* (copied from *Makefile.conf.template*), add the module to the build, then build and install as usual:

```make
include_modules= clusterer_controller
```

```bash
make all && make install
```

Building this module also builds *clusterer* with the controller support; without it in the build, *clusterer* is the stock module.

### 2. Configure - the same file on every node

```opensips title="opensips.cfg"
socket=bin:10.0.0.11:3857               # this node's address - no wildcard

loadmodule "proto_bin.so"

loadmodule "clusterer.so"
modparam("clusterer", "cluster_options", "cluster_id=1, use_controller=1")
modparam("clusterer", "sharing_tag", "vip1/1=active")

loadmodule "clusterer_controller.so"
modparam("clusterer_controller", "cluster", "id=1,multicast=239.0.90.1:3333")
modparam("clusterer_controller", "password", "a-long-random-secret")
```

Only the *socket=bin:* line differs between nodes. Point the modules that replicate - *dialog*, *usrloc*, *dispatcher*, *tm* anycast - at cluster 1 as usual ([full example](doc/deployment.md#configuration-example)). Sharing tags start in backup on every node whatever the config says; the master takes the active one.

### 3. Start the nodes

In any order. Within seconds they agree on a master - the highest IP, and a live master keeps the role when a higher IP joins later:

```text
clusterer_controller: [cluster 1] roles: MASTER=10.0.0.13 [me] (highest IP in window); BACKUP=10.0.0.12 (highest-IP non-master); 3 node(s) in window
clusterer_controller: [cluster 1] my role changed: member -> master
clusterer_controller: [cluster 1] activating sharing tags - this node is the cluster master
```

If the master dies, the others replace it in about 5 s on a LAN; a master that is stopped hands over in under a second. A restarted node comes back with its node_id.

### 4. Check it

```bash
opensips-cli -x mi clusterer_controller:list_members   # on the master; a member shows capabilities only for itself and the master
```

```json
[
  {
    "cluster_id": 1,
    "members": [
      { "mtu": 1500, "ip": "10.0.0.13", "node_id": 1, "status": "master", "maintenance": "none",
        "capabilities": [ "probe", "maint_set", "maint_full" ], "bin_sockets": [ "bin:10.0.0.13:3857" ] },
      { "mtu": 1500, "ip": "10.0.0.12", "node_id": 2, "status": "backup", "maintenance": "none",
        "capabilities": [ "probe", "maint_set", "maint_full" ], "bin_sockets": [ "bin:10.0.0.12:3857" ] },
      { "mtu": 1500, "ip": "10.0.0.11", "node_id": 3, "status": "member", "maintenance": "none",
        "capabilities": [ "probe", "maint_set", "maint_full" ], "bin_sockets": [ "bin:10.0.0.11:3857" ] }
    ]
  }
]
```

```bash
opensips-cli -x mi clusterer:list_shtags        # "active" on the master, "backup" elsewhere
```

```json
[ { "Tag": "vip1", "Cluster": 1, "State": "active" } ]
```

```bash
opensips-cli -x mi clusterer_controller:list_config
```

```text title="abridged"
"member_count": 3,                  live members
"term": 4,                          rises with each confirmed change of master
"cluster_uuid_state": "established",
"master_timeout_ms": 3000,          the learned master watchdog (3 s floor)
"local_health": 0,                  0..8 - above 0, this node is in trouble
"tag_released": 0,
"cluster_capabilities": [ "probe", "maint_set", "maint_full" ]
```

### 5. Operate

```bash
# take this node out of election and sharing tags (a restart, an upgrade) - and back
opensips-cli -x mi clusterer_controller:maintenance cluster_id=1 state=evicted
opensips-cli -x mi clusterer_controller:maintenance cluster_id=1 state=none

# from any node: node 3 out, and replication towards it stopped too
opensips-cli -x mi clusterer_controller:maintenance cluster_id=1 state=full node_id=3

# on the master: pin the active tag to node 2, then back to automatic
opensips-cli -x mi clusterer_controller:shtag_force cluster_id=1 node_id=2
opensips-cli -x mi clusterer_controller:shtag_auto cluster_id=1
```

A master put into maintenance hands over at once. Maintenance is runtime-only: a restart brings the node back in service.

### The parameters that matter

| Parameter | Default | |
|---|---|---|
| *cluster* | - | *id=*, *multicast=* (required); optionally *password=*, *bin_socket=* (needed with several clusters), *manage_shtags=*, *master_stickiness=*, *uuid=* per cluster. Repeat for more clusters |
| *password* | *3eCrEt\*5629* | change it - every node of a cluster uses the same one |
| *my_ip* / *interface* | auto | this node's identity address, when it has more than one |
| *query_time* | 5 | seconds between a member's heartbeats; elections look back 3x, peers are dropped after 6x |
| *manage_shtags* | 1 | the master holds the active sharing tags |
| *master_stickiness* | 1 | a live master keeps the role when a higher IP joins |
| *on_config_mismatch* | reject | a joining node with different settings: *reject*, *warn* or *adopt* |
| *release_tag_when_isolated* | 0 | a master cut off from most of its members gives up the active tag |

---

## Documentation

- [When to use it](doc/when-to-use.md) - and when to configure *clusterer* statically instead
- [Installation](doc/installation.md) - dependencies, building, what changes in *clusterer*
- [Parameters](doc/parameters.md) - every parameter, with examples
- [MI commands](doc/mi.md)
- [Scripting](doc/scripting.md) - pseudo-variables, functions, events, messaging between nodes
- [Deployment](doc/deployment.md) - several clusters, native and controller clusters together, a full example, upgrading, limitations
- [How it works](doc/how-it-works.md) - discovery, the cluster identity, the master election, security
- [Wire protocol](doc/protocol.md) - packets and sequence diagrams
- [Failure detection and elections](doc/failure-detection.md) - and the papers behind them
- [Measurements](doc/measurements.md) - the netns suite, a six-node test cluster, CPU load
- [HA behaviour tests](doc/ha-tests.md) - manual failover tests on a three-node cluster
- [Tests](doc/testing.md) - running the suites yourself

<!-- CONTRIBUTORS -->

## License

All documentation files (i.e. .md extension) are licensed under the Creative Common License 4.0
