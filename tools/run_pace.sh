#!/bin/bash
# run_pace.sh <ARM> <SERVER_IP> <PREFIX> "<EXTRA>"  - paced single-packet arrivals (prod-like)
set -u
ARM=$1; SRV=$2; PREFIX=$3; EXTRA=$4
R=$HOME/udp-rx-mode-rig
RES=${RES:-$R/results}; OUT=$RES/$ARM; mkdir -p $OUT; ME=10.22.20.222
srv() { if [ "$SRV" = "$ME" ]; then bash -s; else timeout 120 ssh -o BatchMode=yes root@$SRV 'bash -s'; fi; }
srvpy() { if [ "$SRV" = "$ME" ]; then python3 -u - "$@"; else timeout 120 ssh -o BatchMode=yes root@$SRV "python3 -u - $*"; fi; }
log() { echo "$(date +%H:%M:%S) $*" | tee -a $OUT/log.txt; }
log "PACE ARM=$ARM SRV=$SRV PREFIX=$PREFIX EXTRA='$EXTRA'"
echo "ACTION=start PREFIX=$PREFIX IP=$SRV EXTRA='$EXTRA' bash /tmp/udpbal/srvctl.sh" | srv | tee $OUT/start_pace.txt
grep -q STARTED $OUT/start_pace.txt || { log "START FAILED"; exit 1; }
sleep 2
for rate in 1000 3000; do
  log "pace poisson $rate/s x 20s"
  $R/pace $SRV 5060 $rate $((rate*20)) poisson > $OUT/pace_$rate.txt 2>&1 &
  pp=$!; sleep 4
  echo | srvpy 12 < /tmp/udpbal/probe3.py > $OUT/probe_pace_$rate.txt 2>&1
  wait $pp; cat $OUT/pace_$rate.txt | tee -a $OUT/log.txt
  sleep 1
done
echo "ACTION=stop bash /tmp/udpbal/srvctl.sh" | srv | tee -a $OUT/log.txt
log "pace done"
