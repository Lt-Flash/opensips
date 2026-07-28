/**
 * Topology Hiding Module
 *
 * Copyright (C) 2015 OpenSIPS Foundation
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History
 * -------
 *  2015-02-17  initial version (Vlad Paiu)
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


#include "topo_hiding_logic.h"
#include "th_store.h"
#include "../../async.h"

struct tm_binds tm_api;
struct dlg_binds dlg_api;

int force_dialog = 0;
str topo_hiding_ct_params = {0,0};
int th_loop_protection = 0;
str topo_hiding_ct_hdr_params = {0,0};
str topo_hiding_prefix = str_init("DLGCH_");
str topo_hiding_seed = str_init("OpenSIPS");
str topo_hiding_ct_encode_pw = str_init("ToPoCtPaSS");
str th_contact_encode_param = str_init("thinfo");
str th_contact_encode_scheme = str_init("base64");
str th_contact_caller_var = str_init("_th_contact_caller_username_var_");
str th_contact_callee_var = str_init("_th_contact_callee_username_var_");

int th_ct_enc_scheme;

static int mod_init(void);
static int child_init(int rank);
static void mod_destroy(void);
static int fixup_mmode(void **param);
static int fixup_th_params(void **param);
int w_topology_hiding(struct sip_msg *req, str *flags_s, struct th_params *params);
int w_topology_hiding_match(struct sip_msg *req, void *seq_match_mode_val);
int async_w_topology_hiding_match(struct sip_msg *req, async_ctx *actx,
		void *seq_match_mode_val);
static int pv_topo_callee_callid(struct sip_msg *msg, pv_param_t *param, pv_value_t *res);

static const cmd_export_t cmds[]={
	{"topology_hiding",(cmd_function)w_topology_hiding, {
		{CMD_PARAM_STR|CMD_PARAM_OPT,0,0},
		{CMD_PARAM_STR|CMD_PARAM_OPT,fixup_th_params,fixup_free_pkg}, {0,0,0}},
		REQUEST_ROUTE},
	{"topology_hiding_match",(cmd_function)w_topology_hiding_match, {
		{CMD_PARAM_STR|CMD_PARAM_OPT, fixup_mmode, 0}, {0,0,0}},
		REQUEST_ROUTE},
	{0,0,{{0,0,0}},0}
};

static const acmd_export_t acmds[]={
	{"topology_hiding_match", (acmd_function)async_w_topology_hiding_match, {
		{CMD_PARAM_STR|CMD_PARAM_OPT, fixup_mmode, 0}, {0,0,0}}},
	{0,0,{{0,0,0}}}
};

/* Exported parameters */
static const param_export_t params[] = {
	{ "force_dialog",                INT_PARAM, &force_dialog                },
	{ "th_passed_contact_uri_params",STR_PARAM, &topo_hiding_ct_params.s     },
	{ "th_passed_contact_params",    STR_PARAM, &topo_hiding_ct_hdr_params.s },
	{ "th_callid_passwd",            STR_PARAM, &topo_hiding_seed.s          },
	{ "th_callid_prefix",            STR_PARAM, &topo_hiding_prefix.s        },
	{ "th_contact_encode_passwd",    STR_PARAM, &topo_hiding_ct_encode_pw.s  },
	{ "th_contact_encode_param",     STR_PARAM, &th_contact_encode_param.s   },
	{ "th_contact_encode_scheme",    STR_PARAM, &th_contact_encode_scheme.s  },
	{ "th_contact_caller_username_var", STR_PARAM, &th_contact_caller_var.s  },
	{ "th_contact_callee_username_var", STR_PARAM, &th_contact_callee_var.s  },
	{ "th_callid_loop_protection",      INT_PARAM, &th_loop_protection       },
	{ "th_state_url",                STR_PARAM, &th_state_url.s              },
	{ "th_state_ttl",                INT_PARAM, &th_state_ttl                },
	{ "th_state_ttl_short",          INT_PARAM, &th_state_ttl_short          },
	{0, 0, 0}
};

static const pv_export_t pvars[] = {
	{ str_const_init("TH_callee_callid"), 1000,
		pv_topo_callee_callid,0,0, 0, 0, 0},
	{ {0, 0}, 0, 0, 0, 0, 0, 0, 0 }
};

static module_dependency_t *get_deps_dialog(const param_export_t *param)
{
	int force = *(int *)param->param_pointer;

	if (force == 0)
		return NULL;

	return alloc_module_dep(MOD_TYPE_DEFAULT, "dialog", DEP_ABORT);
}

