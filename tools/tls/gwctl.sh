# gwctl.sh start|stop <PREFIX> <IP> <SRV> <PROTO>  - on a client host, the TLS/WSS gateway
set -u
A=$1 P=${2:-} IP=${3:-} SRV=${4:-} PROTO=${5:-}; G=/tmp/udpbal/gw; mkdir -p $G
case $A in
start)
  MP=$(ls -d $P/lib*/opensips/modules | head -1)
  sed -e "s#__IP__#$IP#g; s#__SRV__#$SRV#g; s#__PROTO__#$PROTO#g; s#__MPATH__#$MP#g" /tmp/udpbal/gw.cfg.tmpl > $G/gw.cfg
  ulimit -n 65536; ulimit -l unlimited; $P/sbin/opensips -f $G/gw.cfg -P $G/gw.pid -m 512:2048 -M 64:256 -a HG_MALLOC 2>$G/start.err || { echo "$(hostname) GW START FAILED"; tail -5 $G/start.err; exit 1; }
  sleep 2; echo "$(hostname) gw $PROTO up: procs=$(pgrep -c -x opensips) pid=$(cat $G/gw.pid)";;
stop)
  [ -f $G/gw.pid ] && kill $(cat $G/gw.pid) 2>/dev/null; for i in $(seq 1 25); do sleep 0.2; [ -f $G/gw.pid ] && kill -0 $(cat $G/gw.pid) 2>/dev/null || break; done
  rm -f $G/gw.pid; echo "$(hostname) gw stopped, opensips left $(pgrep -c -x opensips)";;
esac
