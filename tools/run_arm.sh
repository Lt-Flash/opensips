#!/bin/bash
# run_arm.sh <ARM> <SERVER_IP> <PREFIX> "<EXTRA cfg lines, ; separated>" [RATE] [TCPRATE]
# client side = this host (222). Results in $RES/<ARM>/
set -u
ARM=$1; SRV=$2; PREFIX=$3; EXTRA=$4; RATE=${5:-4000}; TCPRATE=${6:-1000}
R=$HOME/udp-rx-mode-rig
RES=${RES:-$R/results}; OUT=$RES/$ARM; mkdir -p $OUT; cd $OUT
SIPP=/dn/sipp/sipp; ME=10.22.20.222
srv() { if [ "$SRV" = "$ME" ]; then bash -s; else timeout 120 ssh -o BatchMode=yes root@$SRV 'bash -s'; fi; }
srvpy() { if [ "$SRV" = "$ME" ]; then python3 -u - "$@"; else timeout 120 ssh -o BatchMode=yes root@$SRV "python3 -u - $*"; fi; }
log() { echo "$(date +%H:%M:%S) $*" | tee -a $OUT/log.txt; }
log "ARM=$ARM SRV=$SRV PREFIX=$PREFIX EXTRA='$EXTRA' RATE=$RATE TCPRATE=$TCPRATE"
echo "ACTION=start PREFIX=$PREFIX IP=$SRV EXTRA='$EXTRA' bash /tmp/udpbal/srvctl.sh" | srv | tee $OUT/start.txt
grep -q STARTED $OUT/start.txt || { log "START FAILED"; exit 1; }
echo "cat /tmp/udpbal/opensips.cfg" | srv > $OUT/opensips.cfg
echo "md5sum $PREFIX/sbin/opensips; strings $PREFIX/sbin/opensips | grep -c udp_worker_sockets" | srv > $OUT/artifact.txt
sleep 2
cell() { # cell <name> <sipp transport> <rate> <calls> <extra sipp args>
  local name=$1 tr=$2 rate=$3 calls=$4; shift 4
  log "cell $name: sipp -t $tr -r $rate -m $calls $*"
  timeout 90 $SIPP $SRV:5060 -sf $R/options.xml -i $ME -p 6060 -t $tr -r $rate -rp 1000 -m $calls -l 20000 -nostdin -trace_screen -screen_file $OUT/sipp_$name.txt -timeout 80s "$@" >$OUT/sipp_${name}.out 2>&1 &
  local sp=$!
  sleep 6
  echo | srvpy 12 < /tmp/udpbal/probe3.py > $OUT/probe_$name.txt 2>&1
  wait $sp; log "sipp exit $?"
  grep -E 'Successful call|Failed call|Retransmissions|Total-time|Response Time|^\s*[0-9]+ ms' $OUT/sipp_$name.txt | head -12 >> $OUT/log.txt
  sleep 2
}
cell udp_u1 u1 $RATE $((RATE*25))
cell udp_un un $RATE $((RATE*25)) -max_socket 4000
log "burst 32x200"; /tmp/udpbal/burst $SRV 5060 32 200 20 | tee -a $OUT/log.txt
log "burst 8x400"; /tmp/udpbal/burst $SRV 5060 8 400 10 | tee -a $OUT/log.txt
cell tcp_tn tn $TCPRATE $((TCPRATE*20)) -max_socket 4000
cell tcp_t1 t1 $TCPRATE $((TCPRATE*20))
echo "ACTION=stop bash /tmp/udpbal/srvctl.sh" | srv | tee -a $OUT/log.txt
log "done"
