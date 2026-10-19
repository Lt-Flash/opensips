#!/bin/bash
# cps_run_8m.sh <TAG> - does HG_MALLOC drop UDP datagrams with 8 MB socket buffers on the relayed-call scenario of 4.6?
# same server, build, proxy script and load as cps_run_hg.sh / cps_run_buf.sh, plus:
#   - maxbuffer 4 MB (= 8 MB on the live socket, read back from ss), as in the F_MALLOC buffer runs
#   - RcvbufErrors/InErrors counted over the WHOLE cell (server /proc/net/snmp before the load and after sipp exits)
#   - positive controls in the same run: F_MALLOC 8 MB at 12,000/s (known to drop), HG_MALLOC default buffer at 12,000/s
# server: /tmp/udpbal/ holds srvctl.sh, srv_cps.cfg.tmpl, probe3.py (tools/) and udpctr.sh (tools/cps/)
set -u
TAG=$1
R=$(cd "$(dirname "$0")/.." && pwd); C=$R/cps
SRV=192.0.2.10; HOSTS="192.0.2.21 192.0.2.22 192.0.2.23"; ME=192.0.2.21
PREFIX=${PREFIX:-/opt/opensips-udpbal}
NP=4; D=30; SAMPLE_AT=8; SAMPLE=15
RATES=${RATES:-"12000 16000 20000 22000 24000"}
RES=$C/results-$TAG; mkdir -p $RES
log(){ echo "$(date +%H:%M:%S) $*" | tee -a $RES/log.txt; }
rsh(){ local h=$1; shift; if [ $h = $ME ]; then bash -c "$*"; else timeout 300 ssh -o BatchMode=yes root@$h "$*"; fi; }
srv(){ timeout 300 ssh -o BatchMode=yes root@$SRV "$@"; }
uas_start(){ for h in $HOSTS; do for i in $(seq 0 $((NP-1))); do
  rsh $h "ulimit -n 65536; mkdir -p /tmp/udpbal; sipp -sn uas -i $h -p $((5070+i)) -t u1 -default_behaviors all,-abortunexp -nostdin -bg -trace_err -error_file /tmp/udpbal/uas_$i.err 2>&1 | grep -o 'PID=\[[0-9]*\]' | tr -dc 0-9 > /tmp/udpbal/uas_$i.pid; echo \"\$(hostname) uas $i pid \$(cat /tmp/udpbal/uas_$i.pid)\""; done; done | tee -a $RES/log.txt; }
