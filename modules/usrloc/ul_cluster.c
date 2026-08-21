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

#include <poll.h>
#include <errno.h>

#include "../../forward.h"
#include "../cachedb_perf/pull_api.h"

#include "ul_cluster.h"
#include "ul_mod.h"
#include "dlist.h"
#include "kv_store.h"
#include "utime.h"

str contact_repl_cap = str_init("usrloc-contact-repl");

struct clusterer_binds clusterer_api;
str ul_shtag_key = str_init("_st");

int ul_my_nid;
str ul_onid_key = str_init("_onid");

/* the async pull API, for owner-hinted (targeted) pulls; optional -
 * without it every pull is simply the broadcast ask */
static pcache_pull_api_t ul_pull_api;
static int ul_pull_api_ok;

/* orphan adoption: node-down events are queued here and processed from
 * the usrloc timer once the settle delay has passed AND the node is
 * still absent from the membership - a flap costs nothing.  Shared-tag
 * activation queues a full ledger import the same way (the callback may
 * fire in a process without a DB handle; the timer job has one). */
#define UL_ADOPT_DELAY   10          /* seconds */
#define UL_ADOPT_PENDING 8
struct ul_adopt_slot {
	int nid;                         /* 0 = free */
	unsigned int due;
};
static struct ul_adopt_slot *adopt_pending;
static int *ha_import_pending;
static gen_lock_t *adopt_lock;

int ul_ct_owner_nid(ucontact_t *c)
{
	int_str_t *v;

	if (!c->kv_storage)
		return 0;
	v = kv_get(c->kv_storage, &ul_onid_key);
	if (!v || v->is_str)
		return 0;
	return v->i;
}

int ul_ct_is_mine(ucontact_t *c)
{
	if (!c->sock)
		return 0;
	if (is_anycast(c->sock))
		return ul_my_nid && ul_ct_owner_nid(c) == ul_my_nid;
	return 1;
}

/* record this node as the contact's owner (local registration events) */
void ul_ct_stamp_owner(ucontact_t *c)
{
	int_str_t v;

	if (!c->kv_storage)
		return;
	v.is_str = 0;
	v.i = ul_my_nid;
	if (!kv_put(c->kv_storage, &ul_onid_key, &v))
		LM_ERR("oom stamping owner id on <%.*s>\n", c->c.len, c->c.s);
}

static void ul_ha_shtag_cb(str *tag, int state, int c_id, void *param)
{
	if (cluster_mode != CM_PULL_SHARING || state != SHTAG_STATE_ACTIVE)
		return;

	if (ha_import_pending)
		*ha_import_pending = 1;
	LM_INFO("shared tag <%.*s> went ACTIVE - a full ledger takeover is "
		"scheduled\n", tag->len, tag->s);
}

int ul_init_cluster(void)
{
	if (location_cluster == 0)
		return 0;

	if (location_cluster < 0) {
		LM_ERR("Invalid 'location_cluster'!  It must be a positive integer!\n");
		return -1;
	}

	if (load_clusterer_api(&clusterer_api) != 0) {
		LM_ERR("failed to load clusterer API\n");
		return -1;
	}

	if (cluster_mode == CM_PULL_SHARING) {
		ul_my_nid = clusterer_api.get_my_id();
		if (ul_my_nid <= 0)
			LM_WARN("no clusterer node id yet - anycast ownership will "
			        "not resolve until the cluster forms\n");

		if (load_pcache_pull_api(&ul_pull_api) == 0)
			ul_pull_api_ok = 1;
		else
			LM_INFO("cachedb_perf pull API not exported - owner hints "
			        "disabled, pulls stay broadcast-only\n");

		adopt_pending = shm_malloc(UL_ADOPT_PENDING
				* sizeof *adopt_pending + sizeof *ha_import_pending);
		adopt_lock = lock_alloc();
		if (!adopt_pending || !adopt_lock || !lock_init(adopt_lock)) {
			LM_ERR("no more shm memory for the adoption queue\n");
			return -1;
		}
		memset(adopt_pending, 0, UL_ADOPT_PENDING * sizeof *adopt_pending
				+ sizeof *ha_import_pending);
		ha_import_pending = (int *)(adopt_pending + UL_ADOPT_PENDING);

		if (ul_ha_cluster && ul_ha_shtag.s
		        && clusterer_api.shtag_register_callback(&ul_ha_shtag,
		                ul_ha_cluster, NULL, ul_ha_shtag_cb) < 0)
			LM_ERR("failed to register the shared-tag callback - "
			       "activation will not trigger a ledger takeover\n");
	}

	/* register handler for processing usrloc packets to the clusterer module */
	if (clusterer_api.register_capability(&contact_repl_cap,
		receive_binary_packets, receive_cluster_event, location_cluster,
		rr_persist == RRP_SYNC_FROM_CLUSTER? 1 : 0,
		(cluster_mode == CM_FEDERATION
		 || cluster_mode == CM_FEDERATION_CACHEDB) ?
			NODE_CMP_EQ_SIP_ADDR : NODE_CMP_ANY) < 0) {
		LM_ERR("cannot register callbacks to clusterer module!\n");
		return -1;
	}

	if (rr_persist == RRP_SYNC_FROM_CLUSTER &&
	    clusterer_api.request_sync(&contact_repl_cap, location_cluster, 0) < 0)
		LM_ERR("Sync request failed\n");

	return 0;
}

/* packet sending */

static inline void bin_push_urecord(bin_packet_t *packet, urecord_t *r)
{
	str st;

	bin_push_str(packet, r->domain);
	bin_push_str(packet, &r->aor);
	bin_push_int(packet, r->label);
	bin_push_int(packet, r->next_clabel);

	st = store_serialize(r->kv_storage);
	bin_push_str(packet, &st);
	store_free_buffer(&st);
}

void replicate_urecord_insert(urecord_t *r)
{
	int rc;
	bin_packet_t packet;

	if (bin_init(&packet, &contact_repl_cap, REPL_URECORD_INSERT,
	             UL_BIN_VERSION, 1024) != 0) {
		LM_ERR("failed to replicate this event\n");
		return;
	}

	bin_push_urecord(&packet, r);

	if (cluster_mode == CM_FEDERATION_CACHEDB)
		rc = clusterer_api.send_all_having(&packet, location_cluster,
		                                   NODE_CMP_EQ_SIP_ADDR);
	else
		rc = clusterer_api.send_all(&packet, location_cluster);
	switch (rc) {
	case CLUSTERER_CURR_DISABLED:
		LM_INFO("Current node is disabled in cluster: %d\n", location_cluster);
		goto error;
	case CLUSTERER_DEST_DOWN:
		LM_INFO("All destinations in cluster: %d are down or probing\n",
			location_cluster);
		goto error;
	case CLUSTERER_SEND_ERR:
		LM_ERR("Error sending in cluster: %d\n", location_cluster);
		goto error;
	}

	bin_free_packet(&packet);
	return;

error:
	LM_ERR("replicate urecord insert failed\n");
	bin_free_packet(&packet);
}

void replicate_urecord_delete(urecord_t *r)
{
	int rc;
	bin_packet_t packet;

	if (bin_init(&packet, &contact_repl_cap, REPL_URECORD_DELETE,
	             UL_BIN_VERSION, 1024) != 0) {
		LM_ERR("failed to replicate this event\n");
		return;
	}

	bin_push_str(&packet, r->domain);
	bin_push_str(&packet, &r->aor);

	if (cluster_mode == CM_FEDERATION_CACHEDB)
		rc = clusterer_api.send_all_having(&packet, location_cluster,
		                                   NODE_CMP_EQ_SIP_ADDR);
	else
		rc = clusterer_api.send_all(&packet, location_cluster);
	switch (rc) {
	case CLUSTERER_CURR_DISABLED:
		LM_INFO("Current node is disabled in cluster: %d\n", location_cluster);
		goto error;
	case CLUSTERER_DEST_DOWN:
		LM_INFO("All destinations in cluster: %d are down or probing\n",
			location_cluster);
		goto error;
	case CLUSTERER_SEND_ERR:
		LM_ERR("Error sending in cluster: %d\n", location_cluster);
		goto error;
	}

	bin_free_packet(&packet);
	return;

error:
	LM_ERR("replicate urecord delete failed\n");
	bin_free_packet(&packet);
}