static const dep_export_t deps = {
	{ /* OpenSIPS module dependencies */
		{ MOD_TYPE_DEFAULT, "tm", DEP_ABORT },
		{ MOD_TYPE_DEFAULT, "dialog", DEP_SILENT },
		{ MOD_TYPE_NULL, NULL, 0 },
	},
	{ /* modparam dependencies */
		{ "force_dialog",		get_deps_dialog },
		{ "th_state_url",		get_deps_cachedb_url },
		{ NULL, NULL },
	},
};

struct module_exports exports= {
	"topology_hiding",
	MOD_TYPE_DEFAULT, /* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,  /* dlopen flags */
	0,				  /* load function */
	&deps,            /* OpenSIPS module dependencies */
	cmds,             /* exported functions */
	acmds,            /* exported async functions */
	params,           /* param exports */
	0,                /* exported statistics */
	0,                /* exported MI functions */
	pvars,            /* exported pseudo-variables */
	0,				  /* exported transformations */
	0,                /* extra processes */
	0,                /* module pre-initialization function */
	mod_init,         /* module initialization function */
	(response_function) 0,
	mod_destroy,
	child_init,       /* per-child init function */
	0                 /* reload confirm function */
};

static int mod_init(void)
{
	LM_INFO("initializing...\n");

	/* param handling */
	topo_hiding_prefix.len = strlen(topo_hiding_prefix.s);
	topo_hiding_seed.len = strlen(topo_hiding_seed.s);
	th_contact_encode_param.len = strlen(th_contact_encode_param.s);
	topo_hiding_ct_encode_pw.len = strlen(topo_hiding_ct_encode_pw.s);
	if (topo_hiding_ct_params.s) {
		topo_hiding_ct_params.len = strlen(topo_hiding_ct_params.s);
		topo_parse_passed_ct_params(&topo_hiding_ct_params);
	}
	if (topo_hiding_ct_hdr_params.s) {
		topo_hiding_ct_hdr_params.len = strlen(topo_hiding_ct_hdr_params.s);
		topo_parse_passed_hdr_ct_params(&topo_hiding_ct_hdr_params);
	}
	th_contact_caller_var.len = strlen(th_contact_caller_var.s);
	th_contact_callee_var.len = strlen(th_contact_callee_var.s);
	th_contact_encode_scheme.len = strlen(th_contact_encode_scheme.s);
	if (!str_strcmp(&th_contact_encode_scheme, const_str("base64")))
		th_ct_enc_scheme = ENC_BASE64;
	else if (!str_strcmp(&th_contact_encode_scheme, const_str("base32")))
		th_ct_enc_scheme = ENC_BASE32;
	else {
		LM_ERR("Unsupported value for 'th_contact_encode_scheme' modparam!"
			"Use 'base64' or 'base32'\n");
		goto error;
	}


	if (th_store_init() < 0) {
		LM_ERR("failed to initialize the topology hiding state storage\n");
		goto error;
	}
	/* If the storage backend can fetch a key from another node, bind it -
	 * that is what async(topology_hiding_match(), ...) uses.  Absent is
	 * the normal case and costs nothing. */
	th_store_bind_pull();

	/* loading dependencies */
	if (load_tm_api(&tm_api)!=0) {
		LM_ERR("can't load TM API\n");
		goto error;
	}

	if (load_dlg_api(&dlg_api)!=0) {
		if (force_dialog) {
			LM_ERR("cannot force dialog. dialog module not loaded\n");
			goto error;
		}
	}

	if (register_pre_raw_processing_cb(topo_callid_pre_raw, 
	PRE_RAW_PROCESSING, 0/*no free*/) < 0) {
		LM_ERR("failed to initialize pre raw support\n");
		return -1;
	}

	if (register_post_raw_processing_cb(topo_callid_post_raw,
	POST_RAW_PROCESSING, 0/*no free*/) < 0) {
		LM_ERR("failed to initialize post raw support\n");
		return -1;
	}
	/* restore dialog callbacks when restart */
	if (dlg_api.register_dlgcb && dlg_api.register_dlgcb(NULL,
				DLGCB_LOADED,th_loaded_callback, NULL, NULL) < 0)
			LM_ERR("cannot register callback for dialog loaded - topology "
					"hiding signalling for ongoing calls will be lost after "
					"restart\n");



	return 0;
error:
	return -1;
}

static int child_init(int rank)
{
	if (th_store_child_init() < 0) {
		LM_ERR("failed to connect to the topology hiding state storage\n");
		return -1;
	}

	return 0;
}

static void mod_destroy(void)
{
	th_store_destroy();
}

static int fixup_mmode(void **param)
{
	*param = (void*)(unsigned long)dlg_match_mode_str_to_int((str*)*param);

	return 0;
}

#define DIALOG_TH_PARAMS_SEP '/'

