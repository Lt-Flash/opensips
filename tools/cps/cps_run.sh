#!/bin/bash
# cps_run.sh <TAG> <REPS> <rate list...>   rates = TOTAL calls/s across the pool
# Server 224 (stateful proxy, 8 UDP workers). Pool: 222, 223, 23.190, NP UAC/UAS pairs per host.
# Per rate, modes interleaved: rep 1 in order shared,auto,exclusive; rep 2 reversed (ABC/CBA).
set -u
TAG=$1 REPS=$2; shift 2; RATES="$*"
R=$(cd "$(dirname "$0")/.." && pwd); C=$R/cps
SRV=192.0.2.10; HOSTS="192.0.2.21 192.0.2.22 192.0.2.23"; ME=192.0.2.21
NP=${NP:-4}; D=${D:-30}; SAMPLE_AT=${SAMPLE_AT:-8}; SAMPLE=${SAMPLE:-15}
RES=$C/results-$TAG; mkdir -p $RES
log(){ echo "$(date +%H:%M:%S) $*" | tee -a $RES/log.txt; }
rsh(){ local h=$1; shift; if [ $h = $ME ]; then bash -c "$*"; else timeout 300 ssh -o BatchMode=yes root@$h "$*"; fi; }
srv(){ timeout 300 ssh -o BatchMode=yes root@$SRV "$@"; }
extra(){ case $1 in shared) echo 'udp_rx_mode="shared"';; auto) echo 'udp_rx_mode="auto"';; exclusive) echo 'udp_rx_mode="exclusive"';; esac; }
# UAS: NP per host, persistent for the whole run
uas_start(){ for h in $HOSTS; do for i in $(seq 0 $((NP-1))); do
  rsh $h "ulimit -n 65536; mkdir -p /tmp/udpbal; sipp -sn uas -i $h -p $((5070+i)) -t u1 -default_behaviors all,-abortunexp -nostdin -bg -trace_err -error_file /tmp/udpbal/uas_$i.err 2>&1 | grep -o 'PID=\[[0-9]*\]' | tr -dc 0-9 > /tmp/udpbal/uas_$i.pid; echo \"\$(hostname) uas $i pid \$(cat /tmp/udpbal/uas_$i.pid)\""; done; done | tee -a $RES/log.txt; }
uas_stop(){ for h in $HOSTS; do rsh $h 'for f in /tmp/udpbal/uas_*.pid; do [ -s $f ] && kill $(cat $f) 2>/dev/null; done; sleep 1; echo "$(hostname) sipp left: $(pgrep -c -x sipp)"'; done | tee -a $RES/log.txt; }
cell(){ local mode=$1 rate=$2 rep=$3 O=$RES/${rate}_${mode}_r$3; mkdir -p $O
  srv "ACTION=stop bash /tmp/udpbal/srvctl.sh" >/dev/null
  srv "ACTION=start TMPL=srv_cps.cfg.tmpl MEM='-m 2048 -M 32' PREFIX=/opt/opensips-udpbal IP=$SRV EXTRA='$(extra $mode)' bash /tmp/udpbal/srvctl.sh" > $O/start.txt 2>&1
  grep -q STARTED $O/start.txt || { log "$rate $mode r$rep: START FAILED"; cat $O/start.txt | tail -5; return 1; }
  srv 'echo "sockets5060=$(ss -uln | grep -c ":5060 ") excl_epoll_regs=$(for p in $(pgrep -x opensips); do for fd in $(ls /proc/$p/fd); do [ "$(readlink /proc/$p/fd/$fd)" = "anon_inode:[eventpoll]" ] && grep -c "events: *1000001[0-9a-f]" /proc/$p/fdinfo/$fd; done; done 2>/dev/null | awk "{s+=\$1} END{print s+0}")"' > $O/mode.txt
  sleep 2
  local per=$(( (rate + NP*3/2) / (NP*3) )); local m=$((per*D)); local pids=""
  for h in $HOSTS; do for i in $(seq 0 $((NP-1))); do
    rsh $h "ulimit -n 65536; timeout -s INT -k 15 $((D+30)) sipp $h:$((5070+i)) -rsa $SRV:5060 -sf /tmp/udpbal/uac_rr.xml -i $h -p $((6200+i)) -t u1 -default_behaviors all,-abortunexp -r $per -rp 1000 -m $m -l 200000 -nostdin -trace_screen -screen_file /tmp/udpbal/scr_$i.txt >/dev/null 2>&1; cat /tmp/udpbal/scr_$i.txt; rm -f /tmp/udpbal/scr_$i.txt" > $O/sipp_$(echo $h | tr . _)_$i.txt 2>&1 &
    pids="$pids $!"; done; done
  sleep $SAMPLE_AT
  ( srv "bash -s $SAMPLE /tmp/udpbal/out" < $C/sample.sh; srv "cat /tmp/udpbal/out/probe.txt" > $O/probe.txt; srv "cat /tmp/udpbal/out/perf.csv" > $O/perf.csv ) &
  local sp=$!
  sleep 3; for h in $HOSTS; do echo "$h $(rsh $h 'python3 -u - 5' < $C/cliprobe.py)"; done > $O/clientcpu.txt
  wait $sp; wait $pids
  srv "ACTION=stop bash /tmp/udpbal/srvctl.sh" >/dev/null
  log "$rate $mode r$rep: $(python3 -I $C/cellsum.py $O $rate $D)"
}
log "=== RUN $TAG reps=$REPS rates=$RATES NP=$NP D=$D server $SRV (kernel $(srv uname -r)) pool $HOSTS"
for h in $HOSTS; do rsh $h "mkdir -p /tmp/udpbal" ; [ $h = $ME ] && cp $C/uac_rr.xml /tmp/udpbal/ || scp -q -o BatchMode=yes $C/uac_rr.xml root@$h:/tmp/udpbal/; done
uas_start
for rate in $RATES; do for rep in $(seq 1 $REPS); do
  if [ $((rep % 2)) = 1 ]; then order="shared auto exclusive"; else order="exclusive auto shared"; fi
  for mode in $order; do cell $mode $rate $rep; done
done; done
uas_stop
log "=== DONE $TAG"