void bin_push_ctmatch(bin_packet_t *packet, const struct ct_match *match)
{
	str_list *param;
	int np = 0;

	bin_push_int(packet, match->mode);
	if (match->mode != CT_MATCH_PARAMS)
		return;

	for (param = match->match_params; param; param = param->next, np++) {}

	bin_push_int(packet, np);
	for (param = match->match_params; param; param = param->next)
		bin_push_str(packet, &param->s);
}

/* NOTICE: remember to free @match->match_params when done with it! */
void bin_pop_ctmatch(bin_packet_t *packet, struct ct_match *match)
{
	int np;

	memset(match, 0, sizeof *match);

	bin_pop_int(packet, &match->mode);
	if (match->mode != CT_MATCH_PARAMS)
		return;

	bin_pop_int(packet, &np);

	for (; np > 0; np--) {
		str_list *param = pkg_malloc(sizeof *param);
		if (!param) {
			LM_ERR("oom\n");
			free_pkg_str_list(match->match_params);
			*match = (struct ct_match){CT_MATCH_CONTACT_CALLID, NULL};
			return;
		}
		memset(param, 0, sizeof *param);

		bin_pop_str(packet, &param->s);
		add_last(param, match->match_params);
	}
}

void bin_push_contact(bin_packet_t *packet, urecord_t *r, ucontact_t *c,
        const struct ct_match *match)
{
	str st;

	bin_push_str(packet, r->domain);
	bin_push_str(packet, &r->aor);
	bin_push_str(packet, &c->c);

	st.s = (char *)&c->contact_id;
	st.len = sizeof c->contact_id;
	bin_push_str(packet, &st);

	bin_push_str(packet, &c->callid);
	bin_push_str(packet, &c->user_agent);
	bin_push_str(packet, &c->path);
	bin_push_str(packet, &c->attr);
	bin_push_str(packet, &c->received);
	bin_push_str(packet, &c->instance);

	st.s = (char *) &c->expires;
	st.len = sizeof c->expires;
	bin_push_str(packet, &st);

	st.s = (char *) &c->q;
	st.len = sizeof c->q;
	bin_push_str(packet, &st);

	bin_push_str(packet, c->sock?get_socket_internal_name(c->sock):NULL);
	bin_push_int(packet, c->cseq);
	bin_push_int(packet, c->flags);
	st = bitmask_to_flag_list(FLAG_TYPE_BRANCH, c->cflags);
	bin_push_str(packet, &st);
	bin_push_int(packet, c->methods);

	st.s   = (char *)&c->last_modified;
	st.len = sizeof c->last_modified;
	bin_push_str(packet, &st);

	st = store_serialize(c->kv_storage);
	bin_push_str(packet, &st);
	store_free_buffer(&st);

	bin_push_ctmatch(packet, match);
}

void replicate_ucontact_insert(urecord_t *r, str *contact, ucontact_t *c,
        const struct ct_match *match)
{
	int rc;
	bin_packet_t packet;

	if (bin_init(&packet, &contact_repl_cap, REPL_UCONTACT_INSERT,
	             UL_BIN_VERSION, 0) != 0) {
		LM_ERR("failed to replicate this event\n");
		return;
	}

	bin_push_contact(&packet, r, c, match);

	if (cluster_mode == CM_FEDERATION_CACHEDB)
		rc = clusterer_api.send_all_having(&packet, location_cluster,
		                                   NODE_CMP_EQ_SIP_ADDR);
	else
		rc = clusterer_api.send_all(&packet, location_cluster);
	switch (rc) {
	case CLUSTERER_CURR_DISABLED:
		LM_INFO("Current node is disabled in cluster: %d\n", location_cluster);
		goto error;
	case CLUSTERER_DEST_DOWN:
		LM_INFO("All destinations in cluster: %d are down or probing\n",
			location_cluster);
		goto error;
	case CLUSTERER_SEND_ERR:
		LM_ERR("Error sending in cluster: %d\n", location_cluster);
		goto error;
	}

	bin_free_packet(&packet);
	return;

error:
	LM_ERR("replicate ucontact insert failed\n");
	bin_free_packet(&packet);
}

void replicate_ucontact_update(urecord_t *r, ucontact_t *ct,
        const struct ct_match *match)
{
	str st;
	int rc;
	bin_packet_t packet;

	if (bin_init(&packet, &contact_repl_cap, REPL_UCONTACT_UPDATE,
	             UL_BIN_VERSION, 0) != 0) {
		LM_ERR("failed to replicate this event\n");
		return;
	}

	bin_push_str(&packet, r->domain);
	bin_push_str(&packet, &r->aor);
	bin_push_str(&packet, &ct->c);
	bin_push_str(&packet, &ct->callid);
	bin_push_str(&packet, &ct->user_agent);
	bin_push_str(&packet, &ct->path);
	bin_push_str(&packet, &ct->attr);
	bin_push_str(&packet, &ct->received);
	bin_push_str(&packet, &ct->instance);

	st.s = (char *) &ct->expires;
	st.len = sizeof ct->expires;
	bin_push_str(&packet, &st);

	st.s = (char *) &ct->q;
	st.len = sizeof ct->q;
	bin_push_str(&packet, &st);

	bin_push_str(&packet, ct->sock?get_socket_internal_name(ct->sock):NULL);
	bin_push_int(&packet, ct->cseq);
	bin_push_int(&packet, ct->flags);
	st = bitmask_to_flag_list(FLAG_TYPE_BRANCH, ct->cflags);
	bin_push_str(&packet, &st);
	bin_push_int(&packet, ct->methods);

	st.s   = (char *)&ct->last_modified;
	st.len = sizeof ct->last_modified;
	bin_push_str(&packet, &st);

	st = store_serialize(ct->kv_storage);
	bin_push_str(&packet, &st);
	store_free_buffer(&st);

	st.s = (char *)&ct->contact_id;
	st.len = sizeof ct->contact_id;
	bin_push_str(&packet, &st);

	bin_push_ctmatch(&packet, match);

	if (cluster_mode == CM_FEDERATION_CACHEDB)
		rc = clusterer_api.send_all_having(&packet, location_cluster,
		                                   NODE_CMP_EQ_SIP_ADDR);
	else
		rc = clusterer_api.send_all(&packet, location_cluster);
	switch (rc) {
	case CLUSTERER_CURR_DISABLED:
		LM_INFO("Current node is disabled in cluster: %d\n", location_cluster);
		goto error;
	case CLUSTERER_DEST_DOWN:
		LM_INFO("All destinations in cluster: %d are down or probing\n",
			location_cluster);
		goto error;
	case CLUSTERER_SEND_ERR:
		LM_ERR("Error sending in cluster: %d\n", location_cluster);
		goto error;
	}

	bin_free_packet(&packet);
	return;

error:
	LM_ERR("replicate ucontact update failed\n");
	bin_free_packet(&packet);
}

