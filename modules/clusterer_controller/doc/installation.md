[clusterer_controller](../README.md) / Installation

# Installation

## Dependencies

### OpenSIPS Modules

The following modules are required by this module:

- *proto_bin* — required so that BIN listeners are registered and available for discovery when *clusterer_controller* initialises and scans the proto_bin listener list.
- *clusterer* — required, and it *must* register every controller-managed cluster with *modparam("clusterer", "cluster_options", "cluster_id=N, use_controller=1")*. That per-cluster parameter is what pre-creates the controller-managed cluster stubs, marks them so they never touch the database, and arms the guard that stops the controller from driving a native cluster of the same id. The controller-managed ids declared here and the *cluster* entries configured in *clusterer_controller* must match *exactly*: if either side names a cluster the other does not, the controller refuses to start with an error naming the offending id — a managed id with no controller config has no BIN socket or crypto parameters, and a controller config for an unmanaged id has nothing to drive.

Both dependencies are declared in the module's *dep_export_t*. OpenSIPS will refuse to start if either dependency is not satisfied. This does *not* imply that the modules must appear in a particular order in the configuration file — OpenSIPS resolves the dependency at runtime and will initialize the required modules first regardless of *loadmodule* order.

This is also checked from the other side: if a *cluster_options* entry sets *use_controller=1* but the *clusterer_controller* module is not loaded at all, clusterer logs an error at startup, since the controller-managed cluster stubs would never obtain a node identity or form. clusterer itself keeps running, so any native or DB-backed clusters are unaffected.

**Hybrid deployments are unaffected.** *use_controller* is a per-cluster option carried in *cluster_options*, defaulting to *0*. In a hybrid instance (native and controller-managed clusters side by side), the controller-managed clusters each get a *cluster_options* line with *use_controller=1*, while the native ones are defined the usual way (DB rows or static *my_node_info*/*neighbor_node_info*) and need no *cluster_options* line. The exact-match check above compares only the controller-managed ids against the controller's *cluster* entries, so it never fires on a native cluster.

All other modules that use the clusterer interface (*tm*, *dialog*, *dispatcher*, *usrloc* etc.) may be loaded in any order relative to *clusterer_controller*. The clusterer module automatically creates a cluster stub when a *cluster_options* entry sets *use_controller=1* and a module attempts to register a capability for that cluster.

Because a controller-managed node receives its *node_id* at runtime (after it joins) rather than from static configuration at startup, any module that stamps this node's id into on-the-wire data must read it live per message instead of caching it once at initialisation — a value read at startup would be the not-yet-assigned placeholder, and it may also change on a re-election. In particular *tm*'s anycast support (*tm_replication_cluster* / *t_anycast_replicate()*) stamps this node's id into the *cid* Via parameter so that a reply landing on a different anycast member can be relayed to the node holding the transaction; it renders that parameter from the current id on each request, so anycast reply routing works unchanged under a controller-managed cluster.

### External Libraries or Applications

libsodium is required:

- *libsodium* (required) — provides the payload AEAD (XChaCha20-Poly1305), the bootstrap-key KDF (Argon2id), the Noise_NNpsk0 join handshake, and X25519 / HKDF-SHA256 / RNG. The build fails with a clear error if libsodium development files are not found. It is linked dynamically, so each target host also needs the libsodium runtime package (for example *libsodium23*). No other crypto library is used or linked.

The active suite is printed in the startup log (*crypto=...*).

## Building the Module

*clusterer_controller* is **excluded from the default build** (it is listed in *exclude_modules* in `Makefile.conf.template`), like the other modules that depend on external libraries. A stock OpenSIPS build therefore does not include it. To build it, add it to *include_modules* in your `Makefile.conf`:

```opensips
include_modules= clusterer_controller
```

then rebuild (*make all* / *make modules*). libsodium development files must be present on the build host or the build fails — see *External Libraries or Applications*.

**The clusterer module is unmodified when clusterer_controller is not built.** The controller integration on the *clusterer* side (the *clusterer_ctrl* API, the *cluster_options* modparam and all controller hooks) is compiled only when clusterer_controller is part of the build: the top-level Makefile detects this and passes *-DCLUSTERER_CTRL_SUPPORT* to the clusterer module. A build without clusterer_controller produces the stock clusterer module, unchanged in behaviour and exported interface - in particular the *cluster_options* parameter does not exist and is rejected as unknown. Enabling clusterer_controller automatically rebuilds clusterer with the support compiled in; the two are always a matched pair.
