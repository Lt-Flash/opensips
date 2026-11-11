/*
 * Copyright (C) 2026 Yury Kirsanov
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* cachedb_perfd - CacheDB backend for the perfcached daemon (perfd://).
 * Handles are lazy and pid-stamped: never shared across fork(). */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../ut.h"
#include "../../cachedb/cachedb.h"
#include "../../cachedb/cachedb_id.h"
#include "../../async.h"
#include "../../pvar.h"

#include <perfd.h>

static str cache_mod_name = str_init("perfd");

/* applied to every perfd:// connection; 0 keeps the libperfd default */
static int connect_timeout_ms;        /* default 5000 */
static int query_timeout_ms;          /* default 5000, per send/recv */
static int fleet_refresh_ms;          /* default 30000, member list */
static int keepalive_s;               /* TCP keepalive idle; 0 = 30, -1 = off */
static int idle_ping_ms;              /* ping an idle standby; 0 = 30000, -1 = off */
static int async_conns = 8;           /* per-process async pool size */
static struct cachedb_url *perfd_script_urls;

#define PERFD_DEF_TIMEOUT_MS 5000
#define PERFD_KEY_MAX 512             /* key buffer, NUL included */
#define PERFD_TTL_MAX (1LL << 30)     /* the daemon keeps 32-bit expiry ticks */
#define PERFD_BACKOFF_MIN_MS 1000
#define PERFD_BACKOFF_MAX_MS 30000

static int set_connection(unsigned int type, void *val)
{
	return cachedb_store_url(&perfd_script_urls, (char *)val);
}

typedef struct {
	struct cachedb_id *id;         /* the cachedb_pool_con contract: */
	unsigned int ref;              /* these three fields first, always */
	struct cachedb_pool_con_t *next;

	perfd_t *pd;                   /* NULL until first use in a process */
	pid_t pid;                     /* who dialled pd */
#define PERFD_MAX_SEEDS 8
	struct {
		char *host;                /* pkg, NUL-terminated */
		int port;
		int said;                  /* its failure was logged this outage */
	} seed[PERFD_MAX_SEEDS];
	int nseeds;
	int seed_used;                 /* index of the seed pd came from */
	char *col;                     /* the collection, NUL-terminated */
	char *secret;
	int route, spares, encrypt;
	int policy;                    /* enum perfd_policy */
	/* the fleet as last reported, so a change is said once */
	unsigned int fleet_fp;
	time_t fleet_check_s;
	/* after a failed dial no op dials again before this (doubling backoff) */
	long long next_dial_ms;
	int backoff_ms;
} perfd_con;

static int mod_init(void);
static int child_init(int rank);
static void mod_destroy(void);

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int timeout_ms(void)
{
	return query_timeout_ms > 0 ? query_timeout_ms : PERFD_DEF_TIMEOUT_MS;
}