void replicate_ucontact_delete(urecord_t *r, ucontact_t *c,
        const struct ct_match *_match)
{
	struct ct_match match;
	int rc;
	bin_packet_t packet;

	if (bin_init(&packet, &contact_repl_cap, REPL_UCONTACT_DELETE,
	             UL_BIN_VERSION, 0) != 0) {
		LM_ERR("failed to replicate this event\n");
		return;
	}

	if (!_match)
		match = (struct ct_match){CT_MATCH_CONTACT_CALLID, NULL};
	else
		match = *_match;

	bin_push_str(&packet, r->domain);
	bin_push_str(&packet, &r->aor);
	bin_push_str(&packet, &c->c);
	bin_push_str(&packet, &c->callid);
	bin_push_int(&packet, c->cseq);
	bin_push_ctmatch(&packet, &match);

	if (cluster_mode == CM_FEDERATION_CACHEDB)
		rc = clusterer_api.send_all_having(&packet, location_cluster,
		                                   NODE_CMP_EQ_SIP_ADDR);
	else
		rc = clusterer_api.send_all(&packet, location_cluster);
	switch (rc) {
	case CLUSTERER_CURR_DISABLED:
		LM_INFO("Current node is disabled in cluster: %d\n", location_cluster);
		goto error;
	case CLUSTERER_DEST_DOWN:
		LM_INFO("All destinations in cluster: %d are down or probing\n",
			location_cluster);
		goto error;
	case CLUSTERER_SEND_ERR:
		LM_ERR("Error sending in cluster: %d\n", location_cluster);
		goto error;
	}

	bin_free_packet(&packet);
	return;

error:
	LM_ERR("replicate ucontact delete failed\n");
	bin_free_packet(&packet);
}

/* pull-sharing: the record blob
 *
 * The value stored under ul$<domain>$<aor> is a self-describing bin
 * packet (capability + type + version header included), so the absorb
 * side can refuse a format it does not speak and treat the pull as a
 * miss - rolling upgrades degrade to not-shared instead of misparsing.
 * Payload: publisher node id, the urecord fields, then a counted list
 * of contacts in the exact bin_push_contact() layout.  EVERY valid
 * contact the node knows is included, owned and pulled alike: blobs
 * converge the way memory does, so any single pull answer tends toward
 * the full contact set of the AoR even when its contacts are owned by
 * different nodes (per-contact ownership rides each contact's "_onid"
 * K/V entry, inside the serialized kv_storage). */

static str ul_blob_key(const str *domain, const str *aor)
{
	static char kbuf[512];
	str k = {kbuf, 0};

	if (3 + domain->len + 1 + aor->len > (int)sizeof(kbuf))
		return k;                              /* .len == 0: unusable */
	memcpy(kbuf, "ul$", 3);
	k.len = 3;
	memcpy(kbuf + k.len, domain->s, domain->len);
	k.len += domain->len;
	kbuf[k.len++] = '$';
	memcpy(kbuf + k.len, aor->s, aor->len);
	k.len += aor->len;
	return k;
}

static int ul_pull_serialize_urecord(urecord_t *r, bin_packet_t *pkt)
{
	struct ct_match cmatch = { CT_MATCH_CONTACT_ONLY, NULL };
	ucontact_t *c;
	int n = 0;

	if (bin_init(pkt, &contact_repl_cap, REPL_UREC_BLOB,
	             UL_BIN_VERSION, 0) != 0) {
		LM_ERR("failed to build the record blob\n");
		return -1;
	}

	bin_push_int(pkt, ul_my_nid);
	bin_push_urecord(pkt, r);

	for (c = r->contacts; c; c = c->next)
		if (VALID_CONTACT(c, act_time))
			n++;
	bin_push_int(pkt, n);

	for (c = r->contacts; c; c = c->next)
		if (VALID_CONTACT(c, act_time))
			bin_push_contact(pkt, r, c, &cmatch);

	return n;
}

void ul_pull_publish(urecord_t *r)
{
	bin_packet_t pkt;
	str key, blob;
	time_t max_exp = 0;
	ucontact_t *c;
	int n;

	if (cluster_mode != CM_PULL_SHARING || !cdbc)
		return;

	key = ul_blob_key(r->domain, &r->aor);
	if (!key.len) {
		LM_ERR("AoR too long to share: <%.*s>\n", r->aor.len, r->aor.s);
		return;
	}

	get_act_time();
	n = ul_pull_serialize_urecord(r, &pkt);
	if (n < 0)
		return;
	if (n == 0) {
		bin_free_packet(&pkt);
		if (cdbf.remove(cdbc, &key) < 0)
			LM_ERR("failed to withdraw <%.*s>\n", key.len, key.s);
		return;
	}

	for (c = r->contacts; c; c = c->next)
		if (VALID_CONTACT(c, act_time) && c->expires > max_exp)
			max_exp = c->expires;

	bin_get_buffer(&pkt, &blob);
	/* +30 s so a refresh landing at the expiry edge still overwrites an
	 * existing blob rather than racing a hole */
	if (cdbf.set(cdbc, &key, &blob, (int)(max_exp - act_time) + 30) < 0)
		LM_ERR("failed to publish <%.*s>\n", key.len, key.s);
	bin_free_packet(&pkt);
}

void ul_pull_unpublish(urecord_t *r)
{
	str key;

	if (cluster_mode != CM_PULL_SHARING || !cdbc)
		return;

	key = ul_blob_key(r->domain, &r->aor);
	if (!key.len)
		return;
	if (cdbf.remove(cdbc, &key) < 0)
		LM_ERR("failed to withdraw <%.*s>\n", key.len, key.s);
}

/* The miss half of get_urecord_or_pull(): ask the shared cache, whose
 * vtable get() hides whether the answer was local state or a completed
 * cluster pull (CP-15), and absorb a hit into local memory.  The caller
 * holds the domain lock for @aor - which also means the bounded pull
 * wait (pull_timeout_ms) runs under that slot lock; only genuine remote
 * misses pay it, and those are negative-cached. */
int ul_pull_fetch(struct udomain *domain, str *aor, urecord_t **r,
		int hint_nid)
{
	str key, val = STR_NULL;
	int rc;

	*r = NULL;
	if (!cdbc)
		return ul_pull_ledger_fetch(domain, aor, r);

	key = ul_blob_key(domain->name, aor);
	if (!key.len)
		return 1;

	/* a fresh owner hint turns the broadcast ask into a targeted one -
	 * typically when re-fetching a record whose local copy just expired:
	 * if it lives on anywhere, it lives on (extended) at its owner */
	if (hint_nid > 0 && hint_nid != ul_my_nid && ul_pull_api_ok) {
		unsigned int handle;
		int fd;

		rc = ul_pull_api.start_at(cdbc, &key, hint_nid, &fd, &handle);
		if (rc == 1) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			int left = 100;                     /* ms */

			while (left > 0) {
				int n = poll(&pfd, 1, left);

				if (n > 0)
					break;
				if (n < 0 && errno == EINTR) {
					left -= 1;
					continue;
				}
				break;
			}

			rc = ul_pull_api.finish(cdbc, &key, handle, &val);
			if (rc == 1) {
				rc = ul_pull_absorb_blob(domain, aor, &val);
				pkg_free(val.s);
				if (rc > 0)
					return get_urecord(domain, aor, r);
			}
			/* the hinted node had nothing (or nothing usable) - a lone
			 * negative is not the cluster's answer, so ask everyone */
		} else if (rc == 0) {
			/* a cached negative IS the cluster's answer */
			return ul_pull_ledger_fetch(domain, aor, r);
		}
	}

	rc = cdbf.get(cdbc, &key, &val);
	if (rc < 0)
		/* no peer holds it (or none answered) - the shared ledger is
		 * the last resort, and the one that survives an owner crash */
		return ul_pull_ledger_fetch(domain, aor, r);

	rc = ul_pull_absorb_blob(domain, aor, &val);
	pkg_free(val.s);
	if (rc <= 0)
		return ul_pull_ledger_fetch(domain, aor, r);

	return get_urecord(domain, aor, r);
}

