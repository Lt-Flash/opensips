#!/bin/sh
# run one scenario: $1 = name, $2 = seconds, $3 = procs per node
# seeding is done by the caller before invoking this
set -e
NAME=$1; SECS=$2; PPN=${3:-4}; NKEYS=${NKEYS:-30000}
R=/dn/cbench/results/$NAME
mkdir -p $R
T0=$(python3 -c "import time; print(time.time()+1)")
python3 /dn/cbench/cb_collector.py $(( SECS + 3 )) $R/stats.jsonl $T0 > $R/collector.log 2>&1 &
CPID=$!
i=0
for ip in 10.94.0.11 10.94.0.12 10.94.0.13; do
  p=0
  while [ $p -lt $PPN ]; do
    python3 /dn/cbench/cb_loader.py $ip $SECS $NKEYS $R/load-$i.pkl $T0 > /dev/null 2>&1 &
    i=$(( i + 1 )); p=$(( p + 1 ))
  done
done
wait
echo "scenario $NAME done: $(ls $R/load-*.pkl | wc -l) loader dumps"
