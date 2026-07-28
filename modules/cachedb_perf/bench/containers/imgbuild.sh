#!/bin/sh
set -e
nerdctl rm -f cp15build 2>/dev/null || true
nerdctl run -d --name cp15build -v /dn/alpimg/src:/src alpine:latest sleep 3600
nerdctl exec cp15build sh -c '
set -e
apk add --no-cache build-base bison flex openssl-dev libsodium-dev linux-headers iproute2 >/dev/null
cd /src
make -j16 opensips 2>&1 | tail -2
make -j16 modules modules="modules/proto_bin modules/clusterer modules/clusterer_controller modules/mi_datagram modules/cachedb_perf modules/sl modules/sipmsgops" 2>&1 | tail -2
mkdir -p /usr/local/opensips/modules
cp opensips /usr/local/opensips/
for m in proto_bin clusterer clusterer_controller mi_datagram cachedb_perf sl sipmsgops; do
  cp modules/$m/$m.so /usr/local/opensips/modules/
done
apk del build-base bison flex openssl-dev linux-headers >/dev/null 2>&1 || true
apk add --no-cache libssl3 libcrypto3 libsodium >/dev/null
/usr/local/opensips/opensips -V 2>&1 | head -2
'
nerdctl commit cp15build cp15:latest >/dev/null
nerdctl rm -f cp15build >/dev/null
nerdctl images | grep cp15
