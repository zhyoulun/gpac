/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017
 *					All rights reserved
 *
 *  This file is part of GPAC / filters sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terfsess of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "filter_session.h"

//helper functions
void gf_void_del(void *p)
{
	gf_free(p);
}
void gf_filterpacket_del(void *p)
{
	GF_FilterPacket *pck=(GF_FilterPacket *)p;
	if (pck->data) gf_free(pck->data);
	gf_free(p);
}

void gf_filter_parse_args(GF_Filter *filter, const char *args);

GF_Filter *gf_filter_new(GF_FilterSession *fsess, const GF_FilterRegister *registry, const char *args)
{
	char szName[200];
	GF_Filter *filter;
	assert(fsess);

	GF_SAFEALLOC(filter, GF_Filter);
	if (!filter) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to alloc filter for %s\n", registry->name));
		return NULL;
	}
	filter->freg = registry;
	filter->session = fsess;

	if (fsess->use_locks) {
		snprintf(szName, 200, "Filter%sPackets", filter->freg->name);
		filter->pcks_mx = gf_mx_new(szName);
		snprintf(szName, 200, "Filter%sProps", filter->freg->name);
		filter->props_mx = gf_mx_new(szName);
	}
	//for now we always use a lock on the filter task lists
	//TODO: this is our only lock in lock-free mode, we need to find a way to avoid this lock
	//maybe using permanent filter task?
	snprintf(szName, 200, "Filter%sTasks", filter->freg->name);
	filter->tasks_mx = gf_mx_new(szName);


	filter->tasks = gf_fq_new(filter->tasks_mx);

	filter->pcks_shared_reservoir = gf_fq_new(filter->pcks_mx);
	filter->pcks_alloc_reservoir = gf_fq_new(filter->pcks_mx);
	filter->pcks_inst_reservoir = gf_fq_new(filter->pcks_mx);

	filter->prop_maps_list_reservoir = gf_fq_new(filter->props_mx);
	filter->prop_maps_reservoir = gf_fq_new(filter->props_mx);
	filter->prop_maps_entry_reservoir = gf_fq_new(filter->props_mx);

	filter->pending_pids = gf_fq_new(NULL);

	filter->blacklisted = gf_list_new();

	gf_list_add(fsess->filters, filter);

	gf_filter_set_name(filter, NULL);

	gf_filter_parse_args(filter, args);

	if (!strcmp(registry->name, "compositor"))
		filter->skip_process_trigger_on_tasks = GF_TRUE;

	if (filter->freg->initialize) {
		GF_Err e = filter->freg->initialize(filter);
		if (e) {
			if (!filter->finalized) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Error %s while instantiating filter %s\n", gf_error_to_string(e), registry->name));
				gf_filter_setup_failure(filter, e);
			}
			return NULL;
		}
	}
	//flush all pending pid init requests
	if (filter->has_pending_pids) {
		filter->has_pending_pids=GF_FALSE;
		while (gf_fq_count(filter->pending_pids)) {
			GF_FilterPid *pid=gf_fq_pop(filter->pending_pids);
			gf_fs_post_task(filter->session, gf_filter_pid_init_task, filter, pid, "pid_init", NULL);
		}
	}

	return filter;
}

static void reset_filter_args(GF_Filter *filter);

//when destroying the filter queue we have to skip tasks marked as notified, since they are also present in the
//session task list
void task_del(void *task)
{
	if (!((GF_FSTask*)task)->notified) gf_free(task);
}

void gf_filter_del(GF_Filter *filter)
{
	assert(filter);

	//delete output pids before the packet reservoir
	while (gf_list_count(filter->output_pids)) {
		GF_FilterPid *pid = gf_list_pop_back(filter->output_pids);
		gf_filter_pid_del(pid);
	}
	gf_list_del(filter->output_pids);

	gf_list_del(filter->blacklisted);
	gf_list_del(filter->input_pids);

	gf_fq_del(filter->tasks, task_del);
	gf_fq_del(filter->pending_pids, NULL);

	reset_filter_args(filter);

	gf_fq_del(filter->pcks_shared_reservoir, gf_void_del);
	gf_fq_del(filter->pcks_inst_reservoir, gf_void_del);
	gf_fq_del(filter->pcks_alloc_reservoir, gf_filterpacket_del);

	gf_fq_del(filter->prop_maps_reservoir, gf_void_del);
	gf_fq_del(filter->prop_maps_list_reservoir, (gf_destruct_fun) gf_list_del);
	gf_fq_del(filter->prop_maps_entry_reservoir, gf_void_del);

	gf_mx_del(filter->pcks_mx);
	gf_mx_del(filter->props_mx);
	gf_mx_del(filter->tasks_mx);

	if (filter->name) gf_free(filter->name);
	if (filter->id) gf_free(filter->id);
	if (filter->source_ids) gf_free(filter->source_ids);
	if (filter->filter_udta) gf_free(filter->filter_udta);
	gf_free(filter);
}