/* Async pull, for a caller that suspends its transaction instead of
 * occupying a worker: begin the cluster ask for @aor.
 * 1 = started (@fd becomes readable when settled, @handle for finish),
 * 0 = a cached negative IS the cluster's answer, -1 = cannot ask
 * (no API, no peers, no slot - the blocking path is the fallback). */
int ul_pull_start(struct udomain *domain, str *aor, int *fd,
		unsigned int *handle)
{
	str key;
	urecord_t *r;
	ucontact_t *c;
	time_t newest = 0;
	int hint = 0;

	if (cluster_mode != CM_PULL_SHARING || !ul_pull_api_ok || !cdbc)
		return -1;

	key = ul_blob_key(domain->name, aor);
	if (!key.len)
		return -1;

	/* a husk's freshest copy names the owner - ask it directly */
	lock_udomain(domain, aor);
	if (get_urecord(domain, aor, &r) == 0)
		for (c = r->contacts; c; c = c->next)
			if (c->expires > newest) {
				newest = c->expires;
				hint = ul_ct_owner_nid(c);
			}
	unlock_udomain(domain, aor);
	if (hint == ul_my_nid)
		hint = 0;

	return ul_pull_api.start_at(cdbc, &key, hint, fd, handle);
}

/* Async pull: collect the answer and absorb it.  Takes its own lock.
 * 1 = absorbed, 0 = definitively absent, -1 = no answer / not usable
 * (a follow-up blocking lookup still gets the ledger fallback). */
int ul_pull_finish(struct udomain *domain, str *aor, unsigned int handle)
{
	str key, val = STR_NULL;
	int rc;

	if (!cdbc)
		return -1;

	key = ul_blob_key(domain->name, aor);
	if (!key.len)
		return -1;

	rc = ul_pull_api.finish(cdbc, &key, handle, &val);
	if (rc != 1)
		return rc;

	lock_udomain(domain, aor);
	rc = ul_pull_absorb_blob(domain, aor, &val) > 0 ? 1 : -1;
	unlock_udomain(domain, aor);
	pkg_free(val.s);

	return rc;
}

/* Restart bootstrap: after the ledger load, publish the blobs of every
 * record this node owns at least one contact of.  Only owners publish -
 * a foreign row is already in our memory for local lookups, and pulls
 * for it should resolve from its owner's fresher store. */
void ul_pull_publish_all_owned(void)
{
	dlist_t *dl;
	udomain_t *dom;
	map_iterator_t it;
	urecord_t *r;
	ucontact_t *c;
	void **p;
	int i, mine, n = 0;

	if (cluster_mode != CM_PULL_SHARING || !cdbc)
		return;

	for (dl = root; dl; dl = dl->next) {
		dom = dl->d;
		for (i = 0; i < dom->size; i++) {
			lock_ulslot(dom, i);
			for (map_first(dom->table[i].records, &it);
			        iterator_is_valid(&it); iterator_next(&it)) {
				p = iterator_val(&it);
				if (!p)
					continue;
				r = (urecord_t *)*p;

				mine = 0;
				for (c = r->contacts; c; c = c->next)
					if (ul_ct_is_mine(c)) {
						mine = 1;
						break;
					}
				if (mine) {
					ul_pull_publish(r);
					n++;
				}
			}
			unlock_ulslot(dom, i);
		}
	}

	LM_INFO("re-published %d owned records into the shared cache\n", n);
}

/* Merge one pulled blob into local memory; the caller holds the domain
 * lock for @aor.  Contacts run through the same get->update-or-insert
 * machinery the live replication uses, so per-contact callid/cseq rules
 * settle every conflict - including a blob that echoes contacts this
 * node owns: an equal cseq is a no-op, and genuinely newer state means
 * a takeover we missed, which FL_PULLED then correctly reflects. */
int ul_pull_absorb_blob(struct udomain *domain, str *aor, str *blob)
{
	static ucontact_info_t ci;
	static str d, r_aor, contact_str, callid,
		user_agent, path, attr, st, sock, kv_str, cflags_str;
	bin_packet_t pkt;
	urecord_t *record;
	ucontact_t *contact;
	map_t kv_storage;
	struct ct_match cmatch;
	unsigned int rlabel;
	unsigned short _, clabel;
	int rc, sl, n, tmp, publisher, absorbed = 0, fresh = 0;
	short pkg_ver;

	if (!blob->s || blob->len < 32) {
		LM_ERR("pulled a runt blob (%d bytes) for <%.*s>\n",
		       blob->len, aor->len, aor->s);
		return -1;
	}

	bin_init_buffer(&pkt, blob->s, blob->len);
	if (pkt.type != REPL_UREC_BLOB) {
		LM_ERR("pulled value for <%.*s> is not a record blob (type %d)\n",
		       aor->len, aor->s, pkt.type);
		return -1;
	}
	pkg_ver = get_bin_pkg_version(&pkt);
	if (pkg_ver != UL_BIN_VERSION) {
		LM_INFO("record blob format %d differs from ours (%d) - treating "
			"the pull as a miss until the fleet converges\n",
			pkg_ver, UL_BIN_VERSION);
		return -1;
	}

	bin_pop_int(&pkt, &publisher);
	bin_pop_str(&pkt, &d);
	bin_pop_str(&pkt, &r_aor);
	if (!str_match(&r_aor, aor) || !str_match(&d, domain->name)) {
		LM_ERR("blob echoes <%.*s|%.*s>, expected <%.*s|%.*s> - "
		       "refusing\n", d.len, d.s, r_aor.len, r_aor.s,
		       domain->name->len, domain->name->s, aor->len, aor->s);
		return -1;
	}

	if (get_urecord(domain, aor, &record) != 0) {
		if (insert_urecord(domain, aor, &record, 1, NULL, NULL) != 0) {
			LM_ERR("failed to create the record for <%.*s>\n",
			       aor->len, aor->s);
			return -1;
		}
		fresh = 1;
	}

	bin_pop_int(&pkt, &tmp);
	if (fresh) {
		record->label = tmp;
		sl = record->aorhash & (domain->size - 1);
		if (domain->table[sl].next_label <= record->label)
			domain->table[sl].next_label = record->label + 1;
	}
	bin_pop_int(&pkt, &tmp);
	if (fresh)
		record->next_clabel = tmp;

	bin_pop_str(&pkt, &kv_str);
	if (fresh && kv_str.len) {
		kv_storage = store_deserialize(&kv_str);
		if (kv_storage) {
			store_destroy(record->kv_storage);
			record->kv_storage = kv_storage;
		}
	}

	bin_pop_int(&pkt, &n);
	get_act_time();

	for (; n > 0; n--) {
		memset(&ci, 0, sizeof ci);
		cmatch = (struct ct_match){CT_MATCH_NONE, NULL};

		bin_pop_str(&pkt, &d);              /* per-contact domain copy */
		bin_pop_str(&pkt, &r_aor);          /* per-contact aor copy */
		bin_pop_str(&pkt, &contact_str);

		bin_pop_str(&pkt, &st);
		memcpy(&ci.contact_id, st.s, sizeof ci.contact_id);

		bin_pop_str(&pkt, &callid);
		ci.callid = &callid;
		bin_pop_str(&pkt, &user_agent);
		ci.user_agent = &user_agent;
		bin_pop_str(&pkt, &path);
		ci.path = &path;
		bin_pop_str(&pkt, &attr);
		ci.attr = &attr;
		bin_pop_str(&pkt, &ci.received);
		bin_pop_str(&pkt, &ci.instance);

		bin_pop_str(&pkt, &st);
		memcpy(&ci.expires, st.s, sizeof ci.expires);
		bin_pop_str(&pkt, &st);
		memcpy(&ci.q, st.s, sizeof ci.q);

		bin_pop_str(&pkt, &sock);
		if (sock.s && sock.s[0]) {
			ci.sock = parse_sock_info(&sock);
			if (!ci.sock)
				LM_DBG("non-local socket <%.*s>\n", sock.len, sock.s);
		} else {
			ci.sock = NULL;
		}

		bin_pop_int(&pkt, &ci.cseq);
		bin_pop_int(&pkt, &ci.flags);
		bin_pop_str(&pkt, &cflags_str);
		ci.cflags = flag_list_to_bitmask(
		        (str_const *)&cflags_str, FLAG_TYPE_BRANCH, FLAG_DELIM, 0);
		bin_pop_int(&pkt, &ci.methods);

		bin_pop_str(&pkt, &st);
		memcpy(&ci.last_modified, st.s, sizeof ci.last_modified);

		bin_pop_str(&pkt, &kv_str);
		ci.packed_kv_storage = &kv_str;

		bin_pop_ctmatch(&pkt, &cmatch);

		/* a convergence copy: never pinged, never written to any store */
		ci.flags |= FL_PULLED | FL_MEM;

		if (ci.expires <= act_time)
			goto next_contact;              /* already dead in flight */

		unpack_indexes(ci.contact_id, &_, &rlabel, &clabel);

		rc = get_ucontact(record, &contact_str, &callid, ci.cseq, &cmatch,
			&contact);
		switch (rc) {
		case -2:
		case -1:
			/* what we hold is as new or newer */
			break;

		case 0:
			ci.contact_id = pack_indexes((unsigned short)record->aorhash,
						record->label, (unsigned short)contact->label);

			if (update_ucontact(record, contact, &ci, NULL, 1) != 0)
				LM_ERR("failed to absorb update of <%.*s>\n",
				       contact_str.len, contact_str.s);
			else
				absorbed++;
			break;

		case 1:
			if (clabel >= record->next_clabel) {
				record->next_clabel = CLABEL_NEXT(clabel);
			} else {
				clabel = record->next_clabel;
				record->next_clabel = CLABEL_NEXT(record->next_clabel);
			}

			ci.contact_id = pack_indexes((unsigned short)record->aorhash,
						record->label, (unsigned short)clabel);

			if (insert_ucontact(record, &contact_str, &ci, NULL, 1,
			        &contact) != 0)
				LM_ERR("failed to absorb <%.*s>\n",
				       contact_str.len, contact_str.s);
			else
				absorbed++;
			break;
		}

next_contact:
		free_pkg_str_list(cmatch.match_params);
	}

	return absorbed;
}

