#!/bin/bash
# TCP dispatch A/B on 225: shared vs per_worker, 3 repeats, cells tn (conn per call) and t1 (one conn)
R=$HOME/udp-rx-mode-rig; RES=$R/results-tcp; mkdir -p $RES
for rep in 1 2 3; do for mode in shared per_worker; do for cell in tn t1; do
  OUT=$RES/${mode}_${cell}_$rep
  case $cell in tn) rate=400; m=10000;; t1) rate=2000; m=50000;; esac
  ( sleep 5; timeout 60 /dn/sipp/sipp 10.22.20.225:5060 -sf $R/options.xml -i 10.22.20.222 -p 6060 -t $cell -r $rate -rp 1000 -m $m -l 20000 -max_socket 20000 -nostdin -trace_screen -screen_file /tmp/sipp_tcpab.txt -timeout 50s >/dev/null 2>&1 ) &
  timeout 120 ssh -o BatchMode=yes root@10.22.20.225 "bash -s -- $mode $cell /tmp/udpbal/out" < $R/tcpab_srv.sh 2>&1 | grep -v 'Too many open'
  wait
  mkdir -p $OUT; timeout 30 ssh -o BatchMode=yes root@10.22.20.225 'cat /tmp/udpbal/out/probe.txt' > $OUT/probe.txt; timeout 30 ssh -o BatchMode=yes root@10.22.20.225 'cat /tmp/udpbal/out/strace.txt' > $OUT/strace.txt
  grep -E 'Successful call|Failed call' /tmp/sipp_tcpab.txt | tr -s ' ' > $OUT/sipp.txt
  echo "$mode $cell rep$rep: $(head -1 $OUT/probe.txt | cut -c1-120) | $(grep -E 'TCP main|TCP receiver' $OUT/probe.txt | awk '{printf "%s:%s/%s ", $NF, $3, $4}')"
done; done; done
echo TCPAB_DONE