void *gf_filter_get_udta(GF_Filter *filter)
{
	assert(filter);

	return filter->filter_udta;
}

const char * gf_filter_get_name(GF_Filter *filter)
{
	assert(filter);
	return (const char *)filter->name;
}

void gf_filter_set_name(GF_Filter *filter, const char *name)
{
	assert(filter);

	if (filter->name) gf_free(filter->name);
	filter->name = gf_strdup(name ? name : filter->freg->name);
}
void gf_filter_set_id(GF_Filter *filter, const char *ID)
{
	assert(filter);

	if (filter->id) gf_free(filter->id);
	filter->id = ID ? gf_strdup(ID) : NULL;
}
void gf_filter_set_sources(GF_Filter *filter, const char *sources_ID)
{
	assert(filter);

	if (filter->source_ids) gf_free(filter->source_ids);
	filter->source_ids = sources_ID ? gf_strdup(sources_ID) : NULL;
}

void gf_filter_set_arg(GF_Filter *filter, const GF_FilterArgs *a, GF_PropertyValue *argv)
{
	void *ptr = filter->filter_udta + a->offset_in_private;
	Bool res = GF_FALSE;

	switch (argv->type) {
	case GF_PROP_BOOL:
		if (a->offset_in_private + sizeof(Bool) <= filter->freg->private_size) {
			*(Bool *)ptr = argv->value.boolean;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_SINT:
		if (a->offset_in_private + sizeof(s32) <= filter->freg->private_size) {
			*(s32 *)ptr = argv->value.sint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_UINT:
		if (a->offset_in_private + sizeof(u32) <= filter->freg->private_size) {
			*(u32 *)ptr = argv->value.uint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_LSINT:
		if (a->offset_in_private + sizeof(s64) <= filter->freg->private_size) {
			*(s64 *)ptr = argv->value.longsint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_LUINT:
		if (a->offset_in_private + sizeof(u64) <= filter->freg->private_size) {
			*(u64 *)ptr = argv->value.longuint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_FLOAT:
		if (a->offset_in_private + sizeof(Fixed) <= filter->freg->private_size) {
			*(Fixed *)ptr = argv->value.fnumber;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_DOUBLE:
		if (a->offset_in_private + sizeof(Double) <= filter->freg->private_size) {
			*(Double *)ptr = argv->value.number;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_FRACTION:
		if (a->offset_in_private + sizeof(GF_Fraction) <= filter->freg->private_size) {
			*(GF_Fraction *)ptr = argv->value.frac;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_NAME:
	case GF_PROP_STRING:
		if (a->offset_in_private + sizeof(char *) <= filter->freg->private_size) {
			if (*(char **)ptr) gf_free( * (char **)ptr);
			//we don't strdup since we don't free the string at the caller site
			*(char **)ptr = argv->value.string;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_POINTER:
		if (a->offset_in_private + sizeof(void *) <= filter->freg->private_size) {
			*(void **)ptr = argv->value.ptr;
			res = GF_TRUE;
		}
		break;
	default:
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Property type %s not supported for filter argument\n", gf_props_get_type_name(argv->type) ));
		return;
		break;
	}
	if (!res) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to set argument %s: memory offset %d overwrite structure size %f\n", a->arg_name, a->offset_in_private, filter->freg->private_size));
	}
}

void gf_filter_update_arg_task(GF_FSTask *task)
{
	u32 i=0;
	GF_FilterUpdate *arg=task->udta;
	//find arg
	i=0;
	while (task->filter->freg->args) {
		GF_PropertyValue argv;
		const GF_FilterArgs *a = &task->filter->freg->args[i];
		i++;
		if (!a || !a->arg_name) break;

		if (strcmp(a->arg_name, arg->name))
			continue;

		if (!a->updatable) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Argument %s of filter %s is not updatable - ignoring\n", a->arg_name, task->filter->name));
			break;
		}

		argv = gf_props_parse_value(a->arg_type, a->arg_name, arg->val, a->min_max_enum);

		if (argv.type != GF_PROP_FORBIDEN) {
			GF_Err e = task->filter->freg->update_arg(task->filter, arg->name, &argv);
			if (e==GF_OK) {
				gf_filter_set_arg(task->filter, a, &argv);
			} else {
				GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s did not accept opdate of arg %s to value %s: %s\n", task->filter->name, arg->name, arg->val, gf_error_to_string(e) ));
			}
		} else {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to parse argument %s value %s\n", a->arg_name, a->arg_default_val));
		}
		break;
	}
	gf_free(arg->name);
	gf_free(arg->val);
	gf_free(arg);
}


void gf_filter_parse_args(GF_Filter *filter, const char *args)
{
	u32 i=0;
	Bool has_meta_args = GF_FALSE;
	char *szArg=NULL;
	u32 alloc_len=1024;
	if (!filter) return;

	if (!filter->freg->private_size) {
		if (filter->freg->args && filter->freg->args[0].arg_name) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter with arguments but no private stack size, no arg passing\n"));
		}
		return;
	}

	filter->filter_udta = gf_malloc(filter->freg->private_size);
	if (!filter->filter_udta) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to allocate private data stack\n"));
		return;
	}
	memset(filter->filter_udta, 0, filter->freg->private_size);

	//instantiate all others with defauts value
	i=0;
	while (filter->freg->args) {
		GF_PropertyValue argv;
		const GF_FilterArgs *a = &filter->freg->args[i];
		i++;
		if (!a || !a->arg_name) break;
		if (!a->arg_default_val) continue;
		if (a->meta_arg) {
			has_meta_args = GF_TRUE;
			continue;
		}

		argv = gf_props_parse_value(a->arg_type, a->arg_name, a->arg_default_val, a->min_max_enum);

		if (argv.type != GF_PROP_FORBIDEN) {
			if (a->offset_in_private>=0) {
				gf_filter_set_arg(filter, a, &argv);
			} else if (filter->freg->update_arg) {
				filter->freg->update_arg(filter, a->arg_name, &argv);
			}
		} else {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to parse argument %s value %s\n", a->arg_name, a->arg_default_val));
		}
	}

	if (args)
		szArg = gf_malloc(sizeof(char)*1024);

	//parse each arg
	while (args) {
		char *value;
		u32 len;
		Bool found=GF_FALSE;
		//look for our arg separator
		char *sep = strchr(args, ':');

		//watchout for "C:\\"
		while (sep && (sep[1]=='\\')) {
			sep = strchr(sep+1, ':');
		}
		if (sep) len = sep-args;
		else len = strlen(args);

		if (len>=alloc_len) {
			alloc_len = len+1;
			szArg = gf_realloc(szArg, sizeof(char)*len);
		}
		strncpy(szArg, args, len);
		szArg[len]=0;

		value = strchr(szArg, '=');
		if (value) {
			value[0] = 0;
			value++;
		}
		i=0;
		while (filter->freg->args) {
			const GF_FilterArgs *a = &filter->freg->args[i];
			i++;
			if (!a || !a->arg_name) break;

			if (!strcmp(a->arg_name, szArg)) {
				GF_PropertyValue argv;
				found=GF_TRUE;

				argv = gf_props_parse_value(a->meta_arg ? GF_PROP_STRING : a->arg_type, a->arg_name, value, a->min_max_enum);

				if (argv.type != GF_PROP_FORBIDEN) {
					if (a->offset_in_private>=0) {
						gf_filter_set_arg(filter, a, &argv);
					} else if (filter->freg->update_arg) {
						filter->freg->update_arg(filter, a->arg_name, &argv);

						if ((argv.type==GF_PROP_STRING) && argv.value.string)
							gf_free(argv.value.string);
					}
				}
				break;
			}
		}
		if (!found) {
			if (!strcmp("FID", szArg)) {
				gf_filter_set_id(filter, value);
				found = GF_TRUE;
			}
			else if (!strcmp("SID", szArg)) {
				gf_filter_set_sources(filter, value);
				found = GF_TRUE;
			}
			else if (has_meta_args && filter->freg->update_arg) {
				GF_PropertyValue argv = gf_props_parse_value(GF_PROP_STRING, szArg, value, NULL);
				filter->freg->update_arg(filter, szArg, &argv);
				if (argv.value.string) gf_free(argv.value.string);
				found = GF_TRUE;
			}
		}


		if (!found) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Argument \"%s\" not found in filter options, ignoring\n", szArg));
		}
		if (sep) {
			args=sep+1;
		} else {
			args=NULL;
		}
	}
	if (szArg) gf_free(szArg);
}

static void reset_filter_args(GF_Filter *filter)
{
	u32 i;
	//instantiate all others with defauts value
	i=0;
	while (filter->freg->args) {
		GF_PropertyValue argv;
		const GF_FilterArgs *a = &filter->freg->args[i];
		i++;
		if (!a || !a->arg_name) break;

		if (a->arg_type != GF_PROP_FORBIDEN) {
			memset(&argv, 0, sizeof(GF_PropertyValue));
			argv.type = a->arg_type;
			gf_filter_set_arg(filter, a, &argv);
		}
	}
}

void gf_filter_process_task(GF_FSTask *task)
{
	GF_Err e;
	GF_Filter *filter = task->filter;
	assert(task->filter);
	assert(filter->freg);
	assert(filter->freg->process);

	filter->schedule_next_time = 0;
	if (filter->pid_connection_pending) {
		return;
	}
	if (filter->would_block && (filter->would_block == filter->num_output_pids) ) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s blocked, skiping task\n", filter->name));
		filter->nb_tasks_done--;
		return;
	}
	//empty input for this filter, don't call process
	else if (filter->num_input_pids==1 && !filter->pending_packets && !filter->skip_process_trigger_on_tasks) {
		filter->nb_tasks_done--;
		return;
	}
	e = filter->freg->process(filter);

	//flush all pending pid init requests following the call to init
	if (filter->has_pending_pids) {
		filter->has_pending_pids=GF_FALSE;
		while (gf_fq_count(filter->pending_pids)) {
			GF_FilterPid *pid=gf_fq_pop(filter->pending_pids);
			gf_fs_post_task(filter->session, gf_filter_pid_init_task, filter, pid, "pid_init", NULL);
		}
	}

	//source filters, flush data if enough space available. If the sink  returns EOS, don't repost the task
	if (!filter->would_block && !filter->input_pids && filter->output_pids && (e!=GF_EOS)) {
		task->requeue_request = GF_TRUE;
	}

	//last task for filter but pending packets and not blocking, requeue in main scheduler
	else if ((filter->would_block < filter->num_output_pids)
			&& filter->pending_packets
			&& (gf_fq_count(filter->tasks)<=1)
			&& !filter->skip_process_trigger_on_tasks)
		task->requeue_request = GF_TRUE;

	//filter requested a requeue
	else if (filter->schedule_next_time) {
		task->schedule_next_time = filter->schedule_next_time;
		task->requeue_request = GF_TRUE;
	}
}

void gf_filter_send_update(GF_Filter *filter, const char *fid, const char *name, const char *val)
{
	gf_fs_send_update(filter->session, fid, name, val);
}

GF_Filter *gf_filter_clone(GF_Filter *filter)
{
	GF_Filter *new_filter = gf_filter_new(filter->session, filter->freg, filter->orig_args);
	if (!new_filter) return NULL;
	new_filter->cloned_from = filter;

	return new_filter;
}

u32 gf_filter_get_ipid_count(GF_Filter *filter)
{
	return filter->num_input_pids;
}

GF_FilterPid *gf_filter_get_ipid(GF_Filter *filter, u32 idx)
{
	return gf_list_get(filter->input_pids, idx);
}

GF_FilterSession *gf_filter_get_session(GF_Filter *filter)
{
	if (filter) return filter->session;
	return NULL;
}

void gf_filter_post_process_task(GF_Filter *filter)
{
	gf_fs_post_task(filter->session, gf_filter_process_task, filter, NULL, "process", NULL);
}

void gf_filter_ask_rt_reschedule(GF_Filter *filter, u32 us_until_next)
{
	if (!filter->in_process) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter %s request for real-time reschedule but filter is not in process\n", filter->name));
		return;
	}
	if (!us_until_next) return;
	filter->schedule_next_time = 1+us_until_next + gf_sys_clock_high_res();
}