/* packet receiving */

/**
 * Note: prevents the creation of any duplicate AoR
 */
static int receive_urecord_insert(bin_packet_t *packet)
{
	str d, aor, kv_str;
	urecord_t *r;
	map_t kv_storage;
	udomain_t *domain;
	int sl;
	short pkg_ver = get_bin_pkg_version(packet);

	bin_pop_str(packet, &d);
	bin_pop_str(packet, &aor);
	if (aor.len == 0) {
		LM_ERR("the AoR URI is missing the 'username' part!\n");
		goto out_err;
	}

	if (find_domain(&d, &domain) != 0) {
		LM_ERR("domain '%.*s' is not local\n", d.len, d.s);
		goto out_err;
	}

	lock_udomain(domain, &aor);

	if (get_urecord(domain, &aor, &r) == 0)
		goto out;

	if (insert_urecord(domain, &aor, &r, 1, NULL, NULL) != 0) {
		unlock_udomain(domain, &aor);
		goto out_err;
	}

	bin_pop_int(packet, &r->label);
	bin_pop_int(packet, &r->next_clabel);

	sl = r->aorhash & (domain->size - 1);
	if (domain->table[sl].next_label <= r->label)
		domain->table[sl].next_label = r->label + 1;

	if (pkg_ver >= UL_BIN_V5) {
		bin_pop_str(packet, &kv_str);
		kv_storage = store_deserialize(&kv_str);
		if (kv_storage) {
			store_destroy(r->kv_storage);
			r->kv_storage = kv_storage;
		}
	}

out:
	unlock_udomain(domain, &aor);

	return 0;

out_err:
	LM_ERR("failed to replicate event locally. dom: '%.*s', aor: '%.*s'\n",
		d.len, d.s, aor.len, aor.s);
	return -1;
}

static int receive_urecord_delete(bin_packet_t *packet)
{
	str d, aor;
	udomain_t *domain;

	bin_pop_str(packet, &d);
	bin_pop_str(packet, &aor);
	if (aor.len == 0) {
		LM_ERR("the AoR URI is missing the 'username' part!\n");
		goto out_err;
	}

	if (find_domain(&d, &domain) != 0) {
		LM_ERR("domain '%.*s' is not local\n", d.len, d.s);
		goto out_err;
	}

	/* pull-sharing: the sender can only speak for the contacts it saw -
	 * drop our convergence copies, but never contacts we own (a binding
	 * accepted here after the sender's snapshot outlives its wipe) */
	if (cluster_mode == CM_PULL_SHARING) {
		urecord_t *r;
		ucontact_t *c, *t;

		lock_udomain(domain, &aor);
		if (get_urecord(domain, &aor, &r) != 0) {
			unlock_udomain(domain, &aor);
			return 0;
		}
		c = r->contacts;
		while (c) {
			t = c;
			c = c->next;
			if (t->flags & FL_PULLED)
				delete_ucontact(r, t, NULL, 1);
		}
		ul_pull_unpublish(r);
		if (!r->contacts)
			release_urecord(r, 1);
		unlock_udomain(domain, &aor);
		return 0;
	}

	lock_udomain(domain, &aor);

	if (delete_urecord(domain, &aor, NULL, 1) != 0) {
		unlock_udomain(domain, &aor);
		goto out_err;
	}

	unlock_udomain(domain, &aor);

	return 0;

out_err:
	LM_ERR("failed to process replication event. dom: '%.*s', aor: '%.*s'\n",
		d.len, d.s, aor.len, aor.s);
	return -1;
}

