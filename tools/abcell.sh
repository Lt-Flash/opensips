#!/bin/bash
# abcell.sh <ARM> <CELL> <RATE per host> <SECS> <TRANSPORT un|t1|tn> <TARGET: priv|anycast>
# Load from 222, 223, 23.190 (one sipp each) at the test SBCs; samples all six test nodes in the middle.
set -u
ARM=$1 CELL=$2 RATE=$3 SECS=$4 TR=$5 TGT=$6
R=$HOME/udp-rx-mode-rig
OUT=$R/results-test/$ARM/$CELL; mkdir -p $OUT
HOSTS="<load-host> <load-host> <node>"; SBC="191 192 193"
ME=<load-host>
log(){ echo "$(date +%H:%M:%S) $*" | tee -a $OUT/log.txt; }
rsh(){ local h=$1; shift; if [ $h = $ME ]; then bash -c "$*"; else timeout $((SECS+90)) ssh -o BatchMode=yes root@$h "$*"; fi; }
cat $R/options_dom.xml | for h in <load-host> <node>; do ssh -o BatchMode=yes root@$h 'mkdir -p /tmp/udpbal && cat > /tmp/udpbal/options_dom.xml'; done < $R/options_dom.xml
for h in <load-host> <node>; do scp -q -o BatchMode=yes $R/options_dom.xml $R/probe3.py root@$h:/tmp/udpbal/; done
for n in 191 192 193 194 195 196; do scp -q -o BatchMode=yes $R/probe4.py root@192.0.2.$n:/tmp/probe4.py; done
CALLS=$((RATE*SECS)); i=0; pids=""
for h in $HOSTS; do i=$((i+1))
  case $TGT in priv) t=192.0.2.$(echo $SBC | cut -d' ' -f$i):5060;; anycast) t=<vip>:7060;; esac
  log "sipp on $h -> $t: -t $TR -r $RATE -m $CALLS"
  cmd="ulimit -n 65536; mkdir -p /tmp/udpbal; sipp $t -sf /tmp/udpbal/options_dom.xml -i $h -p 6100 -t $TR -r $RATE -rp 1000 -m $CALLS -l 50000 -max_socket 20000 -nostdin -trace_screen -screen_file /tmp/udpbal/screen_${ARM}_${CELL}.txt -timeout $((SECS+40))s >/tmp/udpbal/sipp_${ARM}_${CELL}.out 2>&1"
  [ $h = $ME ] && cmd=${cmd//\/tmp\/udpbal\/options_dom.xml/$R/options_dom.xml}
  rsh $h "$cmd" & pids="$pids $!"
done
sleep 8
log "probe 12 s on all six nodes"
for n in 191 192 193 194 195 196; do ( timeout 60 ssh -o BatchMode=yes root@192.0.2.$n 'python3 -u /tmp/probe4.py 12' > $OUT/probe_$n.txt 2>&1 ) & done
wait
for h in $HOSTS; do rsh $h "cat /tmp/udpbal/screen_${ARM}_${CELL}.txt" > $OUT/sipp_$(echo $h | tr . _).txt 2>/dev/null; done
for h in $HOSTS; do f=$OUT/sipp_$(echo $h | tr . _).txt; printf "%s: " $h; grep -E 'Successful call|Failed call|Retransmissions' $f | tr -s ' ' | tr '\n' ' ' ; echo; done | tee -a $OUT/log.txt
for n in 191 192 193 194 195 196; do printf ".$n: "; head -2 $OUT/probe_$n.txt | tr '\n' ' '; echo; done | tee -a $OUT/log.txt
log "cell done"
