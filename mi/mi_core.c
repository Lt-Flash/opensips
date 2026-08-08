/*
 * Copyright (C) 2006 Voice Sistem SRL
 * Copyright (C) 2011-2018 OpenSIPS Solutions
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
 *
 *
 */


/*!
 * \file
 * \brief MI :: Core
 * \ingroup mi
 */



#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

#include "../dprint.h"
#include "../globals.h"
#include "../ut.h"
#include "../pt.h"
#include "../net/net_tcp.h"
#include "../mem/mem.h"
#include "../mem/rpm_mem.h"
#ifdef HG_MALLOC
#include "../mem/shm_mem.h"
#include "../mem/hg_malloc.h"
#endif
#include "../cachedb/cachedb.h"
#include "../evi/event_interface.h"
#include "../ipc.h"
#include "../xlog.h"
#include "../cfg_reload.h"
#include "../status_report.h"
#include "mi.h"
#include "mi_trace.h"


static str    up_since_ctime;

static int init_mi_uptime(void)
{
	up_since_ctime.s = (char*)pkg_malloc(26);
	if (up_since_ctime.s==0) {
		LM_ERR("no more pkg mem\n");
		return -1;
	}
	ctime_r(&startup_time, up_since_ctime.s);
	up_since_ctime.len = strlen(up_since_ctime.s)-1;
	return 0;
}

static mi_response_t *mi_uptime(const mi_params_t *params,
							struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	time_t now;
	char buf[26];

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	time(&now);
	ctime_r(&now, buf);
	if (add_mi_string(resp_obj, MI_SSTR("Now"), buf, strlen(buf)-1) < 0)
		goto error;

	if (add_mi_string(resp_obj, MI_SSTR("Up since"),
		up_since_ctime.s, up_since_ctime.len) < 0)
		goto error;

	if (add_mi_string_fmt(resp_obj, MI_SSTR("Up time"), "%lu [sec]",
		(unsigned long)difftime(now, startup_time)) < 0)
		goto error;

	return resp;

error:
	LM_ERR("failed to add mi item\n");
	free_mi_response(resp);
	return 0;
}

static mi_response_t *mi_version(const mi_params_t *params,
							struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (add_mi_string(resp_obj, MI_SSTR("Server"), (char *)SERVER_HDR+8,
		SERVER_HDR_LEN-8) < 0) {
		LM_ERR("failed to add mi item\n");
		free_mi_response(resp);
		return 0;
	}

	return resp;
}

static mi_response_t *mi_version_1(const mi_params_t *params,
							struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (add_mi_string(resp_obj, MI_SSTR("Server"), (char *)SERVER_HDR+8,
		SERVER_HDR_LEN-8) < 0) {
		LM_ERR("failed to add mi item\n");
		free_mi_response(resp);
		return 0;
	}

	if (add_mi_string(resp_obj, MI_SSTR(VERSIONTYPE), MI_SSTR(THISREVISION))<0) {
		LM_ERR("failed to add mi item\n");
		free_mi_response(resp);
		return 0;
	}

	return resp;
}

static mi_response_t *mi_pwd(const mi_params_t *params,
						struct mi_handler *async_hdl)
{
	static int max_len = 0;
	static char *cwd_buf = 0;
	mi_response_t *resp;
	mi_item_t *resp_obj;

	if (cwd_buf==NULL) {
		max_len = pathmax();
		cwd_buf = pkg_malloc(max_len);
		if (cwd_buf==NULL) {
			LM_ERR("no more pkg mem\n");
			return 0;
		}
	}

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (getcwd(cwd_buf, max_len)==0) {
		LM_ERR("getcwd failed = %s\n",strerror(errno));
		goto error;
	}

	if (add_mi_string(resp_obj, MI_SSTR("WD"), cwd_buf, strlen(cwd_buf)) < 0) {
		LM_ERR("failed to mi item\n");
		goto error;
	}

	return resp;

error:
	free_mi_response(resp);
	return 0;
}


static mi_response_t *mi_arg(const mi_params_t *params,
						struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_arr;
	int n;

	resp = init_mi_result_array(&resp_arr);
	if (!resp)
		return 0;

	for ( n=0; n<my_argc ; n++ ) {
		if (add_mi_string(resp_arr, 0, 0, my_argv[n], strlen(my_argv[n])) < 0) {
			LM_ERR("failed to add mi item\n");
			free_mi_response(resp);
			return 0;
		}
	}

	return resp;
}

static mi_response_t *mi_which_cmd(const mi_params_t *params,
		struct mi_handler *async_hdl)
{
	mi_item_t *resp_arr, *cmd_arr;
	mi_response_t *resp;
	struct mi_cmd *cmds;
	struct mi_cmd *cmd;
	str cmd_str;
	int found;
	int size;
	int i, j;