static int receive_ucontact_insert(bin_packet_t *packet)
{
	static ucontact_info_t ci;
	static str d, aor, contact_str, callid,
		user_agent, path, attr, st, sock, kv_str, cflags_str;
	udomain_t *domain;
	urecord_t *record;
	ucontact_t *contact;
	int rc, sl;
	unsigned short _, clabel;
	unsigned int rlabel;
	struct ct_match cmatch = {CT_MATCH_NONE, NULL};
	short pkg_ver = get_bin_pkg_version(packet);

	memset(&ci, 0, sizeof ci);

	bin_pop_str(packet, &d);
	bin_pop_str(packet, &aor);
	if (aor.len == 0) {
		LM_ERR("the AoR URI is missing the 'username' part!\n");
		goto error;
	}

	if (find_domain(&d, &domain) != 0) {
		LM_ERR("domain '%.*s' is not local\n", d.len, d.s);
		goto error;
	}

	bin_pop_str(packet, &contact_str);

	bin_pop_str(packet, &st);
	memcpy(&ci.contact_id, st.s, sizeof ci.contact_id);

	bin_pop_str(packet, &callid);
	ci.callid = &callid;

	bin_pop_str(packet, &user_agent);
	ci.user_agent = &user_agent;

	bin_pop_str(packet, &path);
	ci.path = &path;

	bin_pop_str(packet, &attr);
	ci.attr = &attr;

	bin_pop_str(packet, &ci.received);
	bin_pop_str(packet, &ci.instance);

	bin_pop_str(packet, &st);
	memcpy(&ci.expires, st.s, sizeof ci.expires);

	bin_pop_str(packet, &st);
	memcpy(&ci.q, st.s, sizeof ci.q);

	bin_pop_str(packet, &sock);

	if (sock.s && sock.s[0]) {
		ci.sock = parse_sock_info(&sock);
		if (!ci.sock)
			LM_DBG("non-local socket <%.*s>\n", sock.len, sock.s);
	} else {
		ci.sock = NULL;
	}

	bin_pop_int(packet, &ci.cseq);
	bin_pop_int(packet, &ci.flags);
	if (pkg_ver <= UL_BIN_V3) {
		bin_pop_int(packet, &ci.cflags);
	} else {
		bin_pop_str(packet, &cflags_str);
		ci.cflags = flag_list_to_bitmask(
		        (str_const *)&cflags_str, FLAG_TYPE_BRANCH, FLAG_DELIM, 0);
	}
	bin_pop_int(packet, &ci.methods);

	bin_pop_str(packet, &st);
	memcpy(&ci.last_modified, st.s, sizeof ci.last_modified);

	bin_pop_str(packet, &kv_str);
	ci.packed_kv_storage = &kv_str;

	if (pkg_ver <= UL_BIN_V2)
		cmatch = (struct ct_match){CT_MATCH_CONTACT_CALLID, NULL};
	else
		bin_pop_ctmatch(packet, &cmatch);

	if (skip_replicated_db_ops)
		ci.flags |= FL_MEM;

	unpack_indexes(ci.contact_id, &_, &rlabel, &clabel);

	/* pull-sharing receives these only as invalidations: apply onto what
	 * exists (as a convergence copy), never materialize what does not */
	if (cluster_mode == CM_PULL_SHARING)
		ci.flags |= FL_PULLED | FL_MEM;

	lock_udomain(domain, &aor);

	if (get_urecord(domain, &aor, &record) != 0) {
		if (cluster_mode == CM_PULL_SHARING) {
			unlock_udomain(domain, &aor);
			goto out_free;
		}

		LM_INFO("failed to fetch local urecord - creating new one "
			"(ci: '%.*s') \n", callid.len, callid.s);

		if (insert_urecord(domain, &aor, &record, 1, NULL, NULL) != 0) {
			LM_ERR("failed to insert new record\n");
			unlock_udomain(domain, &aor);
			goto error;
		}

		record->label = rlabel;
		sl = record->aorhash & (domain->size - 1);
		if (rlabel >= domain->table[sl].next_label)
			domain->table[sl].next_label = rlabel + 1;
	}

	rc = get_ucontact(record, &contact_str, &callid, ci.cseq, &cmatch,
		&contact);

	switch (rc) {
	case -2:
		/* received data is consistent with what we have */
	case -1:
		/* received data is older than what we have */
		break;

	case 0:
		ci.contact_id = pack_indexes((unsigned short)record->aorhash,
					record->label, (unsigned short)contact->label);

		/* received data is newer than what we have */
		if (update_ucontact(record, contact, &ci, NULL, 1) != 0) {
			LM_ERR("failed to update ucontact (ci: '%.*s')\n", callid.len, callid.s);
			unlock_udomain(domain, &aor);
			goto error;
		}
		/* our cached blob (if any) just went stale - drop it, the next
		 * pull re-fetches from the contact's new owner */
		if (cluster_mode == CM_PULL_SHARING)
			ul_pull_unpublish(record);
		break;

	case 1:
		if (cluster_mode == CM_PULL_SHARING)
			break;
		if (clabel >= record->next_clabel) {
			record->next_clabel = CLABEL_NEXT(clabel);
		} else {
			clabel = record->next_clabel;
			record->next_clabel = CLABEL_NEXT(record->next_clabel);
		}

		ci.contact_id = pack_indexes((unsigned short)record->aorhash,
					record->label, (unsigned short)clabel);

		if (insert_ucontact(record, &contact_str, &ci, NULL, 1, &contact) != 0) {
			LM_ERR("failed to insert ucontact (ci: '%.*s')\n", callid.len, callid.s);
			unlock_udomain(domain, &aor);
			goto error;
		}
		break;
	}

	unlock_udomain(domain, &aor);

out_free:
	free_pkg_str_list(cmatch.match_params);
	return 0;

error:
	free_pkg_str_list(cmatch.match_params);
	LM_ERR("failed to process replication event. dom: '%.*s', aor: '%.*s'\n",
		d.len, d.s, aor.len, aor.s);
	return -1;
}

