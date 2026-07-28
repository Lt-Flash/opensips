#!/bin/sh
set -e
SCEN=${SCEN:-s4_pkts}
BR=$(ip -o link show type bridge | grep -oE "br-[a-z0-9]+" | head -1)
python3 /dn/cbench/cb_seed.py thirds
rm -rf /dn/cbench/results/$SCEN
mkdir -p /dn/cbench/results/$SCEN
tcpdump -i $BR -nn -q -w /dn/cbench/results/$SCEN/plane.pcap udp port 4499 or tcp port 5599 2>/dev/null &
TD=$!
sleep 1
sh /dn/cbench/cb_run.sh $SCEN 30 4
sleep 1
kill $TD; wait $TD 2>/dev/null || true
echo "--- packet accounting ---"
tcpdump -r /dn/cbench/results/$SCEN/plane.pcap -nn -q 2>/dev/null | awk "
/239.0.94.1.4499/ {m++; next}
/\.4499/ {u++; next}
/\.5599/ {b++}
END {printf \"mcast(pull req + beacons)=%d  unicast:4499(replies)=%d  bin:5599=%d\n\", m, u, b}"