/* strict decimal: the whole token, within [lo, hi]; 0 ok, -1 not */
static int parse_ll(const char *s, long long lo, long long hi, long long *out)
{
	char *end;
	long long v;

	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno || *end || v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

/* @k copied into @buf as a C string: libperfd keys cannot hold a NUL */
static int perfd_key(const str *k, char *buf)
{
	if (k->len <= 0 || k->len >= PERFD_KEY_MAX) {
		LM_ERR("key of %d bytes - 1 to %d allowed\n", k->len,
			PERFD_KEY_MAX - 1);
		return -1;
	}
	if (memchr(k->s, 0, (size_t)k->len)) {
		LM_ERR("key '%.*s' holds a NUL byte\n", k->len, k->s);
		return -1;
	}
	memcpy(buf, k->s, (size_t)k->len);
	buf[k->len] = 0;
	return 0;
}

/* ---- URL extras ------------------------------------------------------- */

static int parse_extras(perfd_con *con, const char *opts)
{
	const char *p = opts;
	char tok[32];

	while (p && *p) {
		size_t n = strcspn(p, ",");
		long long v;

		if (n >= sizeof tok) {
			LM_ERR("perfd:// option '%.*s' is not known\n", (int)n, p);
			return -1;
		}
		memcpy(tok, p, n);
		tok[n] = 0;
		if (!strncmp(tok, "route=", 6) && parse_ll(tok + 6, 0, 1, &v) == 0)
			con->route = (int)v;
		else if (!strncmp(tok, "encrypt=", 8) &&
		        parse_ll(tok + 8, 0, 1, &v) == 0)
			con->encrypt = (int)v;
		else if (!strncmp(tok, "spares=", 7) &&
		        parse_ll(tok + 7, -1, 64, &v) == 0)
			/* 0 = none, -1 = one per member (the default) */
			con->spares = v == 0 ? PERFD_SPARES_NONE : (int)v;
		else if (!strcmp(tok, "policy=rr") || !strcmp(tok, "policy=roundrobin"))
			con->policy = PERFD_POLICY_ROUND_ROBIN;
		else if (!strcmp(tok, "policy=least"))
			con->policy = PERFD_POLICY_LEAST_CONN;
		else if (!strcmp(tok, "policy=weighted"))
			con->policy = PERFD_POLICY_WEIGHTED;
		else if (!strcmp(tok, "policy=failover"))
			con->policy = PERFD_POLICY_FAILOVER;
		else {
			LM_ERR("perfd:// option '%s' is not known or has a bad value "
				"(route=0|1, encrypt=0|1, spares=-1..64, "
				"policy=failover|rr|least|weighted)\n", tok);
			return -1;
		}
		p += n;
		if (*p)
			p++;
	}
	return 0;
}

/* ---- the per-process dial --------------------------------------------- */

/* @secrets must outlive @o: the options keep the pointer, not a copy */
static void perfd_con_opts(perfd_con *con, perfd_opts *o,
		const char **secrets)
{
	perfd_opts_init(o);
	if (con->secret && con->secret[0]) {
		secrets[0] = con->secret;
		secrets[1] = NULL;
		o->secrets = secrets;
	}
	o->policy = con->policy;
	o->connect_timeout_ms = connect_timeout_ms;
	o->io_timeout_ms = query_timeout_ms;
	o->refresh_ms = fleet_refresh_ms;
	o->keepalive_s = keepalive_s;
	o->idle_ping_ms = idle_ping_ms;
	o->route_keys = con->route;
	o->spares = con->spares;
	o->encrypt = con->encrypt;
	o->name = "opensips";
}

/* reports this handle's own view of the fleet, never grades it: an
 * unreachable member may still be served through the daemon's forward */
static unsigned int con_fleet_fp(perfd_t *pd)
{
	unsigned int fp = 2166136261u;
	int i, n = perfd_member_count(pd);
	char why[64];

	for (i = 0; i < n; i++) {
		int node = 0;
		const char *st;

		perfd_member_info(pd, i, NULL, 0, NULL, &node, NULL);
		st = perfd_member_state(pd, i, why, sizeof why);
		fp = (fp ^ (unsigned int)node) * 16777619u;
		fp = (fp ^ (unsigned int)(unsigned char)st[0]) * 16777619u;
	}
	return fp ^ (unsigned int)n;
}

static void con_report(perfd_con *con, const char *when)
{
	perfd_t *pd = con->pd;
	const char *ver = perfd_server_version(pd);
	int i, n = perfd_member_count(pd);
	char fleet[48];

	if (n)
		snprintf(fleet, sizeof fleet, "%d member(s) learned", n);
	else
		snprintf(fleet, sizeof fleet, "standalone, no member list");
	LM_NOTICE("perfd: %s %s:%d col=%s - server %s; %s; routing %s; "
		"%d standby held\n", when, con->seed[con->seed_used].host,
		con->seed[con->seed_used].port, con->col,
		ver[0] ? ver : "version not reported (older daemon, or no "
		"standbys wanted)", fleet, perfd_routing(pd) ? "on" : "off",
		perfd_spare_count(pd));
	for (i = 0; i < n; i++) {
		char addr[64], why[64];
		int port = 0, node = 0;
		const char *st;

		perfd_member_info(pd, i, addr, sizeof addr, &port, &node, NULL);
		st = perfd_member_state(pd, i, why, sizeof why);
		if (why[0])
			LM_NOTICE("perfd:   member %s:%d node %d %s: %s\n",
				addr, port, node, st, why);
		else
			LM_NOTICE("perfd:   member %s:%d node %d %s\n",
				addr, port, node, st);
	}
	con->fleet_fp = con_fleet_fp(pd);
	con->fleet_check_s = time(NULL);
}

static int perfd_async_warm(perfd_t *pd);

/* 1 when a dial may run now; else the con is backing off after a failure */
static int con_may_dial(perfd_con *con)
{
	return now_ms() >= con->next_dial_ms;
}

static void con_dial_result(perfd_con *con, int ok)
{
	int i;

	if (ok) {
		con->backoff_ms = 0;
		con->next_dial_ms = 0;
		for (i = 0; i < con->nseeds; i++)
			con->seed[i].said = 0;
		return;
	}
	con->backoff_ms = con->backoff_ms ? con->backoff_ms * 2 :
		PERFD_BACKOFF_MIN_MS;
	if (con->backoff_ms > PERFD_BACKOFF_MAX_MS)
		con->backoff_ms = PERFD_BACKOFF_MAX_MS;
	con->next_dial_ms = now_ms() + con->backoff_ms;
}

/* a seed's failure is logged once per outage; NULL when none answered */
static perfd_t *con_dial(perfd_con *con, const perfd_opts *o, int async)
{
	int start = con->nseeds > 1 ? (int)(getpid() % (pid_t)con->nseeds) : 0;
	int k;

	for (k = 0; k < con->nseeds; k++) {
		int i = (start + k) % con->nseeds;
		perfd_t *pd = async
			? perfd_connect_async(con->seed[i].host, con->seed[i].port, o)
			: perfd_connect(con->seed[i].host, con->seed[i].port, o);

		if (pd && async && perfd_async_warm(pd) < 0) {
			perfd_free(pd);
			pd = NULL;
		}
		if (pd) {
			con->seed_used = i;
			con_dial_result(con, 1);
			return pd;
		}
		if (!con->seed[i].said) {
			con->seed[i].said = 1;
			LM_WARN("perfd: seed %s:%d did not answer (%s)%s\n",
				con->seed[i].host, con->seed[i].port,
				perfd_error(NULL), con->nseeds > 1
				? " - trying the next seed" : "");
		}
	}
	con_dial_result(con, 0);
	LM_ERR("perfd: no seed answered for col=%s (last error: %s) - no "
		"dial for %d ms\n", con->col, perfd_error(NULL), con->backoff_ms);
	return NULL;
}

static int con_ready(perfd_con *con)
{
	perfd_opts o;
	const char *secrets[2];

	if (con->pd && con->pid == getpid() &&
	        perfd_state(con->pd) != PERFD_ST_FAILED) {
		/* re-report only on a change, checked at the library's refresh rate */
		time_t now = time(NULL);
		int period = (fleet_refresh_ms > 0 ? fleet_refresh_ms : 30000) / 1000;

		if (now - con->fleet_check_s >= (period > 0 ? period : 1)) {
			unsigned int fp = con_fleet_fp(con->pd);

			con->fleet_check_s = now;
			if (fp != con->fleet_fp)
				con_report(con, "fleet changed at");
		}
		return 0;
	}
	if (!con_may_dial(con))
		return -1;                 /* backing off: con_dial said why */
	if (con->pd && con->pid == getpid() &&
	        perfd_state(con->pd) == PERFD_ST_FAILED) {
		/* all links died at once: redial through the learned members first -
		 * the seed may be the one node still down */
		if (perfd_redial(con->pd) == 0) {
			con_report(con, "re-dialled");
			return 0;
		}
		LM_WARN("perfd: no learned member answered a re-dial (%s) - "
			"dialling the seeds afresh\n", perfd_error(con->pd));
	}
	if (con->pd) {
		/* inherited across fork (free only drops our fd copies) or FAILED:
		 * neither is reusable, only a fresh dial is */
		perfd_free(con->pd);
		con->pd = NULL;
	}
	perfd_con_opts(con, &o, secrets);
	con->pd = con_dial(con, &o, 0);
	if (!con->pd)
		return -1;
	con->pid = getpid();
	con_report(con, "dialled");
	return 0;
}

/* ---- connections ------------------------------------------------------ */

/* the core has no public group -> connection lookup for our async cmd */
#define PERFD_MAX_CONS 16
static perfd_con *perfd_cons[PERFD_MAX_CONS];
static int n_perfd_cons;

static perfd_con *perfd_con_by_group(const str *group)
{
	int i;

	for (i = 0; i < n_perfd_cons; i++) {
		const char *g = perfd_cons[i]->id->group_name;

		if (!g) {
			/* an ungrouped url answers to the empty group */
			if (!group->len)
				return perfd_cons[i];
			continue;
		}
		if ((int)strlen(g) == group->len &&
		    !memcmp(g, group->s, (size_t)group->len))
			return perfd_cons[i];
	}
	return NULL;
}

/* 0 ok, -1 logged; IPv6 literals as "[::1]:6479" */
static int seed_add(perfd_con *con, const char *b, const char *e)
{
	const char *colon = NULL, *p;
	char *host;
	int port;
	size_t hl;

	while (b < e && (*b == ' ' || *b == '\t'))
		b++;
	while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
		e--;
	for (p = b; p < e; p++)
		if (*p == ':')
			colon = p;             /* the LAST colon: IPv6 has many */
	if (!colon || colon == b || colon + 1 == e) {
		LM_ERR("perfd:// seed '%.*s' is not host:port - there is no "
			"default port to guess\n", (int)(e - b), b);
		return -1;
	}
	port = 0;
	for (p = colon + 1; p < e; p++) {
		if (*p < '0' || *p > '9' || port > 65535) {
			LM_ERR("perfd:// seed '%.*s' has a bad port\n",
				(int)(e - b), b);
			return -1;
		}
		port = port * 10 + (*p - '0');
	}
	if (port < 1 || port > 65535) {
		LM_ERR("perfd:// seed '%.*s' has a bad port\n", (int)(e - b), b);
		return -1;
	}
	if (con->nseeds >= PERFD_MAX_SEEDS) {
		LM_ERR("perfd:// more than %d seeds - the fleet is learned from "
			"the first that answers, a few are enough\n",
			PERFD_MAX_SEEDS);
		return -1;
	}
	if (*b == '[') {
		if (colon[-1] != ']') {
			LM_ERR("perfd:// seed '%.*s' has an unclosed '['\n",
				(int)(e - b), b);
			return -1;
		}
		b++;
		hl = (size_t)(colon - 1 - b);
	} else
		hl = (size_t)(colon - b);
	if (!hl) {
		LM_ERR("perfd:// seed '%.*s' has no host\n", (int)(e - b), b);
		return -1;
	}
	host = pkg_malloc(hl + 1);
	if (!host)
		return -1;
	memcpy(host, b, hl);
	host[hl] = 0;
	con->seed[con->nseeds].host = host;
	con->seed[con->nseeds].port = port;
	con->seed[con->nseeds].said = 0;
	con->nseeds++;
	return 0;
}

static void seeds_free(perfd_con *con)
{
	int i;

	for (i = 0; i < con->nseeds; i++)
		pkg_free(con->seed[i].host);
	con->nseeds = 0;
}

/* the seeds from the URL itself: the core's host field cuts the last char
 * of a multi-host list holding "[...]" and wraps ports over 65535 */
static int seeds_from_url(perfd_con *con, const char *url)
{
	const char *p = strstr(url, "://"), *end, *b, *e;
	const char *last_slash, *last_qm, *last_at = NULL, *q, *lim;

	if (!p)
		return -1;
	p += 3;
	/* the same '@' the core takes: the last one before the last '/' or '?' */
	last_slash = strrchr(p, '/');
	last_qm = strrchr(p, '?');
	lim = last_slash ? last_slash : (last_qm ? last_qm : p + strlen(p));
	for (q = p; q < lim; q++)
		if (*q == '@')
			last_at = q;
	b = last_at ? last_at + 1 : p;
	end = b + strcspn(b, "/?");
	while (b < end) {
		e = memchr(b, ',', (size_t)(end - b));
		if (!e)
			e = end;
		if (seed_add(con, b, e) < 0)
			return -1;
		b = e + 1;
		if (e == end)
			break;
	}
	return con->nseeds ? 0 : -1;
}

static void *perfd_new_connection(struct cachedb_id *id)
{
	perfd_con *con;

	if (!id->initial_url || !id->host) {
		LM_ERR("perfd:// needs an explicit host:port - there is no "
			"default port to guess\n");
		return NULL;
	}
	if (!id->database || !id->database[0]) {
		LM_ERR("perfd:// needs the collection as the URL path, e.g. "
			"perfd://:secret@10.0.0.1:6479/subs\n");
		return NULL;
	}
	con = pkg_malloc(sizeof *con);
	if (!con)
		return NULL;
	memset(con, 0, sizeof *con);
	con->id = id;
	con->ref = 1;
	con->col = id->database;
	con->secret = id->password ? id->password :
		(id->username && id->username[0] ? id->username : NULL);
	con->route = 1;
	con->spares = -1;
	con->encrypt = 1;
	if (seeds_from_url(con, id->initial_url) < 0 ||
	    (id->extra_options && parse_extras(con, id->extra_options) < 0)) {
		seeds_free(con);
		pkg_free(con);
		return NULL;
	}
	if (con->nseeds > 1) {
		char list[PERFD_MAX_SEEDS * 48], *w = list;
		size_t left = sizeof list;
		int k;

		list[0] = 0;
		for (k = 0; k < con->nseeds && left > 1; k++) {
			int n = snprintf(w, left, "%s%s:%d", k ? ", " : "",
				con->seed[k].host, con->seed[k].port);

			if (n < 0 || (size_t)n >= left)
				break;
			w += n;
			left -= (size_t)n;
		}
		LM_INFO("perfd: col=%s has %d seeds (%s) - each worker dials "
			"from a pid-rotated start and learns the fleet from the "
			"first that answers\n", con->col, con->nseeds, list);
	}
	if (n_perfd_cons < PERFD_MAX_CONS)
		perfd_cons[n_perfd_cons++] = con;
	else
		LM_WARN("more than %d perfd urls - '%s' is reachable through "
			"cache_fetch() but not by group from perfd_fetch()\n",
			PERFD_MAX_CONS, id->group_name ? id->group_name : "");
	/* deliberately NOT dialling here: this may run pre-fork, and the
	 * handle must belong to the process that uses it */
	return con;
}

static cachedb_con *perfd_cdb_init(str *url)
{
	return cachedb_do_init(url, (void *)perfd_new_connection);
}

static void perfd_apool_forget(perfd_con *con);

static void perfd_free_connection(cachedb_pool_con *cpc)
{
	perfd_con *con = (perfd_con *)cpc;
	int i;

	/* a con freed before fork (b2b_logic's restore) must not be found later */
	for (i = 0; i < n_perfd_cons; i++)
		if (perfd_cons[i] == con) {
			memmove(&perfd_cons[i], &perfd_cons[i + 1],
				(size_t)(n_perfd_cons - i - 1) * sizeof *perfd_cons);
			n_perfd_cons--;
			break;
		}
	perfd_apool_forget(con);
	if (con->pd && con->pid == getpid())
		perfd_free(con->pd);
	seeds_free(con);
	pkg_free(con);
}

static void perfd_cdb_destroy(cachedb_con *con)
{
	cachedb_do_close(con, perfd_free_connection);
}

#define CON(c) ((perfd_con *)((c)->data))

/* ---- raw queries ------------------------------------------------------ */

/* MGET returns one row per key in the order asked, a miss as CDB_NULL -
 * a shifted result set would pair values with the wrong keys */
#define PERFD_RAW_MAXKEYS 256
#define PERFD_RAW_MAXARGS (PERFD_RAW_MAXKEYS + 2)

static void perfd_raw_free_rows(cdb_raw_entry **rows, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (rows[i]) {
			if (rows[i][0].type == CDB_STR && rows[i][0].val.s.s)
				pkg_free(rows[i][0].val.s.s);
			pkg_free(rows[i]);
		}
	}
	pkg_free(rows);
}