static int receive_ucontact_update(bin_packet_t *packet)
{
	static ucontact_info_t ci;
	static str d, aor, contact_str, callid,
		user_agent, path, attr, st, kv_str, sock, cflags_str;
	udomain_t *domain;
	urecord_t *record;
	ucontact_t *contact;
	int rc, sl;
	unsigned short _, clabel;
	unsigned int rlabel;
	struct ct_match cmatch = {CT_MATCH_NONE, NULL};
	short pkg_ver = get_bin_pkg_version(packet);

	memset(&ci, 0, sizeof ci);

	bin_pop_str(packet, &d);
	bin_pop_str(packet, &aor);
	if (aor.len == 0) {
		LM_ERR("the AoR URI is missing the 'username' part!\n");
		goto error;
	}

	if (find_domain(&d, &domain) != 0) {
		LM_ERR("domain '%.*s' is not local\n", d.len, d.s);
		goto error;
	}

	bin_pop_str(packet, &contact_str);

	bin_pop_str(packet, &callid);
	ci.callid = &callid;

	bin_pop_str(packet, &user_agent);
	ci.user_agent = &user_agent;

	bin_pop_str(packet, &path);
	ci.path = &path;

	bin_pop_str(packet, &attr);
	ci.attr = &attr;

	bin_pop_str(packet, &ci.received);
	bin_pop_str(packet, &ci.instance);

	bin_pop_str(packet, &st);
	memcpy(&ci.expires, st.s, sizeof ci.expires);

	bin_pop_str(packet, &st);
	memcpy(&ci.q, st.s, sizeof ci.q);

	bin_pop_str(packet, &sock);

	if (sock.s && sock.s[0]) {
		ci.sock = parse_sock_info(&sock);
		if (!ci.sock)
			LM_DBG("non-local socket <%.*s>\n", sock.len, sock.s);
	} else {
		ci.sock = NULL;
	}

	bin_pop_int(packet, &ci.cseq);
	bin_pop_int(packet, &ci.flags);
	if (pkg_ver <= UL_BIN_V3) {
		bin_pop_int(packet, &ci.cflags);
	} else {
		bin_pop_str(packet, &cflags_str);
		ci.cflags = flag_list_to_bitmask(
		        (str_const *)&cflags_str, FLAG_TYPE_BRANCH, FLAG_DELIM, 0);
	}
	bin_pop_int(packet, &ci.methods);

	bin_pop_str(packet, &st);
	memcpy(&ci.last_modified, st.s, sizeof ci.last_modified);

	bin_pop_str(packet, &kv_str);
	ci.packed_kv_storage = &kv_str;

	if (skip_replicated_db_ops)
		ci.flags |= FL_MEM;

	bin_pop_str(packet, &st);
	memcpy(&ci.contact_id, st.s, sizeof ci.contact_id);

	unpack_indexes(ci.contact_id, &_, &rlabel, &clabel);

	if (pkg_ver <= UL_BIN_V2)
		cmatch = (struct ct_match){CT_MATCH_CONTACT_CALLID, NULL};
	else
		bin_pop_ctmatch(packet, &cmatch);

	/* pull-sharing: this is the moved-binding invalidation - the sender
	 * took over the contact, so an existing copy here is re-pointed and
	 * demoted to a convergence copy; what we do not hold we ignore */
	if (cluster_mode == CM_PULL_SHARING)
		ci.flags |= FL_PULLED | FL_MEM;

	lock_udomain(domain, &aor);

	/* failure in retrieving a urecord may be ok, because packet order in UDP
	 * is not guaranteed, so update commands may arrive before inserts */
	if (get_urecord(domain, &aor, &record) != 0) {
		if (cluster_mode == CM_PULL_SHARING) {
			unlock_udomain(domain, &aor);
			goto out_free;
		}

		LM_INFO("failed to fetch local urecord - create new record and contact"
			" (ci: '%.*s')\n", callid.len, callid.s);

		if (insert_urecord(domain, &aor, &record, 1, NULL, NULL) != 0) {
			LM_ERR("failed to insert urecord\n");
			unlock_udomain(domain, &aor);
			goto error;
		}

		record->label = rlabel;
		sl = record->aorhash & (domain->size - 1);
		if (domain->table[sl].next_label <= record->label)
			domain->table[sl].next_label = record->label + 1;

		if (insert_ucontact(record, &contact_str, &ci, NULL, 1, &contact) != 0) {
			LM_ERR("failed (ci: '%.*s')\n", callid.len, callid.s);
			unlock_udomain(domain, &aor);
			goto error;
		}

		if (record->next_clabel <= clabel)
			record->next_clabel = CLABEL_NEXT(clabel);
	} else {
		rc = get_ucontact(record, &contact_str, &callid, ci.cseq + 1, &cmatch,
			&contact);
		if (rc == 1) {
			if (cluster_mode == CM_PULL_SHARING)
				goto out_unlock;

			LM_INFO("contact '%.*s' not found, inserting new (ci: '%.*s')\n",
				contact_str.len, contact_str.s, callid.len, callid.s);

			if (insert_ucontact(record, &contact_str, &ci, NULL, 1, &contact) != 0) {
				LM_ERR("failed to insert ucontact (ci: '%.*s')\n",
					callid.len, callid.s);
				unlock_udomain(domain, &aor);
				goto error;
			}

			if (record->next_clabel <= clabel)
				record->next_clabel = CLABEL_NEXT(clabel);

		} else if (rc == 0) {
			if (update_ucontact(record, contact, &ci, NULL, 1) != 0) {
				LM_ERR("failed to update ucontact '%.*s' (ci: '%.*s')\n",
					contact_str.len, contact_str.s, callid.len, callid.s);
				unlock_udomain(domain, &aor);
				goto error;
			}
			if (cluster_mode == CM_PULL_SHARING)
				ul_pull_unpublish(record);
		} /* XXX: for -2 and -1, the master should have already handled
			 these errors - so we can skip them - razvanc */
	}

out_unlock:
	unlock_udomain(domain, &aor);

out_free:
	free_pkg_str_list(cmatch.match_params);
	return 0;

error:
	free_pkg_str_list(cmatch.match_params);
	LM_ERR("failed to process replication event. dom: '%.*s', aor: '%.*s'\n",
		d.len, d.s, aor.len, aor.s);
	return -1;
}

static int receive_ucontact_delete(bin_packet_t *packet)
{
	udomain_t *domain;
	urecord_t *record;
	ucontact_t *contact;
	str d, aor, contact_str, callid;
	int cseq, rc;
	struct ct_match cmatch = {CT_MATCH_NONE, NULL};
	short pkg_ver = get_bin_pkg_version(packet);

	bin_pop_str(packet, &d);
	bin_pop_str(packet, &aor);
	if (aor.len == 0) {
		LM_ERR("the AoR URI is missing the 'username' part!\n");
		goto error;
	}

	bin_pop_str(packet, &contact_str);
	bin_pop_str(packet, &callid);
	bin_pop_int(packet, &cseq);

	if (pkg_ver <= UL_BIN_V2)
		cmatch = (struct ct_match){CT_MATCH_CONTACT_CALLID, NULL};
	else
		bin_pop_ctmatch(packet, &cmatch);

	if (find_domain(&d, &domain) != 0) {
		LM_ERR("domain '%.*s' is not local\n", d.len, d.s);
		goto error;
	}

	lock_udomain(domain, &aor);

	/* failure in retrieving a urecord may be ok, because packet order in UDP
	 * is not guaranteed, so urecord_delete commands may arrive before
	 * ucontact_delete's */
	if (get_urecord(domain, &aor, &record) != 0) {
		LM_INFO("failed to fetch local urecord - ignoring request "
			"(ci: '%.*s')\n", callid.len, callid.s);
		goto out;
	}

	/* simply specify a higher cseq and completely avoid any complications */
	rc = get_ucontact(record, &contact_str, &callid, cseq + 1, &cmatch,
		&contact);
	switch (rc) {
	case -2:
	case -1:
		/* the DEL packet is too old (same or lower CSeq) */
		LM_ERR("contact '%.*s' found, but DEL too old: (rc: %d, ci: '%.*s')\n",
		        contact_str.len, contact_str.s, rc, callid.len, callid.s);
		goto out;
		break;

	case 1:
		LM_DBG("contact '%.*s' already deleted: (ci: '%.*s')\n", contact_str.len,
			contact_str.s, callid.len, callid.s);
		goto out;
		break;
	default:;
	}

	if (skip_replicated_db_ops)
		contact->flags |= FL_MEM;

	if (delete_ucontact(record, contact, NULL, 1) != 0) {
		LM_ERR("failed to delete ucontact '%.*s' (ci: '%.*s')\n",
			contact_str.len, contact_str.s, callid.len, callid.s);
		unlock_udomain(domain, &aor);
		goto error;
	}

	/* pull-sharing: our cached blob just went stale - withdraw it */
	if (cluster_mode == CM_PULL_SHARING)
		ul_pull_unpublish(record);

out:
	unlock_udomain(domain, &aor);
	free_pkg_str_list(cmatch.match_params);
	return 0;

error:
	free_pkg_str_list(cmatch.match_params);
	LM_ERR("failed to process replication event. dom: '%.*s', aor: '%.*s'\n",
	        d.len, d.s, aor.len, aor.s);
	return -1;
}

static int receive_sync_packet(bin_packet_t *packet)
{
	int is_contact;
	int rc = -1;

	while (clusterer_api.sync_chunk_iter(packet)) {
		bin_pop_int(packet, &is_contact);
		if (is_contact) {
			if (receive_ucontact_insert(packet) == 0)
				rc = 0;
		} else
			if (receive_urecord_insert(packet) == 0)
				rc = 0;
	}

	return rc;
}

void receive_binary_packets(bin_packet_t *pkt)
{
	int rc;

	LM_DBG("received a binary packet [%d]!\n", pkt->type);

	switch (pkt->type) {
	case REPL_URECORD_INSERT:
		_ensure_bin_version2(pkt, UL_BIN_V2, UL_BIN_V5, "usrloc aor-ins packet");
		rc = receive_urecord_insert(pkt);
		break;

	case REPL_URECORD_DELETE:
		_ensure_bin_version2(pkt, UL_BIN_V2, UL_BIN_V5, "usrloc aor-del packet");
		rc = receive_urecord_delete(pkt);
		break;

	case REPL_UCONTACT_INSERT:
		_ensure_bin_version2(pkt, UL_BIN_V2, UL_BIN_V5, "usrloc ct-ins packet");
		rc = receive_ucontact_insert(pkt);
		break;

	case REPL_UCONTACT_UPDATE:
		_ensure_bin_version2(pkt, UL_BIN_V2, UL_BIN_V5, "usrloc ct-upd packet");
		rc = receive_ucontact_update(pkt);
		break;

	case REPL_UCONTACT_DELETE:
		_ensure_bin_version2(pkt, UL_BIN_V2, UL_BIN_V5, "usrloc ct-del packet");
		rc = receive_ucontact_delete(pkt);
		break;

	case SYNC_PACKET_TYPE:
		_ensure_bin_version2(pkt, UL_BIN_V2, UL_BIN_V5, "usrloc sync packet");
		rc = receive_sync_packet(pkt);
		break;

	default:
		rc = -1;
		LM_ERR("invalid usrloc binary packet type: %d\n", pkt->type);
	}

	if (rc != 0)
		LM_ERR("failed to process binary packet!\n");
}