	if (get_mi_string_param(params, "command", &cmd_str.s, &cmd_str.len) < 0)
		return init_mi_param_error();

	resp = init_mi_result_array(&resp_arr);
	if (!resp)
		return 0;

	if (cmd_str.len > 0 && cmd_str.s[cmd_str.len - 1] == ':') {
		found = 0;
		get_mi_cmds(&cmds, &size);
		for (i = 0; i < size; i++) {
			if (cmds[i].name.len < cmd_str.len ||
					memcmp(cmds[i].name.s, cmd_str.s, cmd_str.len) != 0)
				continue;
			found = 1;
			if (add_mi_string(resp_arr, 0, 0,
					cmds[i].name.s, cmds[i].name.len) < 0) {
				LM_ERR("failed to add mi item\n");
				free_mi_response(resp);
				return 0;
			}
		}

		if (found)
			return resp;

		free_mi_response(resp);
		return init_mi_error(404, MI_SSTR("unknown MI command"));
	}

	cmd = lookup_mi_cmd(cmd_str.s, cmd_str.len);
	if (!cmd) {
		free_mi_response(resp);
		return init_mi_error(404, MI_SSTR("unknown MI command"));
	}
	for (i = 0; i < MAX_MI_RECIPES && cmd->recipes[i].cmd; i++) {
		cmd_arr = add_mi_array(resp_arr, NULL, 0);
		if (! cmd_arr) {
			LM_ERR("failed to add mi array\n");
			free_mi_response(resp);
			return 0;
		}
		for (j = 0; j < MAX_MI_PARAMS && cmd->recipes[i].params[j]; j++) {
			if (add_mi_string(cmd_arr, 0, 0,
					cmd->recipes[i].params[j],
					strlen(cmd->recipes[i].params[j])) < 0) {
				LM_ERR("failed to add mi item\n");
				free_mi_response(resp);
				return 0;
			}
		}
	}

	return resp;
}

static mi_response_t *mi_which(const mi_params_t *params, struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_arr;
	struct mi_cmd  *cmds;
	int size;
	int i;

	resp = init_mi_result_array(&resp_arr);
	if (!resp)
		return 0;

	get_mi_cmds( &cmds, &size);
	for ( i=0 ; i<size ; i++ ) {
		if (add_mi_string(resp_arr, 0, 0,
			cmds[i].name.s, cmds[i].name.len) < 0) {
			LM_ERR("failed to add mi item\n");
			free_mi_response(resp);
			return 0;
		}
	}

	return resp;
}


static mi_response_t *mi_ps(const mi_params_t *params,
						struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	mi_item_t *procs_arr, *proc_item;
	int i;

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	procs_arr = add_mi_array(resp_obj, MI_SSTR("Processes"));
	if (!procs_arr) {
		free_mi_response(resp);
		return 0;
	}

	for ( i=0 ; i<counted_max_processes ; i++ ) {
		if (!is_process_running(i))
			continue;
		proc_item = add_mi_object(procs_arr, 0, 0);
		if (!proc_item)
			goto error;

		if (add_mi_number(proc_item, MI_SSTR("ID"), i) < 0)
			goto error;

		if (add_mi_number(proc_item, MI_SSTR("PID"), pt[i].pid) < 0)
			goto error;

		if (add_mi_string(proc_item, MI_SSTR("Type"),
			pt[i].desc, strlen(pt[i].desc)) < 0)
			goto error;

		/* -1 = never pinned (no pin_workers / no matching group) */
		if (add_mi_number(proc_item, MI_SSTR("PinnedCPU"),
			pt[i].pinned_cpu) < 0)
			goto error;
	}

	return resp;

error:
	LM_ERR("failed to add mi item\n");
	free_mi_response(resp);
	return 0;
}


static mi_response_t *mi_kill(const mi_params_t *params,
							struct mi_handler *async_hdl)
{
	kill(0, SIGTERM);

	return init_mi_result_ok();
}


mi_response_t *mi_log_level(const mi_params_t *params, pid_t pid)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	int i;
	int new_level;

	if (get_mi_int_param(params, "level", &new_level) < 0)
		return init_mi_param_error();

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (pid) {
		if (add_mi_number(resp_obj, MI_SSTR("Log level"), new_level) < 0)
			goto error;
	} else {
		if (add_mi_number(resp_obj, MI_SSTR("New global log level"), new_level) < 0)
			goto error;
	}

	if (pid) {
		/* convert pid to OpenSIPS id */
		i = get_process_ID_by_PID(pid);
		if (i == -1) {
			free_mi_response(resp);
			return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
				MI_SSTR(JSONRPC_INVAL_PARAMS_MSG), MI_SSTR("Bad PID"));
		}

		__set_proc_default_log_level(i, new_level);
		__set_proc_log_level(i, new_level);
	} else
		set_global_log_level(new_level);

	return resp;