/* one row as wide as the script asked: the core reads every column */
static cdb_raw_entry *perfd_raw_row(int ncols)
{
	cdb_raw_entry *row;
	int j;

	if (ncols < 1)
		ncols = 1;
	row = pkg_malloc((size_t)ncols * sizeof *row);
	if (!row)
		return NULL;
	memset(row, 0, (size_t)ncols * sizeof *row);
	for (j = 0; j < ncols; j++)
		row[j].type = CDB_NULL;
	return row;
}

static int perfd_raw_tokenize(char *buf, char **argv, int maxargs)
{
	int argc = 0, sq = 0, dq = 0;
	char *p, *arg = NULL;

	for (p = buf; *p; p++) {
		if (sq || dq) {
			if ((sq && *p == '\'') || (dq && *p == '"')) {
				if (p[1] && p[1] != ' ' && p[1] != '\t')
					return -1;     /* text glued to a closing quote */
				*p = 0;
				sq = dq = 0;
			}
			continue;
		}
		if (*p == '\'' || *p == '"') {
			if (arg)
				return -1;         /* a quote glued to a word */
			if (argc >= maxargs)
				return -1;
			if (*p == '\'')
				sq = 1;
			else
				dq = 1;
			argv[argc++] = p + 1;
		} else if (*p == ' ' || *p == '\t') {
			*p = 0;
			arg = NULL;
		} else if (!arg) {
			if (argc >= maxargs)
				return -1;
			arg = argv[argc++] = p;
		}
	}
	if (sq || dq)
		return -1;                     /* unterminated quote */
	return argc;
}

/* @reply is NULL in the two-argument script form; nothing is built */
static int perfd_raw_reply1(cdb_raw_entry ***reply, int *reply_no,
		int ncols, int is_str, int n, const char *s, size_t slen)
{
	cdb_raw_entry **rows;

	if (!reply)
		return 0;
	rows = pkg_malloc(sizeof *rows);
	if (!rows)
		return -1;
	rows[0] = perfd_raw_row(ncols);
	if (!rows[0]) {
		pkg_free(rows);
		return -1;
	}
	if (!is_str) {
		rows[0][0].type = CDB_INT32;
		rows[0][0].val.n = n;
	} else if (s) {
		rows[0][0].val.s.s = pkg_malloc(slen ? slen : 1);
		if (!rows[0][0].val.s.s) {
			pkg_free(rows[0]);
			pkg_free(rows);
			return -1;
		}
		memcpy(rows[0][0].val.s.s, s, slen);
		rows[0][0].val.s.len = (int)slen;
		rows[0][0].type = CDB_STR;
	}
	*reply = rows;
	if (reply_no)
		*reply_no = 1;
	return 0;
}