static int receive_sync_request(int node_id)
{
	struct ct_match cmatch = {CT_MATCH_CONTACT_CALLID, NULL};
	bin_packet_t *sync_packet;
	dlist_t *dl;
	udomain_t *dom;
	map_iterator_t it;
	struct urecord *r;
	ucontact_t* c;
	void **p;
	int i;

	for (dl = root; dl; dl = dl->next) {
		dom = dl->d;
		for(i = 0; i < dom->size; i++) {
			lock_ulslot(dom, i);
			for (map_first(dom->table[i].records, &it);
				iterator_is_valid(&it);
				iterator_next(&it)) {

				p = iterator_val(&it);
				if (p == NULL)
					goto error_unlock;
				r = (urecord_t *)*p;

				sync_packet = clusterer_api.sync_chunk_start(&contact_repl_cap,
									location_cluster, node_id, UL_BIN_VERSION);
				if (!sync_packet)
					goto error_unlock;

				/* urecord in this chunk */
				bin_push_int(sync_packet, 0);
				bin_push_urecord(sync_packet, r);

				for (c = r->contacts; c; c = c->next) {
					sync_packet = clusterer_api.sync_chunk_start(&contact_repl_cap,
										location_cluster, node_id, UL_BIN_VERSION);
					if (!sync_packet)
						goto error_unlock;

					/* ucontact in this chunk */
					bin_push_int(sync_packet, 1);
					bin_push_contact(sync_packet, r, c, &cmatch);
				}
			}
			unlock_ulslot(dom, i);
		}
	}

	return 0;

error_unlock:
	unlock_ulslot(dom, i);
	return -1;
}

void receive_cluster_event(enum clusterer_event ev, int node_id)
{
	if (ev == SYNC_REQ_RCV && receive_sync_request(node_id) < 0)
		LM_ERR("Failed to send sync data to node: %d\n", node_id);

	/* pull-sharing drops NOTHING on CLUSTER_NODE_DOWN - every record a
	 * survivor holds stays to natural expiry or an explicit
	 * un-REGISTER.  The event only QUEUES orphan adoption: after the
	 * settle delay, this node takes ownership of its deterministic
	 * share of the dead node's adoptable contacts, so their pinging
	 * and row maintenance resume instead of pausing until each UA
	 * re-registers. */
	if (cluster_mode != CM_PULL_SHARING || !adopt_pending)
		return;

	if (ev == CLUSTER_NODE_DOWN) {
		int i, slot = -1;

		lock_get(adopt_lock);
		for (i = 0; i < UL_ADOPT_PENDING; i++) {
			if (adopt_pending[i].nid == node_id) {
				slot = -2;
				break;
			}
			if (slot == -1 && !adopt_pending[i].nid)
				slot = i;
		}
		if (slot >= 0) {
			adopt_pending[slot].nid = node_id;
			adopt_pending[slot].due = get_ticks() + UL_ADOPT_DELAY;
		}
		lock_release(adopt_lock);

		if (slot == -1)
			LM_WARN("adoption queue full - node %d's orphans wait for "
				"their UAs' re-REGISTERs\n", node_id);
	} else if (ev == CLUSTER_NODE_UP) {
		int i;

		lock_get(adopt_lock);
		for (i = 0; i < UL_ADOPT_PENDING; i++)
			if (adopt_pending[i].nid == node_id)
				adopt_pending[i].nid = 0;
		lock_release(adopt_lock);
	}
}

/* take ownership of the dead node's adoptable contacts already held in
 * local memory (our deterministic share of them) */
static int ul_pull_adopt_memory(int dead_nid, int my_idx, int nr_nodes)
{
	dlist_t *dl;
	udomain_t *dom;
	map_iterator_t it;
	void **dest;
	urecord_t *r;
	ucontact_t *c;
	int i, touched, taken = 0;

	for (dl = root; dl; dl = dl->next) {
		dom = dl->d;
		for (i = 0; i < dom->size; i++) {
			lock_ulslot(dom, i);
			for (map_first(dom->table[i].records, &it);
			        iterator_is_valid(&it); iterator_next(&it)) {
				dest = iterator_val(&it);
				if (!dest)
					break;
				r = (urecord_t *)*dest;

				if (r->aorhash % nr_nodes != (unsigned int)my_idx)
					continue;

				touched = 0;
				for (c = r->contacts; c; c = c->next) {
					if (ul_ct_is_mine(c)
					        || ul_ct_owner_nid(c) != dead_nid)
						continue;
					if (!((c->sock && is_anycast(c->sock))
					        || c->path.len))
						continue;

					ul_ct_stamp_owner(c);
					c->flags &= ~(FL_PULLED | FL_MEM);
					if (c->state == CS_SYNC)
						c->state = CS_DIRTY;
					taken++;
					touched = 1;
				}
				if (touched)
					ul_pull_publish(r);
			}
			unlock_ulslot(dom, i);
		}
	}

	return taken;
}

/* called from the usrloc timer: run any due orphan adoptions and a
 * queued shared-tag takeover */
void ul_pull_process_failover(void)
{
	clusterer_node_t *nodes, *n;
	dlist_t *dl;
	int i, nid, my_idx, nr_nodes, taken;

	if (cluster_mode != CM_PULL_SHARING || !adopt_pending)
		return;

	if (*ha_import_pending) {
		*ha_import_pending = 0;
		taken = 0;
		for (dl = root; dl; dl = dl->next)
			taken += ul_pull_ledger_import(dl->d, 0, 0, 0);
		LM_INFO("shared-tag takeover: %d contacts are now maintained "
			"here\n", taken);
	}

	for (i = 0; i < UL_ADOPT_PENDING; i++) {
		lock_get(adopt_lock);
		if (!adopt_pending[i].nid
		        || (int)(get_ticks() - adopt_pending[i].due) < 0) {
			lock_release(adopt_lock);
			continue;
		}
		nid = adopt_pending[i].nid;
		adopt_pending[i].nid = 0;
		lock_release(adopt_lock);

		/* a node that came back meanwhile keeps its registrations -
		 * and re-acquires organically what it may have lost */
		nodes = clusterer_api.get_nodes(location_cluster);
		for (n = nodes; n; n = n->next)
			if (n->node_id == nid)
				break;
		if (nodes)
			clusterer_api.free_nodes(nodes);
		if (n)
			continue;

		my_idx = clusterer_api.get_my_index(location_cluster,
				&contact_repl_cap, &nr_nodes);
		if (my_idx < 0 || nr_nodes <= 0)
			continue;

		taken = ul_pull_adopt_memory(nid, my_idx, nr_nodes);
		for (dl = root; dl; dl = dl->next)
			taken += ul_pull_ledger_import(dl->d, nid, my_idx, nr_nodes);

		LM_INFO("node %d down: adopted %d of its contacts (share %d/%d) "
			"- pinging and row maintenance resume here\n",
			nid, taken, my_idx, nr_nodes);
	}
}

