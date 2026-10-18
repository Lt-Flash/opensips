#!/bin/bash
RUN=${RUN:-/tmp/udpbal}; mkdir -p $RUN; PID=$RUN/opensips.pid; CFG=$RUN/opensips.cfg
case "$ACTION" in
start)
  if [ -f $PID ] && kill -0 $(cat $PID) 2>/dev/null; then echo "ALREADY RUNNING $(cat $PID)"; exit 1; fi
  MPATH=$(ls -d $PREFIX/lib*/opensips/modules | head -1)
  TMPL=${TMPL:-srv.cfg.tmpl} python3 - "$IP" "$MPATH" "$EXTRA" > $CFG <<'PY'
import sys; ip, mpath, extra = sys.argv[1:4]
t = open('/tmp/udpbal/' + __import__('os').environ.get('TMPL', 'srv.cfg.tmpl')).read()
print(t.replace('__IP__', ip).replace('__MPATH__', mpath).replace('__EXTRA__', extra.replace(';', '\n')))
PY
  $PREFIX/sbin/opensips -C -f $CFG >/dev/null 2>$RUN/check.err || { echo "CFG CHECK FAILED"; cat $RUN/check.err; exit 1; }
  ulimit -n 65536; [ "${ALLOC:-F_MALLOC}" = HG_MALLOC ] && ulimit -l unlimited; $PREFIX/sbin/opensips -f $CFG -P $PID ${MEM:--m 64 -M 8} -a ${ALLOC:-F_MALLOC} ${OPTS:-} 2>$RUN/start.err || { echo "START FAILED"; cat $RUN/start.err; exit 1; }
  for i in $(seq 1 50); do sleep 0.2; ss -ulnp 2>/dev/null | grep -q ":5060 " && break; done
  sleep 1
  echo "STARTED pid=$(cat $PID) procs=$(pgrep -x opensips | wc -l) udpsocks=$(ss -uln | grep -c ':5060 ') rev=$($PREFIX/sbin/opensips -V 2>&1 | grep -i -m1 'git revision')"
  echo "cmdline: $(tr '\0' ' ' < /proc/$(cat $PID)/cmdline)"
  ;;
stop)
  if [ -f $PID ]; then p=$(cat $PID); kill $p 2>/dev/null; for i in $(seq 1 50); do sleep 0.2; pgrep -x opensips >/dev/null || break; done; fi
  if pgrep -x opensips >/dev/null; then echo "STILL RUNNING: $(pgrep -x opensips | tr '\n' ' ')"; else echo "STOPPED"; rm -f $PID; fi
  ;;
status)
  echo "procs=$(pgrep -x opensips | wc -l) pidfile=$(cat $PID 2>/dev/null)"; ss -ulnp | grep ':5060 ' | head -3
  ;;
esac