/* bounded appends into one buffer; any overflow sticks as len = -1 */
typedef struct { char *b; size_t cap; long len; } perfd_buf;

static void buf_raw(perfd_buf *w, const char *s, size_t n)
{
	if (w->len < 0 || (size_t)w->len + n + 1 > w->cap) {
		w->len = -1;
		return;
	}
	memcpy(w->b + w->len, s, n);
	w->len += (long)n;
	w->b[w->len] = 0;
}

static void buf_str(perfd_buf *w, const char *s)
{
	buf_raw(w, s, strlen(s));
}

/* @s as a JSON string literal */
static void buf_json(perfd_buf *w, const char *s)
{
	char esc[8];

	buf_raw(w, "\"", 1);
	for (; *s && w->len >= 0; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = (char)c;
			buf_raw(w, esc, 2);
		} else if (c < 0x20) {
			snprintf(esc, sizeof esc, "\\u%04x", c);
			buf_raw(w, esc, 6);
		} else
			buf_raw(w, (const char *)&c, 1);
	}
	buf_raw(w, "\"", 1);
}

/* one JSON value at *@p, skipping spaces; 0 ok, -1 not JSON (depth-bounded) */
static int json_skip(const char **p, int depth)
{
	const char *s = *p;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if (depth > 64)
		return -1;
	if (*s == '{' || *s == '[') {
		char close = *s == '{' ? '}' : ']';
		int first = 1;

		for (s++;; first = 0) {
			while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
				s++;
			if (*s == close && first) {
				s++;
				break;
			}
			if (close == '}') {
				if (*s != '"' || json_skip(&s, depth + 1) < 0)
					return -1;
				while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
					s++;
				if (*s++ != ':')
					return -1;
			}
			if (json_skip(&s, depth + 1) < 0)
				return -1;
			while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
				s++;
			if (*s == ',') {
				s++;
				continue;
			}
			if (*s++ != close)
				return -1;
			break;
		}
	} else if (*s == '"') {
		for (s++; *s != '"'; s++) {
			if (!*s || (unsigned char)*s < 0x20)
				return -1;
			if (*s == '\\' && !*++s)
				return -1;
		}
		s++;
	} else if (!strncmp(s, "true", 4) || !strncmp(s, "null", 4)) {
		s += 4;
	} else if (!strncmp(s, "false", 5)) {
		s += 5;
	} else if (*s == '-' || (*s >= '0' && *s <= '9')) {
		s += strspn(s, "-+0123456789.eE");
	} else
		return -1;
	*p = s;
	return 0;
}

/* exactly one JSON value, nothing around it: it goes into the request raw */
static int json_is_value(const char *s)
{
	if (json_skip(&s, 0) < 0)
		return 0;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	return !*s;
}

/* JSON.SET with NX/XX: the daemon's jset decides on the path, atomically.
 * 1 set, 0 declined, -1 error */
static int perfd_jset_cond(perfd_con *con, const char *key, const char *path,
		const char *val, long long ttl, int nx)
{
	perfd_buf w;
	char num[32], *reply;
	int rc = -1;

	w.cap = 128 + 6 * (strlen(con->col) + strlen(key) + strlen(path) +
		strlen(val));
	w.len = 0;
	w.b = pkg_malloc(w.cap);
	if (!w.b)
		return -1;
	w.b[0] = 0;
	buf_str(&w, "{\"col\":");
	buf_json(&w, con->col);
	buf_str(&w, ",\"key\":");
	buf_json(&w, key);
	buf_str(&w, ",\"path\":");
	buf_json(&w, path);
	buf_str(&w, ",\"val\":");
	buf_str(&w, val);                  /* a JSON value, checked by the caller */
	if (ttl > 0) {
		snprintf(num, sizeof num, ",\"ttl\":%lld", ttl);
		buf_str(&w, num);
	}
	buf_str(&w, nx ? ",\"nx\":true}" : ",\"xx\":true}");
	if (w.len < 0) {
		LM_BUG("JSON.SET params overflow\n");
		goto out;
	}
	reply = perfd_command(con->pd, "jset", w.b);
	if (!reply) {
		const char *e = perfd_error(con->pd);

		/* the daemon refuses a declined NX/XX with its path error */
		if ((nx && strstr(e, "value exists")) ||
		    (!nx && (strstr(e, "no value at path") || strstr(e, "no such key"))))
			rc = 0;
		else
			LM_ERR("JSON.SET %s %s: %s\n", key, path, e);
		goto out;
	}
	if (strstr(reply, "\"set\":true"))
		rc = 1;
	else if (strstr(reply, "\"set\":false"))
		rc = 0;
	else
		LM_ERR("JSON.SET %s %s: unexpected reply %s\n", key, path, reply);
	perfd_free_value(reply);
out:
	pkg_free(w.b);
	return rc;
}

