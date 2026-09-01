/*
 * cachedb_perfd — the perfcached DAEMON as a cachedb backend (perfd://).
 *
 * This is the remote sibling of cachedb_perf: that module IS the
 * engine, in-process; this one dials a perfcached fleet over the
 * network through libperfd - Noise-encrypted, binary dialect, cluster
 * aware (it learns the fleet from one seed and routes each key to its
 * owner, with the daemon's forward as the safety net for a stale map).
 *
 * URL:  perfd://[:secret@]host:port/collection[?opts]
 *   - the PASSWORD field carries the Noise client secret (like every
 *     other cachedb URL carries its credentials); empty = plaintext,
 *     which perfcached only permits on loopback listeners.
 *   - the port is REQUIRED: perfcached has no registered default, and
 *     a guessed port that half-works is worse than a refusal.
 *   - the database path is the COLLECTION, also required.
 *   - extra options, comma separated: binary=0/1 (default 1),
 *     route=0/1 (default 1), spares=N (default -1 = one per member).
 *
 * FORK SAFETY, the one rule that matters in OpenSIPS's process model:
 * a libperfd handle wraps a TCP+Noise stream and must never be shared
 * across fork().  Connections here are LAZY and pid-stamped - the
 * first operation in any process dials its own; an inherited handle is
 * detected by pid mismatch and discarded (perfd_free only close()s the
 * child's fd copies and writes nothing, so the parent's stream is
 * untouched).
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../ut.h"
#include "../../cachedb/cachedb.h"
#include "../../cachedb/cachedb_id.h"

#include "vendor/lib/perfd.h"

static str cache_mod_name = str_init("perfd");
static struct cachedb_url *perfd_script_urls;

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
	char *col;                     /* the collection, NUL-terminated */
	char *secret;
	int binary, route, spares;
} perfd_con;

static int mod_init(void);
static int child_init(int rank);
static void mod_destroy(void);

/* ---- URL extras ------------------------------------------------------- */

static void parse_extras(perfd_con *con, const char *opts)
{
	const char *p = opts;

	while (p && *p) {
		if (!strncmp(p, "binary=", 7))
			con->binary = p[7] == '1';
		else if (!strncmp(p, "route=", 6))
			con->route = p[6] == '1';
		else if (!strncmp(p, "spares=", 7))
			con->spares = atoi(p + 7);
		p = strchr(p, ',');
		if (p)
			p++;
	}
}

/* ---- the per-process dial --------------------------------------------- */

static int con_ready(perfd_con *con)
{
	perfd_opts o;
	const char *secrets[2];

	if (con->pd && con->pid == getpid())
		return 0;
	if (con->pd) {
		/* inherited across fork: closing only drops this process's
		 * fd copies; nothing is written, the parent is untouched */
		perfd_free(con->pd);
		con->pd = NULL;
	}
	memset(&o, 0, sizeof o);
	if (con->secret && con->secret[0]) {
		secrets[0] = con->secret;
		secrets[1] = NULL;
		o.secrets = secrets;
	}
	o.binary = con->binary;
	o.route_keys = con->route;
	o.spares = con->route ? (con->spares ? con->spares : -1)
	                      : con->spares;
	con->pd = perfd_connect(con->id->host, con->id->port, &o);
	if (!con->pd) {
		LM_ERR("cannot reach perfcached at %s:%d: %s\n",
			con->id->host, con->id->port, perfd_error(NULL));
		return -1;
	}
	con->pid = getpid();
	LM_DBG("connected to %s:%d col=%s (pid %d)\n", con->id->host,
		con->id->port, con->col, (int)con->pid);
	return 0;
}

/* ---- cachedb ops ------------------------------------------------------ */

static void *perfd_new_connection(struct cachedb_id *id)
{
	perfd_con *con;

	if (!id->host || !id->port) {
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
	con->binary = 1;
	con->route = 1;
	con->spares = 0;
	if (id->extra_options)
		parse_extras(con, id->extra_options);
	/* deliberately NOT dialling here: this may run pre-fork, and the
	 * handle must belong to the process that uses it */
	return con;
}

static cachedb_con *perfd_cdb_init(str *url)
{
	return cachedb_do_init(url, (void *)perfd_new_connection);
}

static void perfd_free_connection(cachedb_pool_con *cpc)
{
	perfd_con *con = (perfd_con *)cpc;

	if (con->pd && con->pid == getpid())
		perfd_free(con->pd);
	pkg_free(con);
}

static void perfd_cdb_destroy(cachedb_con *con)
{
	cachedb_do_close(con, perfd_free_connection);
}

#define CON(c) ((perfd_con *)((c)->data))

static int perfd_cdb_get(cachedb_con *cachedb_con, str *attr, str *val)
{
	perfd_con *con = CON(cachedb_con);
	char key[512];
	void *v = NULL;
	size_t vlen = 0;
	long long ttl;
	int rc;

	if (attr->len >= (int)sizeof key)
		return -1;
	memcpy(key, attr->s, attr->len);
	key[attr->len] = 0;
	if (con_ready(con) < 0)
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
		free(v);
		return -1;
	}
	memcpy(val->s, v, vlen);
	val->s[vlen] = 0;
	val->len = (int)vlen;
	free(v);
	return 0;
}

