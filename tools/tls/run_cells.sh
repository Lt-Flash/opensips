#!/bin/bash
# run_cells.sh <TAG> <PROTO> <ALLOC> <mode:rate ...> - named single cells, exact count of server dispatch drops
# The server is started with srv_tls.cfg.tmpl (SRVTMPL to override) from SRVPREFIX; one 30 s cell per argument.
set -u
TAG=$1 PROTO=$2 ALLOC=$3; shift 3; MODES="$*"
T=$(cd "$(dirname "$0")" && pwd); C=$T/../cps
SRV=192.0.2.10; HOSTS="192.0.2.21 192.0.2.22 192.0.2.23"; ME=192.0.2.21
NP=8; D=${D:-30}; SAMPLE_AT=8; SAMPLE=15; RATES=${RATES:-"10000 8000 6000 5000 4000 3000 2000 1000"}
RES=$T/results-$TAG; mkdir -p $RES
case $ALLOC in HG_MALLOC) MEM="-m 512:4096 -M 64:512";; *) MEM="-m 2048 -M 32";; esac
log(){ echo "$(date +%H:%M:%S) $*" | tee -a $RES/log.txt; }
rsh(){ local h=$1; shift; if [ $h = $ME ]; then bash -c "$*"; else timeout 300 ssh -o BatchMode=yes root@$h "$*"; fi; }
srv(){ timeout 300 ssh -o BatchMode=yes root@$SRV "$@"; }
gwp(){ echo ${GWPREFIX:-/opt/opensips}; }   # OpenSIPS install used for the client gateways
uas_start(){ for h in $HOSTS; do for i in $(seq 0 $((NP-1))); do
  rsh $h "ulimit -n 65536; mkdir -p /tmp/udpbal; sipp -sn uas -i $h -p $((5070+i)) -t u1 -default_behaviors all,-abortunexp -nostdin -bg -trace_err -error_file /tmp/udpbal/uas_$i.err 2>&1 | grep -o 'PID=\[[0-9]*\]' | tr -dc 0-9 > /tmp/udpbal/uas_$i.pid"; done; done; }