static int perfd_cdb_raw_query(cachedb_con *cachedb_con, str *query,
		cdb_raw_entry ***reply, int expected_kv_no, int *reply_no)
{
	perfd_con *con = CON(cachedb_con);
	char buf[4096], *argv[PERFD_RAW_MAXARGS];
	const char *ckeys[PERFD_RAW_MAXKEYS];
	void *vals[PERFD_RAW_MAXKEYS];
	size_t vlens[PERFD_RAW_MAXKEYS];
	cdb_raw_entry **rows = NULL;
	int argc, nkeys = 0, i, rc, built = 0;
	long long v;

	if (!query || query->len <= 0 || query->len >= (int)sizeof buf) {
		LM_ERR("raw query missing or longer than %zu bytes\n",
			sizeof buf - 1);
		return -1;
	}
	if (memchr(query->s, 0, (size_t)query->len)) {
		LM_ERR("raw query '%.*s' holds a NUL byte\n", query->len, query->s);
		return -1;
	}
	memcpy(buf, query->s, query->len);
	buf[query->len] = 0;
	argc = perfd_raw_tokenize(buf, argv, PERFD_RAW_MAXARGS);
	if (argc < 1) {
		LM_ERR("raw query '%.*s': %s\n", query->len, query->s,
			argc < 0 ? "bad quoting or too many arguments" : "empty");
		return -1;
	}
	for (i = 1; i < argc && i < 2; i++)
		if (!argv[i][0] || strlen(argv[i]) >= PERFD_KEY_MAX) {
			LM_ERR("raw query '%.*s': key of %zu bytes - 1 to %d "
				"allowed\n", query->len, query->s, strlen(argv[i]),
				PERFD_KEY_MAX - 1);
			return -1;
		}
	if (con_ready(con) < 0)
		return -1;

	if (strcasecmp(argv[0], "EXISTS") == 0 && argc == 2) {
		rc = perfd_exists(con->pd, con->col, argv[1]);
		if (rc < 0) {
			LM_ERR("EXISTS %s: %s\n", argv[1], perfd_error(con->pd));
			return -1;
		}
		return perfd_raw_reply1(reply, reply_no, expected_kv_no, 0, rc,
			NULL, 0);
	}
	if (strcasecmp(argv[0], "EXPIRE") == 0 && argc == 3) {
		if (parse_ll(argv[2], LLONG_MIN, PERFD_TTL_MAX, &v) < 0) {
			LM_ERR("EXPIRE %s: '%s' is not a number of seconds up to "
				"%lld\n", argv[1], argv[2], PERFD_TTL_MAX);
			return -1;
		}
		if (v <= 0) {
			/* as Redis: a past expiry deletes the key */
			rc = perfd_del(con->pd, con->col, argv[1]);
			if (rc < 0) {
				LM_ERR("EXPIRE %s: %s\n", argv[1], perfd_error(con->pd));
				return -1;
			}
			return perfd_raw_reply1(reply, reply_no, expected_kv_no, 0,
				rc > 0, NULL, 0);
		}
		rc = perfd_expire(con->pd, con->col, argv[1], v);
		if (rc < 0) {
			LM_ERR("EXPIRE %s: %s\n", argv[1], perfd_error(con->pd));
			return -1;
		}
		/* Redis: 1 = set, 0 = no such key; libperfd: 0 ok, 1 absent */
		return perfd_raw_reply1(reply, reply_no, expected_kv_no, 0,
			rc == 0, NULL, 0);
	}
	if (strcasecmp(argv[0], "JSON.SET") == 0 && argc >= 4) {
		long long ttl = 0;
		int nx = 0, xx = 0;

		for (i = 4; i < argc; i++) {
			if (strcasecmp(argv[i], "NX") == 0)
				nx = 1;
			else if (strcasecmp(argv[i], "XX") == 0)
				xx = 1;
			else if (strcasecmp(argv[i], "EX") == 0 && i + 1 < argc) {
				if (parse_ll(argv[++i], 1, PERFD_TTL_MAX, &ttl) < 0) {
					LM_ERR("JSON.SET: EX '%s' is not 1 to %lld "
						"seconds\n", argv[i], PERFD_TTL_MAX);
					return -1;
				}
			} else {
				LM_ERR("JSON.SET: unknown option '%s'\n", argv[i]);
				return -1;
			}
		}
		if (nx && xx) {
			LM_ERR("JSON.SET: NX and XX together\n");
			return -1;
		}
		if ((nx || xx) && !json_is_value(argv[3])) {
			LM_ERR("JSON.SET %s %s: the value is not one JSON value\n",
				argv[1], argv[2]);
			return -1;
		}
		if (nx || xx) {
			rc = perfd_jset_cond(con, argv[1], argv[2], argv[3], ttl, nx);
			if (rc < 0)
				return -1;
			return perfd_raw_reply1(reply, reply_no, expected_kv_no, 1, 0,
				rc ? "OK" : NULL, rc ? 2 : 0);
		}
		rc = perfd_jset(con->pd, con->col, argv[1], argv[2], argv[3], ttl);
		if (rc != 0) {
			LM_ERR("JSON.SET %s %s: %s\n", argv[1], argv[2],
				perfd_error(con->pd));
			return -1;
		}
		return perfd_raw_reply1(reply, reply_no, expected_kv_no, 1, 0,
			"OK", 2);
	}
	if (strcasecmp(argv[0], "JSON.DEL") == 0 && (argc == 2 || argc == 3)) {
		rc = perfd_jdel(con->pd, con->col, argv[1], argc == 3 ? argv[2] : "$");
		if (rc < 0) {
			LM_ERR("JSON.DEL %s: %s\n", argv[1], perfd_error(con->pd));
			return -1;
		}
		return perfd_raw_reply1(reply, reply_no, expected_kv_no, 0, rc,
			NULL, 0);
	}
	if (strcasecmp(argv[0], "JSON.GET") == 0 && (argc == 2 || argc == 3)) {
		char *frag = NULL;

		rc = perfd_jget(con->pd, con->col, argv[1], argc == 3 ? argv[2] : "$",
			&frag);
		if (rc < 0) {
			LM_ERR("JSON.GET %s: %s\n", argv[1], perfd_error(con->pd));
			return -1;
		}
		rc = perfd_raw_reply1(reply, reply_no, expected_kv_no, 1, 0,
			rc ? frag : NULL, rc && frag ? strlen(frag) : 0);
		perfd_free_value(frag);
		return rc;
	}
	if (strcasecmp(argv[0], "MGET") != 0) {
		LM_ERR("unsupported raw query '%.*s' - this driver accepts MGET, "
			"EXISTS, EXPIRE, JSON.GET, JSON.SET and JSON.DEL; the fixed "
			"verbs (get/set/remove/add/sub) have their own cachedb "
			"calls\n", query->len, query->s);
		return -1;
	}
	for (i = 1; i < argc; i++) {
		if (nkeys >= PERFD_RAW_MAXKEYS) {
			LM_ERR("MGET with more than %d keys\n", PERFD_RAW_MAXKEYS);
			return -1;
		}
		if (!argv[i][0] || strlen(argv[i]) >= PERFD_KEY_MAX) {
			LM_ERR("MGET: key %d is empty or %d bytes or longer\n", i,
				PERFD_KEY_MAX);
			return -1;
		}
		ckeys[nkeys++] = argv[i];
	}
	if (!nkeys) {
		LM_ERR("MGET without keys\n");
		return -1;
	}
	rc = perfd_mget(con->pd, con->col, ckeys, nkeys, vals, vlens);
	if (rc < 0) {
		LM_ERR("mget of %d key(s): %s\n", nkeys, perfd_error(con->pd));
		return -1;
	}

	rows = pkg_malloc((size_t)nkeys * sizeof *rows);
	if (!rows)
		goto oom;
	memset(rows, 0, (size_t)nkeys * sizeof *rows);
	for (i = 0; i < nkeys; i++) {
		rows[i] = perfd_raw_row(expected_kv_no);
		if (!rows[i])
			goto oom;
		built++;
		if (!vals[i])                  /* a miss keeps its position */
			continue;
		rows[i][0].val.s.s = pkg_malloc(vlens[i] ? vlens[i] : 1);
		if (!rows[i][0].val.s.s)
			goto oom;
		memcpy(rows[i][0].val.s.s, vals[i], vlens[i]);
		rows[i][0].val.s.len = (int)vlens[i];
		rows[i][0].type = CDB_STR;
	}
	for (i = 0; i < nkeys; i++)
		perfd_free_value(vals[i]);
	if (!reply) {
		perfd_raw_free_rows(rows, built);
		return 0;
	}
	*reply = rows;
	if (reply_no)
		*reply_no = nkeys;
	return 0;

oom:
	LM_ERR("no more pkg memory for %d MGET row(s)\n", nkeys);
	for (i = 0; i < nkeys; i++)
		perfd_free_value(vals[i]);
	if (rows)
		perfd_raw_free_rows(rows, built);
	return -1;
}

/* ---- cachedb ops ------------------------------------------------------ */

