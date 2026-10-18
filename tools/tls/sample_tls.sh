# sample_tls.sh <SECS> <OUTDIR> - perf counters of all opensips tasks + role-aware /proc probe over the same window
set -u
S=$1 O=$2; mkdir -p $O
PIDS=$(pgrep -x opensips | paste -sd,)
perf stat -x, -o $O/perf.csv -e task-clock,context-switches,raw_syscalls:sys_enter,syscalls:sys_enter_sched_yield,syscalls:sys_enter_read,syscalls:sys_enter_write,syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto,syscalls:sys_enter_epoll_wait -e syscalls:sys_exit_recvfrom --filter 'ret == -11' -p $PIDS -- sleep $S 2>/dev/null &
pp=$!
FIFO=/tmp/opensips_fifo python3 -u /tmp/udpbal/probe4.py $S > $O/probe.txt 2>&1
wait $pp