uas_stop(){ for h in $HOSTS; do rsh $h 'for f in /tmp/udpbal/uas_*.pid; do [ -s $f ] && kill $(cat $f) 2>/dev/null; done; rm -f /tmp/udpbal/uas_*.pid; sleep 1; echo "$(hostname) sipp left: $(pgrep -c -x sipp)"'; done | tee -a $RES/log.txt; }
# cell <ALLOC> <BUF: 8m|def> <mode> <rate> <rep>
cell(){ local alloc=$1 buf=$2 mode=$3 rate=$4 rep=$5; local O=$RES/${alloc}_${buf}_${rate}_${mode}_r$rep; rm -rf $O; mkdir -p $O
  local mem extra="udp_rx_mode=\"$mode\""
  case $alloc in HG_MALLOC) mem='-m 512:4096 -M 64:512';; *) mem='-m 2048 -M 32';; esac
  [ $buf = 8m ] && extra="$extra;maxbuffer=4194304"
  srv "ACTION=stop bash /tmp/udpbal/srvctl.sh" >/dev/null
  srv "ACTION=start TMPL=srv_cps.cfg.tmpl ALLOC=$alloc MEM='$mem' PREFIX=$PREFIX IP=$SRV EXTRA='$extra' bash /tmp/udpbal/srvctl.sh" > $O/start.txt 2>&1
  grep -q STARTED $O/start.txt || { log "$alloc $buf $rate $mode r$rep: START FAILED $(tail -3 $O/start.txt | tr '\n' ' ')"; return 1; }
  srv 'echo "sockets5060=$(ss -uln | grep -c ":5060 ") rcvbuf=$(ss -uanm "sport = :5060" | grep -o "rb[0-9]*" | sort | uniq -c | tr -s " " | tr "\n" " ") alloc=$(tr "\0" " " < /proc/$(pgrep -o -x opensips)/cmdline | grep -o "\-a [A-Z_]*") $(grep -E "HugePages_Total|AnonHugePages" /proc/meminfo | tr -s " " | tr "\n" " ")"' > $O/mode.txt
  sleep 2
  srv "bash /tmp/udpbal/udpctr.sh; date +%s" > $O/ctr_before.txt
  local per=$(( (rate + NP*3/2) / (NP*3) )); local m=$((per*D)); local pids=""
  for h in $HOSTS; do for i in $(seq 0 $((NP-1))); do
    rsh $h "ulimit -n 65536; timeout -s INT -k 15 $((D+30)) sipp $h:$((5070+i)) -rsa $SRV:5060 -sf /tmp/udpbal/uac_rr.xml -i $h -p $((6200+i)) -t u1 -default_behaviors all,-abortunexp -r $per -rp 1000 -m $m -l 200000 -nostdin -trace_screen -screen_file /tmp/udpbal/scr_$i.txt >/dev/null 2>&1; cat /tmp/udpbal/scr_$i.txt; rm -f /tmp/udpbal/scr_$i.txt" > $O/sipp_$(echo $h | tr . _)_$i.txt 2>&1 &
    pids="$pids $!"; done; done
  sleep $SAMPLE_AT
  ( srv "bash -s $SAMPLE /tmp/udpbal/out" < $C/sample.sh; srv "cat /tmp/udpbal/out/probe.txt" > $O/probe.txt; srv "cat /tmp/udpbal/out/perf.csv" > $O/perf.csv ) &
  local sp=$!
  sleep 3; for h in $HOSTS; do echo "$h $(rsh $h 'python3 -u - 5' < $C/cliprobe.py)"; done > $O/clientcpu.txt
  wait $sp; for p in $pids; do wait $p; done
  srv "bash /tmp/udpbal/udpctr.sh; date +%s" > $O/ctr_after.txt
  local t0=$(tail -1 $O/ctr_before.txt)
  srv "journalctl --since @$t0 --no-pager -o cat 2>/dev/null | grep -a -c -E 'ERROR|CRIT'" > $O/errors.txt 2>&1
  srv "ACTION=stop bash /tmp/udpbal/srvctl.sh" >/dev/null
  local d=$(python3 -I - $O <<'PY'
import sys
O = sys.argv[1]
def rd(f):
    return dict(kv.split('=') for kv in open(O + '/' + f).readline().split())
a, b = rd('ctr_before.txt'), rd('ctr_after.txt')
print(' '.join('cell_%s=%d' % (k, int(b[k]) - int(a[k])) for k in ('RcvbufErrors', 'InErrors', 'InDatagrams')))
PY
)
  echo "$d" > $O/drops.txt
  log "$alloc $buf $rate $mode r$rep: $d errors=$(cat $O/errors.txt) $(grep -o 'rcvbuf=.*alloc=[^ ]* [^ ]*' $O/mode.txt | cut -c1-60) | $(python3 -I $C/cellsum.py $O $rate $D)"
}
log "=== RUN $TAG server $SRV (kernel $(srv uname -r)) prefix $PREFIX ($(srv "$PREFIX/sbin/opensips -V 2>&1 | grep -i -m1 'git revision'; md5sum < $PREFIX/sbin/opensips | cut -c1-12" | tr '\n' ' ')) rates=$RATES"
for h in $HOSTS; do rsh $h "mkdir -p /tmp/udpbal"; [ $h = $ME ] && cp $C/uac_rr.xml /tmp/udpbal/ || scp -q -o BatchMode=yes $C/uac_rr.xml root@$h:/tmp/udpbal/; done
uas_start
[ "${CONTROLS:-1}" = 1 ] && cell F_MALLOC 8m shared 12000 1
for rate in $RATES; do for rep in 1 2; do
  if [ $rep = 1 ]; then order="shared auto exclusive"; else order="exclusive auto shared"; fi
  for mode in $order; do cell HG_MALLOC 8m $mode $rate $rep; done
done; done
if [ "${CONTROLS:-1}" = 1 ]; then cell HG_MALLOC def shared 12000 1; cell F_MALLOC 8m shared 12000 2; fi
uas_stop
log "=== DONE $TAG"
