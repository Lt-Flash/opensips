/*
 * Copyright (C) 2026 VoIPcloud
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

/*
 * Consumer messaging API - lets other modules (and, through them, the
 * script) exchange messages over the controller's encrypted UDP plane
 * instead of building their own transport.  A consumer inherits the
 * XChaCha20-Poly1305 group session key (with its rotation), the
 * per-packet receive gauntlet (magic gate, cluster_id filter, size
 * bound, per-source rate limiting) and the controller's membership,
 * with zero transport code of its own.
 *
 * Usage:
 *     clctr_api_t clctr;
 *     if (load_clctr_api(&clctr) < 0) ...        (mod_init)
 *     clctr.register_channel(&ch, my_cb);        (mod_init - PRE-FORK)
 *     clctr.send_mcast(cluster_id, &ch, &payload, 0);
 *     clctr.send_ucast(cluster_id, node_id, &ch, &payload, 0);
 *
 * Delivery contract:
 *   - the receive callback runs in the CONTROLLER'S WORKER PROCESS for
 *     that cluster, not in the process that called send.  A consumer
 *     that must wake another process brings its own mechanism (shm +
 *     eventfd, ipc_send_rpc, ...).  Keep callbacks short - they run on
 *     the cluster's receive path.
 *   - sends are marshalled to the same worker over IPC, so ordering is
 *     preserved per node and the anti-replay sequence space stays
 *     single-writer.
 *   - CLCTR_SEND_TO_SELF delivers to this node by invoking the local
 *     callback directly - never by hearing our own packet back.  A
 *     unicast addressed to our own node id degenerates to exactly that
 *     local dispatch, with no packet on the wire.
 *   - src_node_id is 0 when the sender had not been assigned an id yet.
 */

#ifndef CL_CTR_API_H
#define CL_CTR_API_H

#include "../../str.h"
#include "../../sr_module.h"

/* deliver to this node too, via local callback dispatch (never off the wire) */
#define CLCTR_SEND_TO_SELF   (1 << 0)

/* limits a consumer can rely on */
#define CLCTR_MAX_CHAN_LEN   31
#define CLCTR_MAX_PAYLOAD    1300

typedef void (*clctr_msg_cb_f)(int cluster_id, int src_node_id,
		str *channel, str *payload);

/* register a named channel; PRE-FORK only (call from mod_init).
 * Returns 0 on success, -1 on bad name / duplicate / table full. */
typedef int (*clctr_register_channel_f)(str *channel, clctr_msg_cb_f cb);

/* send to the cluster's multicast group / to one node by id.
 * Returns 0 = accepted for sending, -1 = bad arguments or unknown
 * cluster, -2 = cluster not ready (no worker / not joined yet).
 * "Accepted" means handed to the cluster worker - UDP gives no
 * delivery guarantee, by design (consumers must be loss-tolerant). */
typedef int (*clctr_send_mcast_f)(int cluster_id, str *channel,
		str *payload, int flags);
typedef int (*clctr_send_ucast_f)(int cluster_id, int node_id,
		str *channel, str *payload, int flags);

/* this node's controller-assigned id in the cluster; 0 = none yet */
typedef int (*clctr_get_my_node_id_f)(int cluster_id);

typedef struct clctr_api {
	clctr_register_channel_f  register_channel;
	clctr_send_mcast_f        send_mcast;
	clctr_send_ucast_f        send_ucast;
	clctr_get_my_node_id_f    get_my_node_id;
} clctr_api_t;

typedef int (*load_clctr_f)(clctr_api_t *api);

static inline int load_clctr_api(clctr_api_t *api)
{
	load_clctr_f load_clctr;

	load_clctr = (load_clctr_f)(void *)find_export("load_clctr", 0);
	if (!load_clctr)
		return -1;
	return load_clctr(api);
}

#endif /* CL_CTR_API_H */
