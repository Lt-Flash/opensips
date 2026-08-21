/*
 * user location clustering
 *
 * Copyright (C) 2013-2019 OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _USRLOC_CLUSTER_H_
#define _USRLOC_CLUSTER_H_

#include "../../ut.h"
#include "../../bin_interface.h"
#include "../../socket_info.h"
#include "../../resolve.h"
#include "../../timer.h"
#include "../clusterer/api.h"

#include "urecord.h"

#define REPL_URECORD_INSERT  1
#define REPL_URECORD_DELETE  2
#define REPL_UCONTACT_INSERT 3
#define REPL_UCONTACT_UPDATE 4
#define REPL_UCONTACT_DELETE 5
/* pull-sharing: never sent as a packet - it is the self-describing
 * format of the record blob stored in the shared cache collection */
#define REPL_UREC_BLOB       6

#define UL_BIN_V2      2
#define UL_BIN_V3      3 // added "cmatch" (default: CT_MATCH_CONTACT_CALLID)
#define UL_BIN_V4      4 // changed 'ct.cflags' from int bitmask to string repr
#define UL_BIN_V5      5 // added 'r.kv_storage' to AoR INSERT packets
#define UL_BIN_VERSION UL_BIN_V5

extern int location_cluster;
extern struct clusterer_binds clusterer_api;
extern str ul_shtag_key;

extern int ul_ha_cluster;
extern str ul_ha_shtag;

extern str contact_repl_cap;

/* pull-sharing: this node's clusterer id, learned at init (0 = unknown) */
extern int ul_my_nid;
/* pull-sharing: contact K/V key holding the owner's clusterer id.  Riding
 * the K/V store means it persists through the kv_store DB column, the bin
 * packets and the pull blob without any schema or wire-format change. */
extern str ul_onid_key;

int ul_init_cluster(void);

/* the owner node id recorded on a contact, 0 if none */
int ul_ct_owner_nid(ucontact_t *c);

/* record this node as the contact's owner (local registration events) */
void ul_ct_stamp_owner(ucontact_t *c);

/* pull-sharing: (re)publish the record's blob into the shared cache
 * collection, or withdraw it.  No-ops outside CM_PULL_SHARING. */
void ul_pull_publish(urecord_t *r);
void ul_pull_unpublish(urecord_t *r);

/* pull-sharing: restart bootstrap - publish every record this node owns
 * at least one contact of (call once, after the ledger preload) */
void ul_pull_publish_all_owned(void);

/* pull-sharing: merge a pulled blob into local memory.  The caller MUST
 * hold the domain lock for @aor.  Returns the number of live contacts
 * absorbed, or -1 for a blob this build refuses (foreign format). */
int ul_pull_absorb_blob(struct udomain *domain, str *aor, str *blob);

/* pull-sharing: async pull pair for transaction-suspending callers.
 * start: 1 = suspended on @fd, 0 = known absent, -1 = cannot (fall back
 * to the blocking path).  finish: 1 absorbed / 0 absent / -1 no answer. */
int ul_pull_start(struct udomain *domain, str *aor, int *fd,
		unsigned int *handle);
int ul_pull_finish(struct udomain *domain, str *aor, unsigned int handle);

/* pull-sharing: the shared-cache half of get_urecord_or_pull().  The
 * caller MUST hold the domain lock for @aor.  @hint_nid > 0 asks that
 * node first (falling back to the broadcast); 0 asks everyone.
 * Returns like get_urecord: 0 found (@r set), 1 not found. */
int ul_pull_fetch(struct udomain *domain, str *aor, urecord_t **r,
		int hint_nid);

/* pull-sharing ownership: is this node responsible for the contact
 * (pinging, DB maintenance)?  Unicast: accepted on a local socket means
 * ours - a pulled or foreign-row contact carries a non-local socket that
 * resolved to NULL.  Anycast: the socket is local on every node, so the
 * recorded owner id decides. */
int ul_ct_is_mine(ucontact_t *c);

#define _is_my_ucontact(__ct) \
	(!__ct->shtag.s || \
	 clusterer_api.shtag_get(&__ct->shtag, location_cluster) \
		== SHTAG_STATE_ACTIVE)

#define ul_is_active_node() \
	(!ul_ha_cluster || !ul_ha_shtag.s || \
	 clusterer_api.shtag_get(&ul_ha_shtag, ul_ha_cluster) \
		== SHTAG_STATE_ACTIVE)

/* duplicate local events to other OpenSIPS instances */
void replicate_urecord_insert(urecord_t *r);
void replicate_urecord_delete(urecord_t *r);
void replicate_ucontact_insert(urecord_t *r, str *contact, ucontact_t *c,
        const struct ct_match *match);
void replicate_ucontact_update(urecord_t *r, ucontact_t *ct,
        const struct ct_match *match);
void replicate_ucontact_delete(urecord_t *r, ucontact_t *c,
        const struct ct_match *match);

void receive_binary_packets(bin_packet_t *packet);
void receive_cluster_event(enum clusterer_event ev, int node_id);

/* pull-sharing: process queued orphan adoptions and shared-tag
 * takeovers (runs off the usrloc timer, in a worker with a DB handle) */
void ul_pull_process_failover(void);

#endif /* _USRLOC_CLUSTER_H_ */