static int fixup_th_params(void **param)
{
	char *p;
	struct th_params *params = NULL;
	str *contacts = (str*)*param;
	str ct, caller;

	if (!contacts)
		return E_BUG;
	trim(contacts);

	if (!contacts->len) {
		*param = NULL;
		return 0;
	}

	params = pkg_malloc(sizeof *params + contacts->len);
	if (!params) {
		LM_ERR("oom for params\n");
		return E_OUT_OF_MEM;
	}
	memset(params, 0, sizeof *params);
	ct = *contacts;

	if (ct.s[0] == DIALOG_TH_PARAMS_SEP) {
		/* search for a second contact */
		ct.s++;
		ct.len--;
		caller = ct;
		p = q_memchr(ct.s, DIALOG_TH_PARAMS_SEP, ct.len);
		if (p) {
			/* we might have a callee as well; caller ends here */
			caller.s = ct.s;
			caller.len = p - ct.s;
			ct.len -= caller.len + 1;
			ct.s = p + 1;
			trim(&ct);
		} else {
			ct.len = 0;
		}

		trim(&caller);
		if (caller.len > 0) {
			params->ct_caller_user.s = (char *)(params + 1);
			params->ct_caller_user.len = caller.len;
			memcpy(params->ct_caller_user.s, caller.s, caller.len);
		}
		if (ct.len) {
			if (caller.len > 0)
				params->ct_callee_user.s = params->ct_caller_user.s + caller.len;
			else
				params->ct_callee_user.s = (char *)(params + 1);
			params->ct_callee_user.len = ct.len;
			memcpy(params->ct_callee_user.s, ct.s, ct.len);
		}
	} else {
		/* the contact is for both - copy it accordingly */
		params->ct_caller_user.s = params->ct_callee_user.s = (char *)(params + 1);
		params->ct_caller_user.len = params->ct_callee_user.len = ct.len;
		memcpy(params->ct_caller_user.s, ct.s, ct.len);
	}

	*param = params;
	return 0;
}

int w_topology_hiding(struct sip_msg *req, str *flags_s, struct th_params *params)
{
	int flags=0;
	char *p;

	if (flags_s)
		for (p=flags_s->s;p<flags_s->s+flags_s->len;p++)
		{
			switch (*p)
			{
				case 'U':
					flags |= TOPOH_KEEP_USER;
					LM_DBG("Will preserve usernames while doing topo hiding\n");
					break;
				case 'C':
					flags |= TOPOH_HIDE_CALLID;
					LM_DBG("Will change callid while doing topo hiding\n");
					break;
				case 'D':
					flags |= TOPOH_DID_IN_USER;
					LM_DBG("Will push DID into contact username\n");
					break;
				case 'a':
					flags |= TOPOH_KEEP_ADV_A;
					LM_DBG("Will store advertised contact for calller\n");
					break;
				case 'A':
					flags |= TOPOH_KEEP_ADV_B;
					LM_DBG("Will store advertised contact for calllee\n");
					break;
				default:
					LM_DBG("unknown topology_hiding flag : [%c] . Skipping\n",*p);
			}
		}

	return topology_hiding(req,flags,params);
}

int w_topology_hiding_match(struct sip_msg *req, void *seq_match_mode_val)
{
	int mm;

	/* copy-paste from w_match_dialog() */
	if (!seq_match_mode_val)
		mm = SEQ_MATCH_DEFAULT;
	else
		mm = (int)(long)seq_match_mode_val;

	if (!dlg_api.match_dialog || dlg_api.match_dialog(req, mm) < 0)
		return topology_hiding_match(req);
	else
		/* we went to the dlg module, which triggered us back, all good */
		return 1;
}

/* ---- asynchronous match ------------------------------------------------
 *
 * Behind a load balancer that does not keep a dialog on one node, the
 * sequential request carrying a hidden Contact can land on a node that
 * never stored the state.  The ordinary match then fails and the call
 * breaks, even though a sibling has exactly what is needed.
 *
 * async(topology_hiding_match(), resume) asks the cluster for it.  The
 * transaction suspends; the process goes back to work; when the answer
 * lands the resume route runs and the match is retried - by then the
 * value is in this node's cache, so the retry is the ordinary path and
 * every rule it enforces still applies.
 *
 * Nothing changes for the common case: a match that succeeds locally
 * never suspends, and a deployment without cross-node fetch behaves
 * exactly as the synchronous function does.
 * ---------------------------------------------------------------------- */

struct th_async_ctx {
	unsigned int handle;
	char         key[TH_KEY_LEN];
	int          klen;
	int          mm;
	int          hinted;             /* asked one node, so a miss is not final */
};