uas_stop(){ for h in $HOSTS; do rsh $h 'for f in /tmp/udpbal/uas_*.pid; do [ -s $f ] && kill $(cat $f) 2>/dev/null; done; rm -f /tmp/udpbal/uas_*.pid'; done; }
gw_start(){ for h in $HOSTS; do rsh $h "bash /tmp/udpbal/gwctl.sh start $(gwp $h) $h $SRV $PROTO"; done | tee -a $RES/log.txt; }
gw_stop(){ for h in $HOSTS; do rsh $h "bash /tmp/udpbal/gwctl.sh stop"; done | tee -a $RES/log.txt; }
cell(){ local mode=$1 rate=$2 rep=$3; local O=$RES/${rate}_${mode}_r${rep}; rm -rf $O; mkdir -p $O
  srv "ACTION=stop bash /tmp/udpbal/srvctl.sh" >/dev/null
  srv "ACTION=start LOGFG=1 TMPL=${SRVTMPL:-srv_tls.cfg.tmpl} ALLOC=$ALLOC MEM='$MEM' PREFIX=${SRVPREFIX:-/opt/opensips-udpbal-tls} IP=$SRV EXTRA='udp_rx_mode=\"auto\";tcp_dispatch_mode=\"$mode\"' bash /tmp/udpbal/srvctl.sh" > $O/start.txt 2>&1
  grep -q STARTED $O/start.txt || { log "$rate $mode r$rep: START FAILED $(tail -3 $O/start.txt | tr '\n' ' ')"; return 1; }
  srv 'echo "tcp_listeners=$(ss -tln | grep -c -E ":(5061|7443) ") unix_dgram_pairs=$(ss -xap | grep opensips | grep -c u_dgr) excl_epoll_regs=$(for p in $(pgrep -x opensips); do for fd in $(ls /proc/$p/fd); do [ "$(readlink /proc/$p/fd/$fd)" = "anon_inode:[eventpoll]" ] && grep -c "events: *1000001[0-9a-f]" /proc/$p/fdinfo/$fd; done; done 2>/dev/null | awk "{s+=\$1} END{print s+0}") alloc=$(tr "\0" " " < /proc/$(pgrep -o -x opensips)/cmdline | grep -o "\-a [A-Z_]*")"' > $O/mode.txt
  sleep 3
  local per=$(( (rate + NP*3/2) / (NP*3) )); local m=$((per*D)); local pids=""
  for h in $HOSTS; do for i in $(seq 0 $((NP-1))); do
    rsh $h "ulimit -n 65536; timeout -s INT -k 15 $((D+30)) sipp $h:$((5070+i)) -rsa $h:$((5080+i)) -sf /tmp/udpbal/uac_rr.xml -i $h -p $((6200+i)) -t u1 -default_behaviors all,-abortunexp -r $per -rp 1000 -m $m -l 200000 -nostdin -trace_screen -screen_file /tmp/udpbal/scr_$i.txt >/dev/null 2>&1; cat /tmp/udpbal/scr_$i.txt; rm -f /tmp/udpbal/scr_$i.txt" > $O/sipp_$(echo $h | tr . _)_$i.txt 2>&1 &
    pids="$pids $!"; done; done
  sleep $SAMPLE_AT
  ( srv "bash -s $SAMPLE /tmp/udpbal/out" < $T/sample_tls.sh; srv "cat /tmp/udpbal/out/probe.txt" > $O/probe.txt; srv "cat /tmp/udpbal/out/perf.csv" > $O/perf.csv ) &
  local sp=$!
  sleep 3; for h in $HOSTS; do echo "$h $(rsh $h 'python3 -u - 5' < $T/cliprobe2.py)"; done > $O/clientcpu.txt
  wait $sp; wait $pids
  srv "ACTION=stop bash /tmp/udpbal/srvctl.sh" >/dev/null
  srv 'L=/tmp/udpbal/opensips.log; echo "dispatch_drops=$(grep -a -c "failed to dispatch TCP message" $L) all_errors=$(grep -a -c -E "ERROR|CRIT" $L)"; echo "first: $(grep -a -m1 -E "ERROR|CRIT" $L | cut -c1-200)"' > $O/dispatch.txt 2>&1
  log "$PROTO $ALLOC $rate $mode r$rep: $(python3 -I $T/cellsum_tls.py $O $rate $D)"
}
holds(){ python3 -I -c "import json,sys; sys.exit(0 if json.load(open('$RES/${2}_${1}_r$3/sum.json'))['holds'] else 1)" 2>/dev/null; }
log "=== RUN $TAG proto=$PROTO alloc=$ALLOC cells=$MODES server=${SRVPREFIX:-/opt/opensips-udpbal-tls}"
for h in $HOSTS; do if [ $h = $ME ]; then mkdir -p /tmp/udpbal; cp $C/uac_rr.xml $T/gwctl.sh $T/gw.cfg.tmpl /tmp/udpbal/; mkdir -p /tmp/udpbal/certs; cp $T/certs/*.pem /tmp/udpbal/certs/; else rsh $h "mkdir -p /tmp/udpbal/certs"; scp -q -o BatchMode=yes $C/uac_rr.xml $T/gwctl.sh $T/gw.cfg.tmpl root@$h:/tmp/udpbal/; scp -q -o BatchMode=yes $T/certs/*.pem root@$h:/tmp/udpbal/certs/; fi; done
uas_start; gw_start
for spec in $MODES; do mode=${spec%%:*}; r=${spec##*:}; cell $mode $r 1; log "   $(cat $RES/${r}_${mode}_r1/dispatch.txt | tr '\n' ' ')"; done
gw_stop; uas_stop
log "=== DONE $TAG"
