# sample.sh <SECS> <OUTDIR> - on the server: perf counters of all opensips tasks + /proc probe over the same window
set -u
S=$1 O=$2; mkdir -p $O
PIDS=$(pgrep -x opensips | paste -sd,)
perf stat -x, -o $O/perf.csv -e task-clock,context-switches,cpu-migrations,raw_syscalls:sys_enter,syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto,syscalls:sys_enter_epoll_wait,syscalls:sys_enter_futex -e syscalls:sys_exit_recvfrom --filter 'ret == -11' -p $PIDS -- sleep $S 2>/dev/null &
pp=$!
python3 -u /tmp/udpbal/probe3.py $S > $O/probe.txt 2>&1
wait $pp
