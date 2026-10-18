# sample_all.sh <SECS> <OUTDIR> - every syscall tracepoint for the opensips tasks over one window (dedicated cell only)
set -u
S=$1 O=$2; mkdir -p $O
PIDS=$(pgrep -x opensips | paste -sd,)
perf stat -x, -o $O/perf_all.csv -e 'syscalls:sys_enter_*' -e context-switches -p $PIDS -- sleep $S 2>/dev/null &
pp=$!
python3 -u /tmp/udpbal/probe3.py $S > $O/probe.txt 2>&1
wait $pp
