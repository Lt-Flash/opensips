#!/bin/bash
# udpctr.sh - print the server's UDP counters as key=value (from /proc/net/snmp)
awk '/^Udp:/{ if (!h) { for (i = 2; i <= NF; i++) k[i] = $i; h = 1 } else { for (i = 2; i <= NF; i++) if (k[i] == "RcvbufErrors" || k[i] == "InErrors" || k[i] == "InDatagrams") printf "%s=%s ", k[i], $i; print "" } }' /proc/net/snmp