error:
	free_mi_response(resp);
	return 0;
}

static mi_response_t *w_log_level(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	mi_item_t *procs_arr, *proc_item;
	int i;

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	procs_arr = add_mi_array(resp_obj, MI_SSTR("Processes"));
	if (!procs_arr) {
		free_mi_response(resp);
		return 0;
	}

	for (i = 0; i < counted_max_processes; i++) {
		if (!is_process_running(i))
			continue;
		proc_item = add_mi_object(procs_arr, NULL, 0);
		if (!proc_item)
			goto error;

		if (add_mi_number(proc_item, MI_SSTR("PID"), pt[i].pid) < 0)
			goto error;

		if (add_mi_number(proc_item, MI_SSTR("Log level"), pt[i].log_level) < 0)
			goto error;

		if (add_mi_string(proc_item, MI_SSTR("Type"),
			pt[i].desc, strlen(pt[i].desc)) < 0)
			goto error;
	}

	return resp;

error:
	free_mi_response(resp);
	return 0;
}

static mi_response_t *w_log_level_1(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	return mi_log_level(params, 0);
}

static mi_response_t *w_log_level_2(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	int pid;

	if (get_mi_int_param(params, "pid", &pid) < 0)
		return init_mi_param_error();

	return mi_log_level(params, pid);
}

static int mi_add_profiling_proc_item(mi_item_t *procs_arr, int i)
{
	mi_item_t *proc_item;

	proc_item = add_mi_object(procs_arr, NULL, 0);
	if (!proc_item)
		return -1;

	if (add_mi_number(proc_item, MI_SSTR("ID"), i) < 0)
		return -1;

	if (add_mi_number(proc_item, MI_SSTR("PID"), pt[i].pid) < 0)
		return -1;

	if (add_mi_number(proc_item, MI_SSTR("Profiling level"),
		pt[i].profiling_proc_level) < 0)
		return -1;

	if (add_mi_string(proc_item, MI_SSTR("Type"),
		pt[i].desc, strlen(pt[i].desc)) < 0)
		return -1;

	return 0;
}

static mi_response_t *w_profiling_proc(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	int id;
	int pid;
	int level;
	int have_id;
	int have_pid;
	int have_level;
	int target_idx = -1;
	int i;
	mi_response_t *resp;
	mi_item_t *resp_obj;
	mi_item_t *procs_arr;

	have_id = (try_get_mi_int_param(params, "id", &id) == 0);
	have_pid = (try_get_mi_int_param(params, "pid", &pid) == 0);
	have_level = (try_get_mi_int_param(params, "level", &level) == 0);

	if (have_id && have_pid)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Only one of 'id' or 'pid' is allowed"));

	if (have_id) {
		if (id < 0 || id >= counted_max_processes)
			return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
				MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
				MI_SSTR("Bad process ID"));
		target_idx = id;
	} else if (have_pid) {
		target_idx = get_process_ID_by_PID(pid);
		if (target_idx < 0)
			return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
				MI_SSTR(JSONRPC_INVAL_PARAMS_MSG), MI_SSTR("Bad PID"));
	}

	if (have_level) {
		if (level < LEVEL_OFF)
			level = LEVEL_OFF;
		else if (level > LEVEL_FULL)
			level = LEVEL_FULL;

		if (target_idx >= 0) {
			pt[target_idx].profiling_proc_level = level;
		} else {
			for (i = 0; i < counted_max_processes; i++)
				pt[i].profiling_proc_level = level;
		}
		return init_mi_result_ok();
	}

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	procs_arr = add_mi_array(resp_obj, MI_SSTR("Processes"));
	if (!procs_arr) {
		free_mi_response(resp);
		return 0;
	}

	if (target_idx >= 0) {
		if (mi_add_profiling_proc_item(procs_arr, target_idx) < 0) {
			free_mi_response(resp);
			return 0;
		}
	} else {
		for (i = 0; i < counted_max_processes; i++)
			if (mi_add_profiling_proc_item(procs_arr, i) < 0) {
				free_mi_response(resp);
				return 0;
			}
	}

	return resp;
}

static mi_response_t *w_xlog_level(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (add_mi_number(resp_obj, MI_SSTR("xLog Level"), *xlog_level) < 0) {
		LM_ERR("failed to add mi item\n");
		free_mi_response(resp);
		return 0;
	}

	return resp;
}


