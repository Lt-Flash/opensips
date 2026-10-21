[clusterer_controller](../README.md) / When to use it

# When to use it

*clusterer_controller* replaces the static *clusterer* topology (*my_node_info* / *neighbor_node_info*, or the clusterer database) with nodes that find each other over IP multicast. It suits clusters whose membership changes - nodes replaced, scaled, rebuilt - and where one node at a time must hold the active sharing tag.

**Use it when:**

- the nodes can reach each other by **IPv4 multicast** - one L2 segment or VLAN (with IGMP snooping), or a routed network with PIM;
- you want **one configuration for every node**: a multicast group, a cluster id and a password, no node ids or neighbour addresses;
- nodes come and go (VMs or containers rebuilt, addresses reassigned) and the cluster should take them in and drop them without configuration changes;
- one node at a time must hold the active sharing tag, with the tag following failover and a maintenance mode for rolling restarts.

**Do not use it - configure *clusterer* with its own modparams or database - when:**

- the network has **no multicast** between the nodes: most public clouds by default, overlays without BUM replication, NAT between nodes. The module has no unicast discovery;
- the nodes are not on Linux (it needs *timerfd* and *SO_BINDTODEVICE*), or the cluster network is IPv6-only;
- you need **consensus or fencing**: this is not a quorum protocol. A partitioned cluster elects a master on each side and, by default, both hold the active tag until the partition heals (*release_tag_when_isolated* makes a cut-off master give it up);
- nodes must authenticate individually: every holder of the cluster password is a member;
- the nodes are far apart (WAN): the timers are tuned for a LAN (keepalives every second, failover in seconds).

| Pros | Cons |
|---|---|
| Same configuration on every node; node ids, BIN addresses and sharing tags handled automatically | Needs IPv4 multicast; Linux only; IPv6 not yet |
| Deterministic master (highest IP, sticky); failover in about 5 s, a graceful handover in under 1 s | Not a consensus protocol: a partition has two masters until it heals |
| Encrypted, authenticated control plane (XChaCha20-Poly1305, Argon2id); foreign and mismatched clusters refused | One shared secret per cluster, no per-node identity |
| Learns its timing from the network, asks the cluster before electing, and heals partitions to the larger side | A wire-version change needs a whole-cluster upgrade |
| Maintenance mode (this node or any member) for rolling restarts; capability bits for rolling upgrades | Requires libsodium; MEMBER_LIST fragments beyond about 80 nodes at a 1500-byte MTU |
| Stock *clusterer* is unchanged unless the module is built (build-time flag) | |
