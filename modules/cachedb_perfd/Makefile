#
# cachedb_perfd — perfcached over the wire, via the vendored libperfd
#
# WARNING: do not run this directly, it should be run by the master Makefile
include ../../Makefile.defs
auto_gen=
NAME=cachedb_perfd.so

# the vendored client (tools/sync-libperfd.sh in Lt-Flash/perfcached)
extra_sources=vendor/lib/perfd.c vendor/src/json.c vendor/src/pc_noise.c

LIBS+=-lsodium -lpthread

include ../../Makefile.modules