static mi_response_t *w_xlog_level_1(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	int new_level;

	if (get_mi_int_param(params, "level", &new_level) < 0)
		return init_mi_param_error();

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (add_mi_number(resp_obj, MI_SSTR("New xLog level"), new_level) < 0) {
		free_mi_response(resp);
		return 0;
	}

	set_shared_xlog_level(new_level);

	return resp;
}

static mi_response_t *w_log_level_filter_1(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	str consumer;
	int level_filter;

	if (get_mi_string_param(params, "consumer", &consumer.s, &consumer.len) < 0)
		return init_mi_param_error();

	if (get_log_consumer_level_filter(&consumer, &level_filter) < 0)
		return init_mi_error(404, MI_SSTR("Unknown log consumer"));

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (add_mi_number(resp_obj, MI_SSTR("Log level filter"), level_filter) < 0)
		goto error;

	return resp;
error:
	free_mi_response(resp);
	return 0;
}

static mi_response_t *w_log_level_filter_2(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	str consumer;
	int level_filter;

	if (get_mi_string_param(params, "consumer", &consumer.s, &consumer.len) < 0)
		return init_mi_param_error();

	if (get_mi_int_param(params, "level_filter", &level_filter) < 0)
		return init_mi_param_error();

	if (set_log_consumer_level_filter(&consumer, level_filter))
		return init_mi_error(404, MI_SSTR("Unknown log consumer"));

	return init_mi_result_ok();
}

static mi_response_t *w_log_mute_state_1(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	str consumer;
	int mute_state;

	if (get_mi_string_param(params, "consumer", &consumer.s, &consumer.len) < 0)
		return init_mi_param_error();

	if (get_log_consumer_mute_state(&consumer, &mute_state) < 0)
		return init_mi_error(404, MI_SSTR("Unknown log consumer"));

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (add_mi_number(resp_obj, MI_SSTR("mute state"), mute_state) < 0)
		goto error;

	return resp;
error:
	free_mi_response(resp);
	return 0;
}

static mi_response_t *w_log_mute_state_2(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	str consumer;
	int mute_state;

	if (get_mi_string_param(params, "consumer", &consumer.s, &consumer.len) < 0)
		return init_mi_param_error();

	if (get_mi_int_param(params, "mute_state", &mute_state) < 0)
		return init_mi_param_error();

	if (set_log_consumer_mute_state(&consumer, mute_state))
		return init_mi_error(404, MI_SSTR("Unknown log consumer"));

	return init_mi_result_ok();
}