static int perfd_cdb_set(cachedb_con *cachedb_con, str *attr, str *val,
		int expires)
{
	perfd_con *con = CON(cachedb_con);
	char key[512];

	if (attr->len >= (int)sizeof key)
		return -1;
	memcpy(key, attr->s, attr->len);
	key[attr->len] = 0;
	if (con_ready(con) < 0)
		return -1;
	if (perfd_set(con->pd, con->col, key, val->s, (size_t)val->len,
	        expires) != 0) {
		LM_ERR("set %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	return 0;
}

static int perfd_cdb_remove(cachedb_con *cachedb_con, str *attr)
{
	perfd_con *con = CON(cachedb_con);
	char key[512];

	if (attr->len >= (int)sizeof key)
		return -1;
	memcpy(key, attr->s, attr->len);
	key[attr->len] = 0;
	if (con_ready(con) < 0)
		return -1;
	if (perfd_del(con->pd, con->col, key) < 0) {
		LM_ERR("remove %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	return 0;
}

static int perfd_cdb_add(cachedb_con *cachedb_con, str *attr, int val,
		int expires, int *new_val)
{
	perfd_con *con = CON(cachedb_con);
	char key[512];
	long long nv = 0;

	if (attr->len >= (int)sizeof key)
		return -1;
	memcpy(key, attr->s, attr->len);
	key[attr->len] = 0;
	if (con_ready(con) < 0)
		return -1;
	if (perfd_add(con->pd, con->col, key, val, expires, &nv) != 0) {
		LM_ERR("add %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	if (new_val)
		*new_val = (int)nv;
	return 0;
}

static int perfd_cdb_sub(cachedb_con *cachedb_con, str *attr, int val,
		int expires, int *new_val)
{
	perfd_con *con = CON(cachedb_con);
	char key[512];
	long long nv = 0;

	if (attr->len >= (int)sizeof key)
		return -1;
	memcpy(key, attr->s, attr->len);
	key[attr->len] = 0;
	if (con_ready(con) < 0)
		return -1;
	if (perfd_sub(con->pd, con->col, key, val, &nv) != 0) {
		LM_ERR("sub %.*s: %s\n", attr->len, attr->s,
			perfd_error(con->pd));
		return -1;
	}
	/* perfd_sub carries no ttl; honour @expires the way the redis
	 * backend does - a follow-up expire, best effort */
	if (expires > 0)
		perfd_expire(con->pd, con->col, key, expires);
	if (new_val)
		*new_val = (int)nv;
	return 0;
}

static int perfd_cdb_get_counter(cachedb_con *cachedb_con, str *attr,
		int *val)
{
	str sval;
	unsigned int uv;
	int rc, sign = 1;
	str tmp;

	rc = perfd_cdb_get(cachedb_con, attr, &sval);
	if (rc != 0)
		return rc;
	tmp = sval;
	if (tmp.len && tmp.s[0] == '-') {
		sign = -1;
		tmp.s++;
		tmp.len--;
	}
	if (str2int(&tmp, &uv) < 0) {
		LM_ERR("counter %.*s holds a non-numeric value\n",
			attr->len, attr->s);
		pkg_free(sval.s);
		return -1;
	}
	pkg_free(sval.s);
	if (val)
		*val = sign * (int)uv;
	return 0;
}

/* ---- module glue ------------------------------------------------------ */

static const param_export_t params[] = {
	{"cachedb_url", STR_PARAM|USE_FUNC_PARAM, (void *)&set_connection},
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
	0,                          /* exported async functions */
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

static int mod_init(void)
{
	cachedb_engine cde;

	LM_NOTICE("initializing module cachedb_perfd ...\n");
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
	cde.cdb_func.capability = CACHEDB_CAP_GET | CACHEDB_CAP_SET |
		CACHEDB_CAP_REMOVE | CACHEDB_CAP_ADD | CACHEDB_CAP_SUB |
		CACHEDB_CAP_BINARY_VALUE;
	if (register_cachedb(&cde) < 0) {
		LM_ERR("cannot register cachedb functions\n");
		return -1;
	}
	return 0;
}

/* per process: register the script URLs into THIS process's pool.  The
 * handles are lazy (see con_ready), so this is bookkeeping, not I/O -
 * the first cache op in the process dials. */
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