void gf_filter_set_setup_failure_callback(GF_Filter *filter, void (*on_setup_error)(GF_Filter *f, void *on_setup_error_udta, GF_Err e), void *udta)
{
	assert(filter);
	filter->on_setup_error = on_setup_error;
	filter->on_setup_error_udta = udta;
}

struct _gf_filter_setup_failure
{
	GF_Err e;
	GF_Filter *filter;
} filter_setup_failure;

void gf_filter_setup_failure_task(GF_FSTask *task)
{
	s32 res;
	GF_Err e = ((struct _gf_filter_setup_failure *)task->udta)->e;
	GF_Filter *f = ((struct _gf_filter_setup_failure *)task->udta)->filter;
	gf_free(task->udta);

	if (f->on_setup_error)
		f->on_setup_error(f, f->on_setup_error_udta, e);

	if (f->freg->finalize)
		f->freg->finalize(f);

	res = gf_list_del_item(f->session->filters, f);
	assert (res >=0 );

	gf_filter_del(f);
}

void gf_filter_setup_failure(GF_Filter *filter, GF_Err reason)
{
	struct _gf_filter_setup_failure *stack;
	//don't accept twice a notif
	if (filter->finalized) return;
	filter->finalized = GF_TRUE;

	stack = gf_malloc(sizeof(struct _gf_filter_setup_failure));
	stack->e = reason;
	stack->filter = filter;

	//setup failure may happen after an initialize, potentially in another thread - post a task
	gf_fs_post_task(filter->session, gf_filter_setup_failure_task, NULL, NULL, "process", stack);
}

void gf_filter_post_task(GF_Filter *filter, gf_fs_task_callback task_fun, void *udta, const char *task_name)
{
	gf_fs_post_task(filter->session, task_fun, filter, NULL, task_name, udta);
}