static int perfd_cdb_get_con(perfd_con *con, str *attr, str *val)
{
	char key[PERFD_KEY_MAX];
	void *v = NULL;
	size_t vlen = 0;
	long long ttl;
	int rc;

	if (perfd_key(attr, key) < 0 || con_ready(con) < 0)
		return -1;
	rc = perfd_get(con->pd, con->col, key, &v, &vlen, &ttl);
	if (rc < 0) {
		LM_ERR("get %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	if (rc == 0)
		return -2;                     /* miss */
	val->s = pkg_malloc(vlen + 1);
	if (!val->s) {
		perfd_free_value(v);
		return -1;
	}
	memcpy(val->s, v, vlen);
	val->s[vlen] = 0;
	val->len = (int)vlen;
	perfd_free_value(v);
	return 0;
}

static int perfd_cdb_get(cachedb_con *cachedb_con, str *attr, str *val)
{
	return perfd_cdb_get_con(CON(cachedb_con), attr, val);
}

static int perfd_cdb_set(cachedb_con *cachedb_con, str *attr, str *val,
		int expires)
{
	perfd_con *con = CON(cachedb_con);
	char key[PERFD_KEY_MAX];

	if (perfd_key(attr, key) < 0 || con_ready(con) < 0)
		return -1;
	/* as cachedb_redis: 0 or less stores without expiry (the daemon's
	 * negative ttl would keep the old one) */
	if (perfd_set(con->pd, con->col, key, val->s, (size_t)val->len,
	        expires > 0 ? expires : 0) != 0) {
		LM_ERR("set %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	return 0;
}

static int perfd_cdb_remove(cachedb_con *cachedb_con, str *attr)
{
	perfd_con *con = CON(cachedb_con);
	char key[PERFD_KEY_MAX];

	if (perfd_key(attr, key) < 0 || con_ready(con) < 0)
		return -1;
	if (perfd_del(con->pd, con->col, key) < 0) {
		LM_ERR("remove %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	return 0;
}

/* add and sub in one: @expires 0 or less keeps the key's expiry (Redis
 * INCRBY); the result is applied even when it does not fit an int */
static int perfd_cdb_addsub(cachedb_con *cachedb_con, str *attr,
		long long by, int expires, int *new_val, const char *verb)
{
	perfd_con *con = CON(cachedb_con);
	char key[PERFD_KEY_MAX];
	long long nv = 0;

	if (perfd_key(attr, key) < 0 || con_ready(con) < 0)
		return -1;
	if (perfd_add(con->pd, con->col, key, by, expires > 0 ? expires : -1,
	        &nv) != 0) {
		LM_ERR("%s %.*s: %s\n", verb, attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	if (nv < INT_MIN || nv > INT_MAX) {
		LM_ERR("%s %.*s: the counter is now %lld, beyond an int\n", verb,
			attr->len, attr->s, nv);
		return -1;
	}
	if (new_val)
		*new_val = (int)nv;
	return 0;
}

static int perfd_cdb_add(cachedb_con *cachedb_con, str *attr, int val,
		int expires, int *new_val)
{
	return perfd_cdb_addsub(cachedb_con, attr, val, expires, new_val, "add");
}

static int perfd_cdb_sub(cachedb_con *cachedb_con, str *attr, int val,
		int expires, int *new_val)
{
	return perfd_cdb_addsub(cachedb_con, attr, -(long long)val, expires,
		new_val, "sub");
}

static int perfd_cdb_get_counter(cachedb_con *cachedb_con, str *attr,
		int *val)
{
	str sval;
	char num[24];
	long long v;
	int rc;

	rc = perfd_cdb_get(cachedb_con, attr, &sval);
	if (rc != 0)
		return rc;
	if (sval.len <= 0 || sval.len >= (int)sizeof num) {
		v = 0;
		rc = -1;
	} else {
		memcpy(num, sval.s, sval.len);
		num[sval.len] = 0;
		rc = parse_ll(num, INT_MIN, INT_MAX, &v);
	}
	pkg_free(sval.s);
	if (rc < 0) {
		LM_ERR("counter %.*s does not hold an int\n", attr->len, attr->s);
		return -1;
	}
	if (val)
		*val = (int)v;
	return 0;
}

/* ---- async fetch ------------------------------------------------------ */

/* OpenSIPS async watches one fd per request, so each in-flight fetch has a
 * handle of its own, dialled to the key's owner when the fleet routes */
typedef struct {
	perfd_t *pd;                   /* PERFD_ST_READY once warmed */
	perfd_con *owner;              /* whose collection/secret it dials */
	char host[64];                 /* the member it is pinned to ("" = seed) */
	int port;
	int busy;                      /* one in-flight request per slot */
	int dead;                      /* freed at the next scan, not under tm */
} perfd_aslot;

static perfd_aslot *apool;
static int apool_n;                    /* slots in use (dialled or holes) */
static pid_t apool_pid;                /* who owns them (fork safety) */

/* one script request in flight */
typedef struct {
	perfd_aslot *slot;
	pv_spec_t *dest;
	char *result;                  /* callback-owned, we free it */
	size_t len;
	long long deadline_ms;
	int fired;                     /* the callback ran */
	int failed;
} perfd_areq;

/* only stash the result: the pv is set in the resume, not in a callback */
static void perfd_async_cb(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	perfd_areq *rq = (perfd_areq *)ctx;

	rq->fired = 1;
	if (!result) {
		rq->failed = 1;
		LM_ERR("async fetch failed: %s\n", errmsg ? errmsg : "?");
		return;
	}
	rq->result = result;           /* malloc'd by the library; ours now */
	rq->len = len;
}

/* drive @pd until @done(pd, arg) or the deadline; 0 done, -1 failed/late */
static int perfd_drive(perfd_t *pd, long long deadline,
		int (*done)(perfd_t *, void *), void *arg)
{
	while (!done(pd, arg)) {
		struct pollfd pfd;
		int ev = perfd_events(pd), rc;
		long long left = deadline - now_ms();

		if (perfd_state(pd) == PERFD_ST_FAILED || left <= 0)
			return -1;
		pfd.fd = perfd_fd(pd);
		if (pfd.fd < 0)
			return -1;
		pfd.events = 0;
		if (ev & PERFD_EV_READ)
			pfd.events |= POLLIN;
		if (ev & PERFD_EV_WRITE)
			pfd.events |= POLLOUT;
		if (!pfd.events)
			pfd.events = POLLIN;
		rc = poll(&pfd, 1, left > INT_MAX ? INT_MAX : (int)left);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (rc == 0)
			return -1;
		if ((pfd.revents & POLLOUT) && perfd_write_ready(pd) < 0)
			return -1;
		/* ERR/HUP/NVAL: the read reports the failure */
		if ((pfd.revents & ~POLLOUT) && perfd_read_ready(pd) < 0)
			return -1;
	}
	return 0;
}

static int perfd_not_connecting(perfd_t *pd, void *arg)
{
	(void)arg;
	return perfd_state(pd) != PERFD_ST_CONNECTING;
}

/* CONNECTING -> READY within connect_timeout; 0 ready, -1 not */
static int perfd_async_warm(perfd_t *pd)
{
	int ms = connect_timeout_ms > 0 ? connect_timeout_ms :
		PERFD_DEF_TIMEOUT_MS;

	if (perfd_drive(pd, now_ms() + ms, perfd_not_connecting, NULL) < 0 ||
	    perfd_state(pd) != PERFD_ST_READY) {
		LM_ERR("async connection not ready within %d ms: %s\n", ms,
			perfd_error(pd));
		return -1;
	}
	return 0;
}

static void slot_clear(perfd_aslot *s)
{
	if (s->pd)
		perfd_free(s->pd);
	memset(s, 0, sizeof *s);
}

static void perfd_apool_forget(perfd_con *con)
{
	int i;

	if (!apool || apool_pid != getpid())
		return;
	for (i = 0; i < apool_n; i++)
		if (apool[i].owner == con) {
			if (apool[i].busy)
				LM_BUG("perfd connection freed with a fetch in flight\n");
			slot_clear(&apool[i]);
		}
}

/* the member that owns @key, when @con's fleet routes; 0 found, -1 none */
static int perfd_key_owner(perfd_con *con, const char *key, char *host,
		size_t hcap, int *port)
{
	int idx;

	if (!con->route || con_ready(con) < 0 || !perfd_routing(con->pd))
		return -1;
	idx = perfd_owner_of(con->pd, con->col, key);
	if (idx < 0 || perfd_member_info(con->pd, idx, host, hcap, port,
	        NULL, NULL) < 0 || !host[0] || *port <= 0)
		return -1;
	return 0;
}

static perfd_t *slot_dial(perfd_con *con, const char *host, int port)
{
	perfd_opts o;
	const char *secrets[2];
	perfd_t *pd;

	if (!con_may_dial(con))
		return NULL;
	perfd_con_opts(con, &o, secrets);
	if (!host[0])
		return con_dial(con, &o, 1);
	/* pinned to the owner: no fleet, no standbys, no moving */
	o.spares = PERFD_SPARES_NONE;
	o.route_keys = 0;
	o.pin = 1;
	pd = perfd_connect_async(host, port, &o);
	if (pd && perfd_async_warm(pd) < 0) {
		perfd_free(pd);
		pd = NULL;
	}
	con_dial_result(con, pd != NULL);
	if (!pd)
		LM_WARN("perfd: owner %s:%d did not take an async connection - "
			"no dial for %d ms\n", host, port, con->backoff_ms);
	return pd;
}

/* a free, warm slot for @key of @con; NULL = serve it synchronously */
static perfd_aslot *perfd_async_slot(perfd_con *con, const char *key)
{
	char host[64] = "";
	int port = 0, i, hole = -1, idle = -1;
	perfd_aslot *s;

	if (apool && apool_pid != getpid()) {
		/* inherited across a fork: the sockets belong to the parent */
		pkg_free(apool);
		apool = NULL;
		apool_n = 0;
	}
	if (!apool) {
		apool = pkg_malloc((size_t)async_conns * sizeof *apool);
		if (!apool) {
			LM_ERR("no pkg memory for %d async slots\n", async_conns);
			return NULL;
		}
		memset(apool, 0, (size_t)async_conns * sizeof *apool);
		apool_pid = getpid();
		apool_n = 0;
	}
	if (perfd_key_owner(con, key, host, sizeof host, &port) < 0) {
		host[0] = 0;
		port = 0;
	}
	for (i = 0; i < apool_n; i++) {
		s = &apool[i];
		if (s->busy)
			continue;
		/* a failed or timed-out handle is rebuilt, never reused */
		if (s->pd && (s->dead || perfd_state(s->pd) != PERFD_ST_READY))
			slot_clear(s);
		if (!s->pd) {
			if (hole < 0)
				hole = i;
			continue;
		}
		if (s->owner == con && s->port == port && !strcmp(s->host, host))
			return s;
		if (idle < 0)
			idle = i;
	}
	if (hole < 0 && apool_n < async_conns)
		hole = apool_n++;
	if (hole < 0 && idle >= 0) {
		/* full of other keys' owners or groups: re-dial an idle one */
		slot_clear(&apool[idle]);
		hole = idle;
	}
	if (hole < 0)
		return NULL;                   /* every slot is in flight */
	s = &apool[hole];
	s->pd = slot_dial(con, host, port);
	if (!s->pd)
		return NULL;
	s->owner = con;
	s->port = port;
	memcpy(s->host, host, sizeof s->host);
	return s;
}

static void perfd_areq_done(perfd_areq *rq)
{
	if (rq->slot) {
		rq->slot->busy = 0;
		if (rq->failed)
			rq->slot->dead = 1;    /* tm still holds its fd: free later */
	}
	if (rq->result)
		perfd_free_value(rq->result);
	pkg_free(rq);
}

static int perfd_rq_settled(perfd_t *pd, void *arg)
{
	perfd_areq *rq = (perfd_areq *)arg;

	(void)pd;
	return rq->fired || rq->failed;
}

/* the reply decoded into the script's variable; 1 found, -2 miss, -1 error */
static int perfd_areq_finish(perfd_areq *rq, struct sip_msg *msg)
{
	pv_value_t val;

	if (rq->failed || !rq->result) {
		rq->failed = 1;
		perfd_areq_done(rq);
		return -1;
	}
	/* reply is [u8 found][u32 ttl][value]; a miss is one zero byte */
	if (rq->len < 1 || (rq->result[0] == 1 && rq->len < 5)) {
		LM_ERR("malformed get reply (%zu bytes)\n", rq->len);
		rq->failed = 1;
		perfd_areq_done(rq);
		return -1;
	}
	if (rq->result[0] != 1) {
		perfd_areq_done(rq);
		return -2;
	}
	memset(&val, 0, sizeof val);
	val.flags = PV_VAL_STR;
	val.rs.s = rq->result + 5;
	val.rs.len = (int)(rq->len - 5);
	if (pv_set_value(msg, rq->dest, 0, &val) != 0)
		LM_ERR("failed to set the async fetch output variable\n");
	perfd_areq_done(rq);
	return 1;
}

/* reactor or tm's synchronous loop: it waits here, bounded by query_timeout,
 * so it never spins (tm re-calls at once on ASYNC_CONTINUE) */
static int perfd_async_resume(int fd, struct sip_msg *msg, void *param)
{
	perfd_areq *rq = (perfd_areq *)param;

	(void)fd;
	if (perfd_read_ready(rq->slot->pd) < 0)
		rq->failed = 1;
	if (!perfd_rq_settled(rq->slot->pd, rq) &&
	    perfd_drive(rq->slot->pd, rq->deadline_ms, perfd_rq_settled, rq) < 0
	    && !rq->fired) {
		LM_ERR("async fetch: no reply within %d ms (%s)\n", timeout_ms(),
			perfd_error(rq->slot->pd));
		rq->failed = 1;
	}
	return perfd_areq_finish(rq, msg);
}

/* tm's own timeout fired first: the request is abandoned with its handle */
static int perfd_async_timeout(int fd, struct sip_msg *msg, void *param)
{
	perfd_areq *rq = (perfd_areq *)param;

	(void)fd;
	(void)msg;
	LM_ERR("async fetch: no reply within %d ms\n", timeout_ms());
	rq->failed = 1;
	perfd_areq_done(rq);
	return -1;
}

static int perfd_fetch_sync(struct sip_msg *msg, perfd_con *con, str *key,
		pv_spec_t *dest)
{
	pv_value_t val;
	str v;
	int rc = perfd_cdb_get_con(con, key, &v);

	async_status = ASYNC_NO_IO;
	if (rc != 0)
		return rc;                     /* -2 miss: output stays unset */
	memset(&val, 0, sizeof val);
	val.flags = PV_VAL_STR;
	val.rs = v;
	if (pv_set_value(msg, dest, 0, &val) != 0)
		LM_ERR("failed to set output variable\n");
	pkg_free(v.s);
	return 1;
}

/* perfd_fetch(group, key, $var) - the async twin of cache_fetch() */
static int w_perfd_fetch(struct sip_msg *msg, async_ctx *ctx, str *group,
		str *key, pv_spec_t *dest)
{
	perfd_con *con = perfd_con_by_group(group);
	perfd_aslot *slot;
	perfd_areq *rq;
	char kbuf[PERFD_KEY_MAX];

	if (!con) {
		LM_ERR("no perfd cachedb_url with group '%.*s'\n",
			group->len, group->s);
		return -1;
	}
	if (perfd_key(key, kbuf) < 0)
		return -1;

	slot = perfd_async_slot(con, kbuf);
	if (!slot) {
		/* saturated or unreachable: answer synchronously rather than
		 * refuse, and say so - rarely enough to stay readable */
		static time_t shouted;
		time_t now = time(NULL);

		if (now - shouted >= 60) {
			shouted = now;
			LM_WARN("async pool (%d) exhausted or unavailable - "
				"serving this fetch synchronously; raise "
				"async_conns if this repeats\n", async_conns);
		}
		return perfd_fetch_sync(msg, con, key, dest);
	}

	rq = pkg_malloc(sizeof *rq);
	if (!rq) {
		LM_ERR("no pkg memory for an async request\n");
		return -1;
	}
	memset(rq, 0, sizeof *rq);
	rq->slot = slot;
	rq->dest = dest;
	rq->deadline_ms = now_ms() + timeout_ms();

	if (perfd_submit_kv(slot->pd, PERFD_V_GET, con->col, kbuf, NULL, 0,
	        0, 0, perfd_async_cb, rq) != 0 ||
	    perfd_push(slot->pd) < 0) {
		LM_ERR("async submit failed: %s\n", perfd_error(slot->pd));
		/* free the handle first: it may still hold the callback with rq */
		slot_clear(slot);
		if (rq->result)
			perfd_free_value(rq->result);
		pkg_free(rq);
		return -1;
	}
	slot->busy = 1;

	/* answered inside perfd_push() already: finish inline */
	if (rq->fired) {
		async_status = ASYNC_NO_IO;
		return perfd_areq_finish(rq, msg);
	}
	ASYNC_SET_RESUME_F(ctx, perfd_async_resume);
	ctx->resume_param = rq;
	ctx->timeout_f = perfd_async_timeout;
	ctx->timeout_s = (unsigned int)((timeout_ms() + 999) / 1000);
	async_status = perfd_fd(slot->pd);
	return 1;
}

static const acmd_export_t acmds[] = {
	{"perfd_fetch", (acmd_function)w_perfd_fetch, {
		{CMD_PARAM_STR, 0, 0},         /* cachedb_url group */
		{CMD_PARAM_STR, 0, 0},         /* key */
		{CMD_PARAM_VAR, 0, 0},         /* output variable */
		{0, 0, 0}}},
	{0, 0, {{0, 0, 0}}}
};

static const param_export_t params[] = {
	{"cachedb_url", STR_PARAM|USE_FUNC_PARAM, (void *)&set_connection},
	{"connect_timeout", INT_PARAM, &connect_timeout_ms},
	{"query_timeout", INT_PARAM, &query_timeout_ms},
	{"fleet_refresh", INT_PARAM, &fleet_refresh_ms},
	{"keepalive", INT_PARAM, &keepalive_s},
	{"idle_ping", INT_PARAM, &idle_ping_ms},
	{"async_conns", INT_PARAM, &async_conns},
	{0, 0, 0}
};

struct module_exports exports = {
	"cachedb_perfd",            /* module name */
	MOD_TYPE_CACHEDB,           /* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,            /* dlopen flags */
	0,                          /* load function */
	0,                          /* OpenSIPS module dependencies */
	0,                          /* exported functions */
	acmds,                      /* exported async functions */
	params,                     /* exported parameters */
	0,                          /* exported statistics */
	0,                          /* exported MI functions */
	0,                          /* exported pseudo-variables */
	0,                          /* exported transformations */
	0,                          /* extra processes */
	0,                          /* module pre-initialization function */
	mod_init,                   /* module initialization function */
	(response_function)0,       /* response handling function */
	(destroy_function)mod_destroy, /* destroy function */
	child_init,                 /* per-child init function */
	0                           /* reload confirm function */
};

/* derive the Noise keys of every cachedb_url secret once in the parent, so
 * workers inherit them (Argon2id each); else a worker derives at first dial */
static void perfd_prepare_keys(void)
{
	struct cachedb_url *it;
	struct cachedb_id **ids;
	const char **secrets;
	int n = 0, max = 0, i;

	for (it = perfd_script_urls; it; it = it->next)
		max++;
	if (!max)
		return;
	ids = pkg_malloc((size_t)max * sizeof *ids);
	secrets = pkg_malloc((size_t)(max + 1) * sizeof *secrets);
	if (!ids || !secrets)
		goto out;
	for (it = perfd_script_urls; it; it = it->next) {
		struct cachedb_id *id = new_cachedb_id(&it->url);
		const char *sec;

		if (!id)
			continue;
		sec = id->password ? id->password :
			(id->username && id->username[0] ? id->username : NULL);
		for (i = 0; sec && i < n; i++)
			if (!strcmp(secrets[i], sec))
				sec = NULL;            /* already listed */
		if (!sec) {
			free_cachedb_id(id);
			continue;
		}
		ids[n] = id;
		secrets[n++] = sec;
	}
	secrets[n] = NULL;
	if (n) {
		if (perfd_psk_prepare(secrets) == 0)
			LM_NOTICE("perfd: derived the key of %d secret(s) before "
				"forking - the workers inherit it\n", n);
		else
			LM_WARN("perfd: could not derive a key before forking - "
				"each worker derives its own at its first dial\n");
	}
	for (i = 0; i < n; i++)
		free_cachedb_id(ids[i]);
out:
	if (ids)
		pkg_free(ids);
	if (secrets)
		pkg_free(secrets);
}

/* libperfd's own few log lines (a failed key derivation) into OpenSIPS' log */
static void perfd_log_to_opensips(int level, const char *msg, void *ctx)
{
	(void)ctx;
	if (level <= 2)
		LM_ERR("libperfd: %s", msg);
	else if (level == 3)
		LM_WARN("libperfd: %s", msg);
	else
		LM_DBG("libperfd: %s", msg);
}

static int mod_init(void)
{
	cachedb_engine cde;

	LM_NOTICE("initializing module cachedb_perfd (libperfd %s) ...\n",
		perfd_version());
	if (async_conns < 1 || async_conns > 1024) {
		LM_ERR("async_conns must be 1 to 1024, not %d\n", async_conns);
		return -1;
	}
	if (connect_timeout_ms < 0 || query_timeout_ms < 0) {
		LM_ERR("connect_timeout and query_timeout cannot be negative\n");
		return -1;
	}
	perfd_set_log(perfd_log_to_opensips, NULL);
	memset(&cde, 0, sizeof cde);
	cde.name = cache_mod_name;
	cde.cdb_func.init = perfd_cdb_init;
	cde.cdb_func.destroy = perfd_cdb_destroy;
	cde.cdb_func.get = perfd_cdb_get;
	cde.cdb_func.get_counter = perfd_cdb_get_counter;
	cde.cdb_func.set = perfd_cdb_set;
	cde.cdb_func.remove = perfd_cdb_remove;
	cde.cdb_func.add = perfd_cdb_add;
	cde.cdb_func.sub = perfd_cdb_sub;
	cde.cdb_func.raw_query = perfd_cdb_raw_query;
	cde.cdb_func.capability = CACHEDB_CAP_GET | CACHEDB_CAP_SET |
		CACHEDB_CAP_REMOVE | CACHEDB_CAP_ADD | CACHEDB_CAP_SUB |
		CACHEDB_CAP_BINARY_VALUE | CACHEDB_CAP_RAW;
	if (register_cachedb(&cde) < 0) {
		LM_ERR("cannot register cachedb functions\n");
		return -1;
	}
	perfd_prepare_keys();
	return 0;
}

/* bookkeeping only: the first cache op in the process dials */
static int child_init(int rank)
{
	struct cachedb_url *it;
	cachedb_con *con;

	for (it = perfd_script_urls; it; it = it->next) {
		con = perfd_cdb_init(&it->url);
		if (!con) {
			LM_ERR("failed to open perfd connection\n");
			return -1;
		}
		if (cachedb_put_connection(&cache_mod_name, con) < 0) {
			LM_ERR("failed to insert perfd connection\n");
			return -1;
		}
	}
	cachedb_free_url(perfd_script_urls);
	return 0;
}

static void mod_destroy(void)
{
	LM_NOTICE("destroying module cachedb_perfd ...\n");
	cachedb_end_connections(&cache_mod_name);
}
