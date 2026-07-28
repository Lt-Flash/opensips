#!/bin/sh
# 3-node cp15 cluster in containers: n1/n2/n3 on 10.94.0.11/12/13,
# clusterer_controller over 239.0.94.1:4499, cachedb_perf pull via clctr.
set -e
TRANSPORT=${TRANSPORT:-clctr}
D=/dn/alpimg
mkdir -p $D/etc $D/run
for c in n1 n2 n3; do nerdctl rm -f $c 2>/dev/null || true; done
nerdctl network rm cp15net 2>/dev/null || true
nerdctl network create cp15net --subnet 10.94.0.0/24 >/dev/null
rm -f $D/run/mi*.sock

for N in 1 2 3; do
cat > $D/etc/n$N.cfg <<EOF
log_level=2
max_while_loops=1000000
stderror_enabled=yes
syslog_enabled=no
udp_workers=6
socket=udp:10.94.0.1$N:5060
socket=bin:10.94.0.1$N:5599
mpath="/usr/local/opensips/modules/"
loadmodule "proto_udp"
loadmodule "proto_bin.so"
loadmodule "clusterer.so"
loadmodule "clusterer_controller.so"
loadmodule "mi_datagram.so"
loadmodule "cachedb_perf.so"
loadmodule "sl.so"
loadmodule "sipmsgops.so"
modparam("clusterer", "db_mode", 0)
modparam("clusterer", "my_node_id", $N)
modparam("clusterer", "cluster_options", "cluster_id=9, use_controller=1")
modparam("clusterer_controller", "my_ip", "10.94.0.1$N")
modparam("clusterer_controller", "password", "cp15-container-bench-long-password")
modparam("clusterer_controller", "cluster", "id=9,multicast=239.0.94.1:4499,bin_socket=bin:10.94.0.1$N:5599")
modparam("clusterer_controller", "consumer_rate_limit", 1000000)
modparam("mi_datagram", "socket_name", "udp:10.94.0.1$N:8787")
modparam("cachedb_perf", "cache_collections", "th=16")
modparam("cachedb_perf", "cachedb_url", "perf:///th")
modparam("cachedb_perf", "sync_cluster_id", 9)
modparam("cachedb_perf", "replicate_collections", "th")
modparam("cachedb_perf", "pull_transport", "$TRANSPORT")
modparam("cachedb_perf", "pull_timeout_ms", 100)
modparam("cachedb_perf", "pull_on_miss", 1)

route {
    if (\$hdr(X-K) != NULL) {
        if (cache_fetch("perf", "th:\$hdr(X-K)", \$var(v)))
            sl_send_reply(200, "h");
        else
            sl_send_reply(404, "m");
        exit;
    }
    if (\$hdr(X-Seed) != NULL) {
        # X-Seed: <from> and X-Cnt: <count> - store k<from> .. k<from+count-1>
        \$var(i) = \$(hdr(X-Seed){s.int});
        \$var(end) = \$var(i) + \$(hdr(X-Cnt){s.int});
        while (\$var(i) < \$var(end)) {
            cache_store("perf", "th:k\$var(i)", "value-of-reasonable-size-to-mimic-a-th-blob-0123456789abcdef", 0);
            \$var(i) = \$var(i) + 1;
        }
        sl_send_reply(200, "seeded");
        exit;
    }
    sl_send_reply(200, "ok");
    exit;
}
EOF
nerdctl run -d --name n$N --net cp15net --ip 10.94.0.1$N \
  --cap-add NET_ADMIN \
  -v $D/etc:/etc/osips -v $D/run:/run/mi \
  cp15:latest sh -c "ip route add 239.0.0.0/8 dev eth0; exec /usr/local/opensips/opensips -f /etc/osips/n$N.cfg -F -a F_MALLOC -m 256" >/dev/null
done

# the CNI bridge exists once the first container is up - kill IGMP snooping
sleep 2
BR=$(ip -o link show type bridge | grep -oE "br-[a-z0-9]+" | head -1)
[ -n "$BR" ] && echo 0 > /sys/class/net/$BR/bridge/multicast_snooping
echo "bridge=$BR snooping=$(cat /sys/class/net/$BR/bridge/multicast_snooping)"
sleep 8
for N in 1 2 3; do
  echo "--- n$N ---"
  nerdctl logs n$N 2>&1 | grep -aE "assigned us node_id|ERROR|CRITICAL" | tail -3
done