static int th_match_resume(int fd, struct sip_msg *msg, void *param)
{
	struct th_async_ctx *ctx = (struct th_async_ctx *)param;
	str key, blob;
	int rc;

	key.s = ctx->key;
	key.len = ctx->klen;

	/* Collect it either way - this releases the request whether an answer
	 * arrived or the wait timed out. */
	rc = th_store_pull_finish(&key, ctx->handle, &blob);
	if (blob.s)
		pkg_free(blob.s);

	if (rc != 1 && ctx->hinted) {
		/* We asked the one node the key named and came back empty.  That
		 * says nothing about the rest of the cluster - the node may have
		 * restarted, expired the entry, or be a different machine wearing
		 * a reused id - so ask everybody before giving up. */
		int fd2 = -1;
		unsigned int handle2 = 0;

		ctx->hinted = 0;
		if (th_store_pull_start(&key, 0, &fd2, &handle2) == 1) {
			ctx->handle = handle2;
			/* Wait again, for the second request's answer.  The framework
			 * wants a new descriptor as the RETURN value with the status
			 * that goes with it - handing it back any other way leaves the
			 * old one armed against a context already released.  The
			 * second request often reuses the first's slot and therefore
			 * its descriptor, and asking to replace a descriptor with
			 * itself is an error there, so say plainly that we are simply
			 * still waiting. */
			if (fd2 == fd) {
				async_status = ASYNC_CONTINUE;
				return 1;
			}
			async_status = ASYNC_CHANGE_FD;
			return fd2;
		}
	}

	/* Done waiting.  ASYNC_DONE takes the descriptor out of the reactor
	 * without closing it, which is exactly right here: it belongs to the
	 * cache's pool and will be handed to another request, but this
	 * registration must not outlive the context it points at - the next
	 * request to use that slot would otherwise wake a resume that has
	 * already been freed. */
	async_status = ASYNC_DONE;
	shm_free(ctx);

	if (rc != 1) {
		LM_DBG("no node had the topology hiding state - the match fails as "
			"it would have without asking\n");
		return -1;
	}

	/* The value is in this node's cache now, so the ordinary path finds
	 * it: retrying means every rule the synchronous match applies is
	 * applied here too, rather than duplicated. */
	return topology_hiding_match(msg);
}

int async_w_topology_hiding_match(struct sip_msg *req, async_ctx *actx,
		void *seq_match_mode_val)
{
	struct th_async_ctx *ctx;
	str *missed;
	int mm, rc, fd = -1;
	unsigned int handle = 0;

	mm = seq_match_mode_val ? (int)(long)seq_match_mode_val
	                        : SEQ_MATCH_DEFAULT;

	th_store_clear_last_miss();
	if (!dlg_api.match_dialog || dlg_api.match_dialog(req, mm) < 0)
		rc = topology_hiding_match(req);
	else
		rc = 1;                        /* the dialog module answered */

	/* the ordinary outcome, whatever it was - do not suspend for it */
	if (rc > 0 || !th_store_pull_available()) {
		async_status = ASYNC_SYNC;
		return rc;
	}

	missed = th_store_last_miss();
	if (!missed) {
		async_status = ASYNC_SYNC;     /* it failed for some other reason */
		return rc;
	}

	ctx = shm_malloc(sizeof *ctx);
	if (!ctx) {
		LM_ERR("no shm for the asynchronous match\n");
		async_status = ASYNC_SYNC;
		return rc;
	}
	memcpy(ctx->key, missed->s, missed->len);
	ctx->klen = missed->len;
	ctx->mm = mm;

	ctx->hinted = 1;
	switch (th_store_pull_start(missed, 1, &fd, &handle)) {
	case 1:
		ctx->handle = handle;
		async_status = fd;             /* suspend until the answer lands */
		ASYNC_SET_RESUME_F(actx, th_match_resume);
		actx->resume_param = ctx;
		return 1;
	case 0:
		/* the cluster has already said nobody has it - no point waiting */
		LM_DBG("no node holds this topology hiding state\n");
		break;
	default:
		break;
	}
	shm_free(ctx);
	async_status = ASYNC_SYNC;
	return rc;
}

static char *callid_buf=NULL;
static int pv_topo_callee_callid(struct sip_msg *msg, pv_param_t *param, pv_value_t *res)
{
	struct dlg_cell *dlg;
	str callid;

	if(res==NULL)
		return -1;

	if ( (dlg=dlg_api.get_dlg())==NULL || 
	(!dlg_api.is_mod_flag_set(dlg,TOPOH_HIDE_CALLID))) {
		return pv_get_null( msg, param, res);
	}

	callid.s = th_get_encoded_callid(msg, &dlg->legs[DLG_CALLER_LEG].tag, &callid.len);
	if (!callid.s) {
		LM_ERR("could not encode callid\n");
		return pv_get_null( msg, param, res);
	}
	if (callid_buf)
		pkg_free(callid_buf);
	callid_buf = callid.s;

	res->rs.s = callid_buf;
	res->rs.len = callid.len;
	res->flags = PV_VAL_STR;

	return 0;
}