static mi_response_t *mi_cachestore(const 	mi_params_t *params, unsigned int expire)
{
	str mc_system;
	str attr;
	str value;

	if (get_mi_string_param(params, "system", &mc_system.s, &mc_system.len) < 0)
		return init_mi_param_error();

	if (!mc_system.s || mc_system.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty memory cache id"));

	if (get_mi_string_param(params, "attr", &attr.s, &attr.len) < 0)
		return init_mi_param_error();

	if (!attr.s || attr.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty attribute name"));

	if (get_mi_string_param(params, "value", &value.s, &value.len) < 0)
		return init_mi_param_error();

	if (!value.s || value.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty value"));

	if (cachedb_store(&mc_system, &attr, &value, expire) < 0) {
		LM_ERR("cachedb_store command failed\n");
		return init_mi_error(500, MI_SSTR("Cache store command failed"));
	}

	return init_mi_result_ok();
}

static mi_response_t *w_cachestore(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	return mi_cachestore(params, 0);
}

static mi_response_t *w_cachestore_1(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	int expire;

	if (get_mi_int_param(params, "expire", &expire) < 0)
		return init_mi_param_error();

	if (expire < 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Negative expire value"));

	return mi_cachestore(params, expire);
}


static mi_response_t *mi_cachefetch(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	str mc_system;
	str attr;
	str value;
	int ret;

	if (get_mi_string_param(params, "system", &mc_system.s, &mc_system.len) < 0)
		return init_mi_param_error();

	if (!mc_system.s || mc_system.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty memory cache id"));

	if (get_mi_string_param(params, "attr", &attr.s, &attr.len) < 0)
		return init_mi_param_error();

	if (!attr.s || attr.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty attribute name"));

	ret = cachedb_fetch(&mc_system, &attr, &value);
	if(ret== -1)
	{
		LM_ERR("cachedb_fetch command failed\n");
		return init_mi_error(500, MI_SSTR("Cache fetch command failed"));
	}

	if(ret == -2 || value.s == 0 || value.len == 0)
		return init_mi_error(400, MI_SSTR("Value not found"));

	resp = init_mi_result_object(&resp_obj);
	if (!resp) {
		pkg_free(value.s);
		return 0;
	}

	if (add_mi_string(resp_obj, MI_SSTR("key"), attr.s, attr.len) < 0)
		goto error;

	if (add_mi_string(resp_obj, MI_SSTR("value"), value.s, value.len) < 0)
		goto error;

	pkg_free(value.s);

	return resp;

error:
	pkg_free(value.s);
	free_mi_response(resp);
	return 0;
}


static mi_response_t *mi_cacheremove(const mi_params_t *params,
								struct mi_handler *async_hdl)
{
	str mc_system;
	str attr;

	if (get_mi_string_param(params, "system", &mc_system.s, &mc_system.len) < 0)
		return init_mi_param_error();

	if (!mc_system.s || mc_system.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty memory cache id"));

	if (get_mi_string_param(params, "attr", &attr.s, &attr.len) < 0)
		return init_mi_param_error();

	if (!attr.s || attr.len == 0)
		return init_mi_error_extra(JSONRPC_INVAL_PARAMS_CODE,
			MI_SSTR(JSONRPC_INVAL_PARAMS_MSG),
			MI_SSTR("Empty attribute name"));

	if(cachedb_remove(&mc_system, &attr)< 0)
	{
		LM_ERR("cachedb_remove command failed\n");
		return init_mi_error(500, MI_SSTR("Cache remove command failed"));
	}

	return init_mi_result_ok();
}


/* RPC function send by an MI process to force a pkg mem dump into
 * a certain process
 */
static void rpc_do_pkg_dump(int sender_id, void *llevel)
{
	#ifdef PKG_MALLOC
	int bk;

	bk = memdump;
	if ( llevel!=0)
		memdump = (int)(long)llevel;
	LM_GEN1(memdump, "Memory status (pkg):\n");
	pkg_status();
	memdump = bk;
	#endif

	return;
}

static mi_response_t *mi_mem_pkg_dump(const mi_params_t *params, int llevel)
{
	int i;
	pid_t pid = 0;

	if (get_mi_int_param(params, "pid", &pid) < 0)
		return init_mi_param_error();

	/* convert pid to OpenSIPS id */
	i = get_process_ID_by_PID(pid);
	if (i == -1)
		return init_mi_error(404, MI_SSTR("Process not found"));

	if (IPC_FD_WRITE(i)<=0)
		return init_mi_error(500, MI_SSTR("Process does not support mem dump"));

	if (ipc_send_rpc( i, rpc_do_pkg_dump, (void*)(long)llevel)<0) {
		LM_ERR("failed to trigger pkg dump for process %d\n", i);
		return init_mi_error(500, MI_SSTR("Internal error"));
	}

	return init_mi_result_ok();
}

static mi_response_t *w_mem_pkg_dump_1(const mi_params_t *params,
									struct mi_handler *async_hdl)
{
	return mi_mem_pkg_dump(params, 0);
}

static mi_response_t *w_mem_pkg_dump_2(const mi_params_t *params,
									struct mi_handler *async_hdl)
{
	int llevel;

	if (get_mi_int_param(params, "log_level", &llevel) < 0)
		return init_mi_param_error();

	return mi_mem_pkg_dump(params, llevel);
}


static mi_response_t *mi_mem_shm_dump(int llevel)
{
	int bk;

	bk = memdump;
	if (llevel!=0)
		memdump = llevel;
	LM_GEN1(memdump, "Memory status (shm):\n");
	shm_status();
	memdump = bk;

	return init_mi_result_ok();
}

static mi_response_t *w_mem_shm_dump(const mi_params_t *params,
									struct mi_handler *async_hdl)
{
	return mi_mem_shm_dump(0);
}

static mi_response_t *w_mem_shm_dump_1(const mi_params_t *params,
									struct mi_handler *async_hdl)
{
	int llevel;

	if (get_mi_int_param(params, "log_level", &llevel) < 0)
		return init_mi_param_error();

	return mi_mem_shm_dump(llevel);
}

static mi_response_t *mi_mem_rpm_dump(int llevel)
{
	int bk;

	bk = memdump;
	if (llevel!=0)
		memdump = llevel;
	LM_GEN1(memdump, "Memory status (rpm):\n");
	rpm_status();
	memdump = bk;

	return init_mi_result_ok();
}

static mi_response_t *w_mem_rpm_dump(const mi_params_t *params,
									struct mi_handler *async_hdl)
{
	return mi_mem_rpm_dump(0);
}

static mi_response_t *w_mem_rpm_dump_1(const mi_params_t *params,
									struct mi_handler *async_hdl)
{
	int llevel;

	if (get_mi_int_param(params, "log_level", &llevel) < 0)
		return init_mi_param_error();

	return mi_mem_rpm_dump(llevel);
}

static mi_response_t *w_reload_routes(const mi_params_t *params,
							struct mi_handler *async_hdl)
{
	if (reload_routing_script()==0)
		return init_mi_result_ok();
	return init_mi_error( 500, MI_SSTR("reload failed"));
}



#ifdef HG_MALLOC
/*
 * HG_MALLOC keeps state the shared shmem:/pkgmem: statistics cannot express.
 * Those six figures were designed for a free-list allocator, where freed
 * memory returns to one general pool; HG_MALLOC instead CARVES the arena into
 * fixed size-class chunks that are never given back, so "how much is
 * committed", "how much is live" and "how much can still be handed out" stop
 * being the same question. Rather than overload the shared names further,
 * report the allocator's own view here.
 */
static int hg_stats_one(mi_item_t *parent, char *name, struct hg_block *hb)
{
	mi_item_t *o, *cls_arr, *cls_item;
	struct hg_chunk *ch;
	unsigned int chunks_of[HG_NCLASSES], cell_size_of[HG_NCLASSES];
	unsigned long cells_of[HG_NCLASSES];
	unsigned long carved;
	const char *tier;
	int c;

	if (!hb)
		return 0;

	o = add_mi_object(parent, name, strlen(name));
	if (!o)
		return -1;

	carved = hb->real_used;
	tier = hg_mem_tier_str(hb->tier);

	if (add_mi_string(o, MI_SSTR("tier"), (char *)tier, strlen(tier)) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("total_size"), hb->size) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("pinned_mb"), hb->locked_mb) < 0)
		return -1;

	/* carved: bytes taken from the arena and cut into size-class chunks.
	 * Never returned - this is the figure that only ever grows, and the one
	 * that free_size counts down from. */
	if (add_mi_number(o, MI_SSTR("carved"), carved) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("carved_peak"), hb->max_real_used) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("chunks"), hb->nchunks) < 0)
		return -1;
	/* what shmem:free_size reports: arena never yet carved */
	if (add_mi_number(o, MI_SSTR("free_to_carve"), hb->size - carved) < 0)
		return -1;

	/* live: what is actually handed out right now */
	if (add_mi_number(o, MI_SSTR("live_cell_bytes"), hg_cell_live(hb)) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("live_payload"), hg_used(hb)) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("live_cells"), hg_fragments(hb)) < 0)
		return -1;
	if (add_mi_number(o, MI_SSTR("live_peak"), hb->max_live_used) < 0)
		return -1;

	/* carved but idle: on a private free stack or in the global pool.
	 * Reusable, but ONLY for its own size class - which is why it is not
	 * counted as free_to_carve. */
	if (add_mi_number(o, MI_SSTR("recycled"), hg_slab_recycled(hb)) < 0)
		return -1;

	memset(chunks_of, 0, sizeof chunks_of);
	memset(cell_size_of, 0, sizeof cell_size_of);
	memset(cells_of, 0, sizeof cells_of);
	for (ch = hb->chunks; ch; ch = ch->next) {
		if (ch->cls >= HG_NCLASSES)
			continue;
		chunks_of[ch->cls]++;
		cell_size_of[ch->cls] = ch->cell_size;
		cells_of[ch->cls] += ch->cells;
	}

	cls_arr = add_mi_array(o, MI_SSTR("classes"));
	if (!cls_arr)
		return -1;
	for (c = 0; c < HG_NCLASSES; c++) {
		if (!chunks_of[c])
			continue;
		cls_item = add_mi_object(cls_arr, 0, 0);
		if (!cls_item)
			return -1;
		if (add_mi_number(cls_item, MI_SSTR("cell_size"),
			cell_size_of[c]) < 0)
			return -1;
		if (add_mi_number(cls_item, MI_SSTR("chunks"), chunks_of[c]) < 0)
			return -1;
		if (add_mi_number(cls_item, MI_SSTR("cells"), cells_of[c]) < 0)
			return -1;
	}

	return 0;
}

static mi_response_t *mi_hg_stats(const mi_params_t *params,
						struct mi_handler *async_hdl)
{
	mi_response_t *resp;
	mi_item_t *resp_obj;
	int reported = 0;

	resp = init_mi_result_object(&resp_obj);
	if (!resp)
		return 0;

	if (mem_allocator_shm == MM_HG_MALLOC ||
	    mem_allocator_shm == MM_HG_MALLOC_DBG) {
		if (hg_stats_one(resp_obj, "shm", (struct hg_block *)shm_block) < 0)
			goto error;
		reported++;
	}

	if (mem_allocator_pkg == MM_HG_MALLOC ||
	    mem_allocator_pkg == MM_HG_MALLOC_DBG) {
		/* pkg arenas are per-process: this is the arena of whichever
		 * process answered the command, not a fleet-wide total */
		if (hg_stats_one(resp_obj, "pkg", (struct hg_block *)mem_block) < 0)
			goto error;
		reported++;
	}

	if (!reported) {
		free_mi_response(resp);
		return init_mi_error(400,
			MI_SSTR("HG_MALLOC is not the active allocator"));
	}

	return resp;

error:
	LM_ERR("failed to add mi item\n");
	free_mi_response(resp);
	return 0;
}
#endif /* HG_MALLOC */

static const mi_export_t mi_core_cmds[] = {
	{ "uptime", "prints various time information about OpenSIPS - "
		"when it started to run, for how long it runs", 0, init_mi_uptime, {
		{mi_uptime, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "version", "prints the version string of a runningOpenSIPS", 0, 0, {
		{mi_version, {0}},
		{mi_version_1, {"revision", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "pwd", "prints the working directory of OpenSIPS", 0, 0, {
		{mi_pwd, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
#ifdef HG_MALLOC
	{ "hg_stats", "HG_MALLOC arena internals: how much of the arena is "
		"carved into size-class chunks, how much of that is live versus "
		"recycled, and the per-class chunk breakdown", 0, 0, {
		{mi_hg_stats, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
#endif
	{ "arg", "returns the full list of arguments used at startup", 0, 0, {
		{mi_arg, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "which", "lists all available MI commands", 0, 0, {
		{mi_which, {0}},
		{mi_which_cmd, {"command", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "ps", "lists all processes used by OpenSIPS", 0, 0, {
		{mi_ps, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "kill", "terminates OpenSIPS", 0, 0, {
		{mi_kill, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "log_level", "gets/sets the per process or global log level in OpenSIPS",
		0, 0, {
		{w_log_level, 	{0}},
		{w_log_level_1, {"level", 0}},
		{w_log_level_2, {"level", "pid", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "xlog_level", "gets/sets the per process or global xlog level in OpenSIPS",
		0, 0, {
		{w_xlog_level, 	{0}},
		{w_xlog_level_1, {"level", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "profiling_proc", "get/set profiling by process id, pid or all", 0, 0, {
		{w_profiling_proc, {0}},
		{w_profiling_proc, {"id", 0}},
		{w_profiling_proc, {"pid", 0}},
		{w_profiling_proc, {"level", 0}},
		{w_profiling_proc, {"id", "level", 0}},
		{w_profiling_proc, {"pid", "level", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "log_level_filter", "gets/sets the per consumer log level filter",
		0, 0, {
		{w_log_level_filter_1, {"consumer", 0}},
		{w_log_level_filter_2, {"consumer", "level_filter", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "log_mute_state", "mute/unmute a log consumer",
		0, 0, {
		{w_log_mute_state_1, {"consumer", 0}},
		{w_log_mute_state_2, {"consumer", "mute_state", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "reload_routes", "triggers the script (routes only) reload", 0, 0, {
		{w_reload_routes, {0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{ "help", "prints information about MI commands usage", 0, 0, {
		{w_mi_help, {0}},
		{w_mi_help_1, {"mi_cmd", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{EMPTY_MI_EXPORT}
};

static const mi_export_t mi_tcp_cmds[] = {
	{ "list", "list all ongoing TCP based connections, optionally filtered by proto", 0, 0, {
		{mi_tcp_list_conns, {0}},
		{mi_tcp_list_conns, {"proto", 0}},
		{EMPTY_MI_RECIPE}}, {"list_tcp_conns", 0}
	},
	{ "close", "close a given TCP connection", 0, 0, {
		{mi_tcp_close_conn, {"ipport", 0}},
		{EMPTY_MI_RECIPE}}, {0}
	},
	{EMPTY_MI_EXPORT}
};
static const mi_export_t mi_mem_cmds[] = {
#if defined(Q_MALLOC) && defined(DBG_MALLOC)
	{ "shm_check", "complete scan of the shared memory pool "
		"(if any error is found, OpenSIPS will abort!)", 0, 0, {
		{mi_shm_check, {0}},
		{EMPTY_MI_RECIPE}}, {"mem_shm_check", 0}
	},
#endif
	{ "pkg_dump", "forces a status dump of the pkg memory (per process)", 0, 0, {
		{w_mem_pkg_dump_1, {"pid", 0}},
		{w_mem_pkg_dump_2, {"pid", "log_level", 0}},
		{EMPTY_MI_RECIPE}}, {"mem_pkg_dump", 0}
	},
	{ "shm_dump", "forces a status dump of the shm memory", 0, 0, {
		{w_mem_shm_dump, {0}},
		{w_mem_shm_dump_1, {"log_level", 0}},
		{EMPTY_MI_RECIPE}}, {"mem_shm_dump", 0}
	},
	{ "rpm_dump", "forces a status dump of the restart persistent memory", 0, 0, {
		{w_mem_rpm_dump, {0}},
		{w_mem_rpm_dump_1, {"log_level", 0}},
		{EMPTY_MI_RECIPE}}, {"mem_rpm_dump", 0}
	},
	{EMPTY_MI_EXPORT}
};
static const mi_export_t mi_cache_cmds[] = {
	{ "store", "stores in a cache system a string value", 0, 0, {
		{w_cachestore, {"system", "attr", "value", 0}},
		{w_cachestore_1, {"system", "attr", "value", "expire", 0}},
		{EMPTY_MI_RECIPE}}, {"cache_store", 0}
	},
	{ "fetch", "queries for a cache stored value", 0, 0, {
		{mi_cachefetch, {"system", "attr", 0}},
		{EMPTY_MI_RECIPE}}, {"cache_fetch", 0}
	},
	{ "remove", "removes a record from the cache system", 0, 0, {
		{mi_cacheremove, {"system", "attr", 0}},
		{EMPTY_MI_RECIPE}}, {"cache_remove", 0}
	},
	{EMPTY_MI_EXPORT}
};
static const mi_export_t mi_status_report_cmds[] = {
	{ "get", "gets the status (only) of a 'status-report' "
	"group/identifier", 0, 0, {
		{mi_sr_get_status, {"group",0}},
		{mi_sr_get_status, {"group","identifier",0}},
		{EMPTY_MI_RECIPE}}, {"sr_get_status", 0}
	},
	{ "status", "list the status of all the identifiers in OpenSIPS"
	" or from a certain 'status-report' group", 0, 0, {
		{mi_sr_list_status, {0}},
		{mi_sr_list_status, {"group",0}},
		{EMPTY_MI_RECIPE}}, {"sr_list_status", 0}
	},
	{ "reports", "list the reports produced by some 'status-report' "
	"identifiers / groups" , 0, 0, {
		{mi_sr_list_reports, {0}},
		{mi_sr_list_reports, {"group",0}},
		{mi_sr_list_reports, {"group","identifier",0}},
		{EMPTY_MI_RECIPE}}, {"sr_list_reports", 0}
	},
	{ "identifiers", "list the identifiers from a group or all",
	0, 0, {
		{mi_sr_list_identifiers, {0}},
		{mi_sr_list_identifiers, {"group",0}},
		{EMPTY_MI_RECIPE}}, {"sr_list_identifiers", 0}
	},
	{EMPTY_MI_EXPORT}
};
static const mi_export_t mi_evi_cmds[] = {
	{ "subscribe", "subscribes an event to the Event Interface", 0, 0, {
		{w_mi_event_subscribe, {"event", "socket", 0}},
		{w_mi_event_subscribe_1, {"event", "socket", "expire", 0}},
		{EMPTY_MI_RECIPE}}, {"event_subscribe", 0}
	},
	{ "list", "lists all the events advertised through the "
		"Event Interface", 0, 0, {
		{mi_events_list, {0}},
		{EMPTY_MI_RECIPE}}, {"events_list", 0}
	},
	{ "subscribers", "lists all the Event Interface subscribers; "
		"Params: [ event [ subscriber ]]", 0, 0, {
		{w_mi_subscribers_list, {0}},
		{w_mi_subscribers_list_1, {"event", 0}},
		{w_mi_subscribers_list_2, {"event", "socket", 0}},
		{EMPTY_MI_RECIPE}}, {"subscribers_list", 0}
	},
	{ "raise", "raises an event through the Event Interface; "
		"Params: event [ params ]", 0, 0, {
		{w_mi_raise_event, {"event", 0}},
		{w_mi_raise_event, {"event", "params", 0}},
		{EMPTY_MI_RECIPE}}, {"raise_event", 0}
	},
	{EMPTY_MI_EXPORT}
};



int init_mi_core(void)
{
	if (register_mi_mod( "core", mi_core_cmds)<0) {
		LM_ERR("unable to register core MI cmds\n");
		return -1;
	}
	if (register_mi_mod( "tcp", mi_tcp_cmds)<0) {
		LM_ERR("unable to register tcp MI cmds\n");
		return -1;
	}
	if (register_mi_mod( "mem", mi_mem_cmds)<0) {
		LM_ERR("unable to register mem MI cmds\n");
		return -1;
	}
	if (register_mi_mod( "cache", mi_cache_cmds)<0) {
		LM_ERR("unable to register cache MI cmds\n");
		return -1;
	}
	if (register_mi_mod( "status_report", mi_status_report_cmds)<0) {
		LM_ERR("unable to register status_report MI cmds\n");
		return -1;
	}
	if (register_mi_mod( "evi", mi_evi_cmds)<0) {
		LM_ERR("unable to register evi MI cmds\n");
		return -1;
	}

	try_load_trace_api();

	return 0;
}
