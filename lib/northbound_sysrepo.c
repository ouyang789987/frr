/*
 * Copyright (C) 2018  NetDEF, Inc.
 *                     Renato Westphal
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "log.h"
#include "command.h"
#include "memory.h"
#include "libfrr.h"
#include "version.h"
#include "northbound.h"

#include <sysrepo.h>
#include <sysrepo/values.h>
#include <sysrepo/xpath.h>

DEFINE_MTYPE_STATIC(LIB, SYSREPO, "Sysrepo module")

static struct thread_master *master;
static struct list *sysrepo_threads;
static sr_session_ctx_t *session;
static sr_conn_ctx_t *connection;

static int frr_sr_read_cb(struct thread *thread);
static int frr_sr_write_cb(struct thread *thread);
static int frr_sr_finish(void);

/* Convert FRR YANG data value to sysrepo YANG data value. */
static int yang_data_frr2sr(struct yang_data *frr_data, sr_val_t *sr_data)
{
	const struct lys_node *snode;
	struct lys_node_container *scontainer;
	struct lys_node_leaf *sleaf;
	struct lys_node_leaflist *sleaflist;
	LY_DATA_TYPE type;

	sr_val_set_xpath(sr_data, frr_data->xpath);

	snode = frr_data->snode;
	switch (snode->nodetype) {
	case LYS_CONTAINER:
		scontainer = (struct lys_node_container *)snode;
		if (!scontainer->presence)
			return -1;
		sr_data->type = SR_CONTAINER_PRESENCE_T;
		return 0;
	case LYS_LIST:
		sr_data->type = SR_LIST_T;
		return 0;
	case LYS_LEAF:
		sleaf = (struct lys_node_leaf *)snode;
		type = sleaf->type.base;
		break;
	case LYS_LEAFLIST:
		sleaflist = (struct lys_node_leaflist *)snode;
		type = sleaflist->type.base;
		break;
	default:
		return -1;
	}

	switch (type) {
	case LY_TYPE_BINARY:
		sr_val_set_str_data(sr_data, SR_BINARY_T, frr_data->value);
		break;
	case LY_TYPE_BITS:
		sr_val_set_str_data(sr_data, SR_BITS_T, frr_data->value);
		break;
	case LY_TYPE_BOOL:
		sr_data->type = SR_BOOL_T;
		sr_data->data.bool_val = yang_str2bool(frr_data->value);
		break;
	case LY_TYPE_DEC64:
		sr_data->type = SR_DECIMAL64_T;
		sr_data->data.decimal64_val = yang_str2dec64(frr_data->value);
		break;
	case LY_TYPE_EMPTY:
		sr_data->type = SR_LEAF_EMPTY_T;
		break;
	case LY_TYPE_ENUM:
		sr_val_set_str_data(sr_data, SR_ENUM_T, frr_data->value);
		break;
	case LY_TYPE_IDENT:
		sr_val_set_str_data(sr_data, SR_IDENTITYREF_T, frr_data->value);
		break;
	case LY_TYPE_INST:
		sr_val_set_str_data(sr_data, SR_INSTANCEID_T, frr_data->value);
		break;
	case LY_TYPE_INT8:
		sr_data->type = SR_INT8_T;
		sr_data->data.int8_val = yang_str2int8(frr_data->value);
		break;
	case LY_TYPE_INT16:
		sr_data->type = SR_INT16_T;
		sr_data->data.int16_val = yang_str2int16(frr_data->value);
		break;
	case LY_TYPE_INT32:
		sr_data->type = SR_INT32_T;
		sr_data->data.int32_val = yang_str2int32(frr_data->value);
		break;
	case LY_TYPE_INT64:
		sr_data->type = SR_INT64_T;
		sr_data->data.int64_val = yang_str2int64(frr_data->value);
		break;
	case LY_TYPE_STRING:
		sr_val_set_str_data(sr_data, SR_STRING_T, frr_data->value);
		break;
	case LY_TYPE_UINT8:
		sr_data->type = SR_UINT8_T;
		sr_data->data.uint8_val = yang_str2uint8(frr_data->value);
		break;
	case LY_TYPE_UINT16:
		sr_data->type = SR_UINT16_T;
		sr_data->data.uint16_val = yang_str2uint16(frr_data->value);
		break;
	case LY_TYPE_UINT32:
		sr_data->type = SR_UINT32_T;
		sr_data->data.uint32_val = yang_str2uint32(frr_data->value);
		break;
	case LY_TYPE_UINT64:
		sr_data->type = SR_UINT64_T;
		sr_data->data.uint64_val = yang_str2uint64(frr_data->value);
		break;
	default:
		return -1;
	}

	return 0;
}

static int frr_sr_process_change(struct lyd_node *config,
				 sr_change_oper_t sr_op, sr_val_t *sr_old_val,
				 sr_val_t *sr_new_val)
{
	struct nb_option *option;
	enum nb_operation nb_op;
	sr_val_t *sr_data;
	const char *xpath;
	char value_str[YANG_VALUE_MAXLEN];
	struct yang_data *data;

	sr_data = sr_new_val ? sr_new_val : sr_old_val;
	assert(sr_data);

	xpath = sr_data->xpath;

	/* Non-presence container - nothing to do. */
	if (sr_data->type == SR_CONTAINER_T)
		return SR_ERR_OK;

	option = nb_option_find(xpath);
	if (!option) {
		zlog_warn("%s: unknown data [xpath %s]", __func__, xpath);
		return SR_ERR_VALIDATION_FAILED;
	}

	/* Map operation values. */
	switch (sr_op) {
	case SR_OP_CREATED:
	case SR_OP_MODIFIED:
		if (nb_operation_is_valid(NB_OP_CREATE, option->snode))
			nb_op = NB_OP_CREATE;
		else if (nb_operation_is_valid(NB_OP_MODIFY, option->snode)) {
			nb_op = NB_OP_MODIFY;
		} else
			/* Ignore list keys modifications. */
			return SR_ERR_OK;
		break;
	case SR_OP_DELETED:
		/*
		 * When a list is deleted or one of its keys is changed, we are
		 * notified about the removal of all of its leafs, even the ones
		 * that are non-optional. We need to ignore these notifications.
		 */
		if (!nb_operation_is_valid(NB_OP_DELETE, option->snode))
			return SR_ERR_OK;

		nb_op = NB_OP_DELETE;
		break;
	case SR_OP_MOVED:
		nb_op = NB_OP_MOVE;
		break;
	default:
		zlog_warn("%s: unexpected operation %u [xpath %s]", __func__,
			  sr_op, xpath);
		return SR_ERR_VALIDATION_FAILED;
	}

	sr_val_to_buff(sr_data, value_str, sizeof(value_str));
	data = yang_data_new(xpath, value_str);

	if (nb_config_edit(config, option, nb_op, xpath, NULL, data) != NB_OK)
		return SR_ERR_VALIDATION_FAILED;

	return SR_ERR_OK;
}

/* Callback for changes in the running configuration. */
static int frr_sr_config_change_cb(sr_session_ctx_t *session,
				   const char *module_name,
				   sr_notif_event_t sr_ev, void *private_ctx)
{
	sr_change_iter_t *it;
	int ret;
	sr_change_oper_t sr_op;
	sr_val_t *sr_old_val, *sr_new_val;
	char xpath[XPATH_MAXLEN];
	struct lyd_node *candidate_transitory;

	if (sr_ev != SR_EV_ENABLED && sr_ev != SR_EV_VERIFY)
		return SR_ERR_OK;

	snprintf(xpath, sizeof(xpath), "/%s:*", module_name);
	ret = sr_get_changes_iter(session, xpath, &it);
	if (ret != SR_ERR_OK) {
		zlog_warn("%s: sr_get_changes_iter() failed for xpath %s",
			  __func__, xpath);
		return ret;
	}

	candidate_transitory = nb_config_dup(candidate_config);

	while ((ret = sr_get_change_next(session, it, &sr_op, &sr_old_val,
					 &sr_new_val))
	       == SR_ERR_OK) {
		ret = frr_sr_process_change(candidate_transitory, sr_op,
					    sr_old_val, sr_new_val);
		sr_free_val(sr_old_val);
		sr_free_val(sr_new_val);
		if (ret != SR_ERR_OK)
			break;
	}

	sr_free_change_iter(it);
	if (ret == SR_ERR_VALIDATION_FAILED) {
		zlog_warn("%s: rejecting changes for xpath '%s'", __func__,
			  xpath);
		nb_config_free(&candidate_transitory);
		return ret;
	}

	/* Update candidate configuration. */
	nb_config_free(&candidate_config);
	candidate_config = nb_config_dup(candidate_transitory);

	/* Commit changes. */
	ret = nb_candidate_commit(candidate_config, NB_CLIENT_SYSREPO, true,
				  NULL);

	/* Map northbound return code to sysrepo return code. */
	switch (ret) {
	case NB_OK:
	case NB_ERR_NO_CHANGES:
		return SR_ERR_OK;
	case NB_ERR_LOCKED:
		return SR_ERR_LOCKED;
	case NB_ERR_RESOURCE:
		return SR_ERR_NOMEM;
	default:
		return SR_ERR_VALIDATION_FAILED;
	}
}

static void frr_sr_state_get_elem(struct list *elements,
				  struct nb_option *option, void *list_entry,
				  const char *xpath)
{
	struct yang_data *data;

	data = option->cbs.get_elem(xpath, list_entry);
	if (data)
		listnode_add(elements, data);
}

static void frr_sr_state_cb_container(struct list *elements, const char *xpath,
				      const struct lys_node *snode)
{
	struct lys_node *child;

	LY_TREE_FOR (snode->child, child) {
		struct nb_option *option = child->priv;
		char xpath_child[XPATH_MAXLEN];

		if (!nb_operation_is_valid(NB_OP_GET_ELEM, child))
			continue;

		snprintf(xpath_child, sizeof(xpath_child), "%s/%s", xpath,
			 child->name);

		frr_sr_state_get_elem(elements, option, NULL, xpath_child);
	}
}

static void frr_sr_state_cb_list_entry(struct list *elements,
				       const char *xpath_list, void *list_entry,
				       struct lys_node *child)
{
	struct nb_option *option = child->priv;
	struct lys_node_leaf *sleaf;
	char xpath_child[XPATH_MAXLEN];

	/* Sysrepo doesn't want to know about list keys. */
	switch (child->nodetype) {
	case LYS_LEAF:
		sleaf = (struct lys_node_leaf *)child;
		if (lys_is_key(sleaf, NULL))
			return;
		break;
	case LYS_LEAFLIST:
		break;
	default:
		return;
	}

	if (!nb_operation_is_valid(NB_OP_GET_ELEM, child))
		return;

	snprintf(xpath_child, sizeof(xpath_child), "%s/%s", xpath_list,
		 child->name);

	frr_sr_state_get_elem(elements, option, list_entry, xpath_child);
}

static void frr_sr_state_cb_list(struct list *elements, const char *xpath,
				 const struct lys_node *snode)
{
	struct nb_option *option = snode->priv;
	struct lys_node_list *slist = (struct lys_node_list *)snode;
	void *next;

	for (next = option->cbs.get_next(NULL); next;
	     next = option->cbs.get_next(next)) {
		struct yang_list_keys keys;
		void *list_entry;
		char xpath_list[XPATH_MAXLEN];
		struct lys_node *child;

		/* Get the list keys. */
		if (option->cbs.get_keys(next, &keys) != NB_OK) {
			zlog_warn("%s: failed to get list keys", __func__);
			continue;
		}

		/* Get list item. */
		list_entry = option->cbs.lookup_entry(&keys);
		if (list_entry == NULL)
			continue;

		/* Append list keys to the XPath. */
		strlcpy(xpath_list, xpath, sizeof(xpath_list));
		for (unsigned int i = 0; i < keys.num; i++) {
			snprintf(xpath_list + strlen(xpath_list),
				 sizeof(xpath_list) - strlen(xpath_list),
				 "[%s='%s']", slist->keys[i]->name,
				 keys.key[i].value);
		}

		/* Loop through list entries. */
		LY_TREE_FOR (snode->child, child) {
			frr_sr_state_cb_list_entry(elements, xpath_list,
						   list_entry, child);
		}
	}
}

/* Callback for state retrieval. */
static int frr_sr_state_cb(const char *xpath, sr_val_t **values,
			   size_t *values_cnt, void *private_ctx)
{
	struct list *elements;
	struct yang_data *data;
	const struct lys_node *snode;
	struct listnode *node;
	sr_val_t *v;
	int ret, count, i = 0;

	/* Find schema node. */
	snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 0);

	elements = yang_data_list_new();

	switch (snode->nodetype) {
	case LYS_CONTAINER:
		frr_sr_state_cb_container(elements, xpath, snode);
		break;
	case LYS_LIST:
		frr_sr_state_cb_list(elements, xpath, snode);
		break;
	default:
		break;
	}
	if (list_isempty(elements))
		goto exit;

	count = listcount(elements);
	ret = sr_new_values(count, &v);
	if (ret != SR_ERR_OK) {
		zlog_warn("%s: sr_new_values(): %s", __func__,
			  sr_strerror(ret));
		goto exit;
	}

	for (ALL_LIST_ELEMENTS_RO(elements, node, data)) {
		if (yang_data_frr2sr(data, &v[i++]) != 0) {
			zlog_warn(
				"%s: failed to convert data to sysrepo format",
				__func__);
		}
	}

	*values = v;
	*values_cnt = count;

	list_delete_and_null(&elements);

	return SR_ERR_OK;

exit:
	list_delete_and_null(&elements);
	*values = NULL;
	values_cnt = 0;

	return SR_ERR_OK;
}

static int frr_sr_config_rpc_cb(const char *xpath, const sr_val_t *sr_input,
				const size_t input_cnt, sr_val_t **sr_output,
				size_t *sr_output_cnt, void *private_ctx)
{
	struct nb_option *option;
	struct list *input;
	struct list *output;
	struct yang_data *data;
	size_t cb_output_cnt;
	int ret = SR_ERR_OK;

	option = nb_option_find(xpath);
	if (!option) {
		zlog_warn("%s: unknown data [xpath %s]", __func__, xpath);
		return SR_ERR_INTERNAL;
	}

	input = yang_data_list_new();
	output = yang_data_list_new();

	/* Process input. */
	for (size_t i = 0; i < input_cnt; i++) {
		char value_str[YANG_VALUE_MAXLEN];

		sr_val_to_buff(&sr_input[i], value_str, sizeof(value_str));

		data = yang_data_new(xpath, value_str);
		listnode_add(input, data);
	}

	/* Execute callback registered for this XPath. */
	if (option->cbs.rpc(xpath, input, output) != NB_OK) {
		ret = SR_ERR_OPERATION_FAILED;
		goto exit;
	}

	/* Process output. */
	if (listcount(output) > 0) {
		sr_val_t *values = NULL;
		struct listnode *node;
		int i = 0;

		cb_output_cnt = listcount(output);
		ret = sr_new_values(cb_output_cnt, &values);
		if (ret != SR_ERR_OK) {
			zlog_warn("%s: sr_new_values(): %s", __func__,
				  sr_strerror(ret));
			goto exit;
		}

		for (ALL_LIST_ELEMENTS_RO(output, node, data)) {
			if (yang_data_frr2sr(data, &values[i++]) != 0) {
				zlog_warn(
					"%s: failed to convert data to Sysrepo format",
					__func__);
				ret = SR_ERR_INTERNAL;
				sr_free_values(values, cb_output_cnt);
				goto exit;
			}
		}

		*sr_output = values;
		*sr_output_cnt = cb_output_cnt;
	}

exit:
	/* Release memory. */
	list_delete_and_null(&input);
	list_delete_and_null(&output);

	return ret;
}

static int frr_sr_notification_send(const char *xpath, struct list *arguments)
{
	sr_val_t *values = NULL;
	size_t values_cnt = 0;
	int ret;

	if (arguments && listcount(arguments) > 0) {
		struct yang_data *data;
		struct listnode *node;
		int i = 0;

		values_cnt = listcount(arguments);
		ret = sr_new_values(values_cnt, &values);
		if (ret != SR_ERR_OK) {
			zlog_warn("%s: sr_new_values(): %s", __func__,
				  sr_strerror(ret));
			goto exit;
		}

		for (ALL_LIST_ELEMENTS_RO(arguments, node, data)) {
			if (yang_data_frr2sr(data, &values[i++]) != 0) {
				zlog_warn(
					"%s: failed to convert data to sysrepo format",
					__func__);
				ret = SR_ERR_INTERNAL;
				sr_free_values(values, values_cnt);
				goto exit;
			}
		}
	}

	ret = sr_event_notif_send(session, xpath, values, values_cnt,
				  SR_EV_NOTIF_DEFAULT);
	if (ret != SR_ERR_OK)
		zlog_warn("%s: sr_event_notif_send() failed for xpath %s",
			  __func__, xpath);

exit:
	/* Release memory. */
	if (arguments)
		list_delete_and_null(&arguments);

	/* Map Sysrepo return code to northbound return code. */
	switch (ret) {
	case SR_ERR_OK:
		return NB_OK;
	default:
		return NB_ERR;
	}
}

/* Code to integrate the sysrepo client into FRR main event loop. */
struct sysrepo_thread {
	struct thread *thread;
	sr_fd_event_t event;
	int fd;
};

static struct sysrepo_thread *frr_sr_fd_lookup(sr_fd_event_t event, int fd)
{
	struct sysrepo_thread *sr_thread;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(sysrepo_threads, node, sr_thread)) {
		if (sr_thread->event == event && sr_thread->fd == fd)
			return sr_thread;
	}

	return NULL;
}

static void frr_sr_fd_add(int event, int fd)
{
	struct sysrepo_thread *sr_thread;

	if (frr_sr_fd_lookup(event, fd) != NULL)
		return;

	sr_thread = XCALLOC(MTYPE_SYSREPO, sizeof(*sr_thread));
	sr_thread->event = event;
	sr_thread->fd = fd;
	listnode_add(sysrepo_threads, sr_thread);

	switch (event) {
	case SR_FD_INPUT_READY:
		thread_add_read(master, frr_sr_read_cb, NULL, fd,
				&sr_thread->thread);
		break;
	case SR_FD_OUTPUT_READY:
		thread_add_write(master, frr_sr_write_cb, NULL, fd,
				 &sr_thread->thread);
		break;
	default:
		return;
	}
}

static void frr_sr_fd_free(struct sysrepo_thread *sr_thread)
{
	THREAD_OFF(sr_thread->thread);
	XFREE(MTYPE_SYSREPO, sr_thread);
}

static void frr_sr_fd_del(int event, int fd)
{
	struct sysrepo_thread *sr_thread;

	sr_thread = frr_sr_fd_lookup(event, fd);
	if (sr_thread == NULL)
		return;

	listnode_delete(sysrepo_threads, sr_thread);
	frr_sr_fd_free(sr_thread);
}

static void frr_sr_fd_update(sr_fd_change_t *fd_change_set,
			     size_t fd_change_set_cnt)
{
	for (size_t i = 0; i < fd_change_set_cnt; i++) {
		int fd = fd_change_set[i].fd;
		int event = fd_change_set[i].events;

		if (event != SR_FD_INPUT_READY && event != SR_FD_OUTPUT_READY)
			continue;

		switch (fd_change_set[i].action) {
		case SR_FD_START_WATCHING:
			frr_sr_fd_add(event, fd);
			break;
		case SR_FD_STOP_WATCHING:
			frr_sr_fd_del(event, fd);
			break;
		default:
			break;
		}
	}
}

static int frr_sr_read_cb(struct thread *thread)
{
	int fd = THREAD_FD(thread);
	sr_fd_change_t *fd_change_set = NULL;
	size_t fd_change_set_cnt = 0;
	int ret;

	ret = sr_fd_event_process(fd, SR_FD_INPUT_READY, &fd_change_set,
				  &fd_change_set_cnt);
	if (ret != SR_ERR_OK) {
		zlog_err("%s: sr_fd_event_process(): %s", __func__,
			 sr_strerror(ret));
		return -1;
	}

	thread = NULL;
	thread_add_read(master, frr_sr_read_cb, NULL, fd, &thread);

	frr_sr_fd_update(fd_change_set, fd_change_set_cnt);
	free(fd_change_set);

	return 0;
}

static int frr_sr_write_cb(struct thread *thread)
{
	int fd = THREAD_FD(thread);
	sr_fd_change_t *fd_change_set = NULL;
	size_t fd_change_set_cnt = 0;
	int ret;

	ret = sr_fd_event_process(fd, SR_FD_OUTPUT_READY, &fd_change_set,
				  &fd_change_set_cnt);
	if (ret != SR_ERR_OK) {
		zlog_err("%s: sr_fd_event_process(): %s", __func__,
			 sr_strerror(ret));
		return -1;
	}

	frr_sr_fd_update(fd_change_set, fd_change_set_cnt);
	free(fd_change_set);

	return 0;
}

static void frr_sr_subscribe_config(struct yang_module *module)
{
	int ret;

	ret = sr_module_change_subscribe(
		session, module->name, frr_sr_config_change_cb, NULL, 0,
		SR_SUBSCR_DEFAULT | SR_SUBSCR_EV_ENABLED,
		&module->sr_subscription);
	if (ret != SR_ERR_OK)
		zlog_err("sr_module_change_subscribe(): %s", sr_strerror(ret));
}

static void frr_sr_subscribe_state(struct yang_module *module)
{
	struct lys_node *root, *next, *snode;

	LY_TREE_FOR (module->info->data, root) {
		LY_TREE_DFS_BEGIN (root, next, snode) {
			struct nb_option *option;
			int ret;

			if (!(snode->flags & LYS_CONFIG_R))
				goto next;
			if (snode->parent
			    && (snode->parent->flags & LYS_CONFIG_R))
				goto next;

			option = snode->priv;
			if (debug_northbound)
				zlog_debug("%s: providing data to '%s'",
					   __func__, option->xpath);

			ret = sr_dp_get_items_subscribe(
				session, option->xpath, frr_sr_state_cb, NULL,
				SR_SUBSCR_CTX_REUSE, &module->sr_subscription);
			if (ret != SR_ERR_OK)
				zlog_err("sr_dp_get_items_subscribe(): %s",
					 sr_strerror(ret));

		next:
			LY_TREE_DFS_END(root, next, snode);
		}
	}
}

static void frr_sr_subscribe_rpc(struct yang_module *module)
{
	struct lys_node *root, *next, *snode;

	LY_TREE_FOR (module->info->data, root) {
		LY_TREE_DFS_BEGIN (root, next, snode) {
			struct nb_option *option;
			int ret;

			if (snode->nodetype != LYS_RPC)
				goto next;

			option = snode->priv;
			if (debug_northbound)
				zlog_debug("%s: providing RPC to '%s'",
					   __func__, option->xpath);

			ret = sr_rpc_subscribe(session, option->xpath,
					       frr_sr_config_rpc_cb, NULL,
					       SR_SUBSCR_CTX_REUSE,
					       &module->sr_subscription);
			if (ret != SR_ERR_OK)
				zlog_err("sr_rpc_subscribe(): %s",
					 sr_strerror(ret));

		next:
			LY_TREE_DFS_END(root, next, snode);
		}
	}
}

static void frr_sr_subscribe_action(struct yang_module *module)
{
	struct lys_node *root, *next, *snode;

	LY_TREE_FOR (module->info->data, root) {
		LY_TREE_DFS_BEGIN (root, next, snode) {
			struct nb_option *option;
			int ret;

			if (snode->nodetype != LYS_ACTION)
				goto next;

			option = snode->priv;
			if (debug_northbound)
				zlog_debug("%s: providing action to '%s'",
					   __func__, option->xpath);

			ret = sr_action_subscribe(session, option->xpath,
						  frr_sr_config_rpc_cb, NULL,
						  SR_SUBSCR_CTX_REUSE,
						  &module->sr_subscription);
			if (ret != SR_ERR_OK)
				zlog_err("sr_action_subscribe(): %s",
					 sr_strerror(ret));

		next:
			LY_TREE_DFS_END(root, next, snode);
		}
	}
}

/* FRR's Sysrepo initialization. */
static int frr_sr_init(const char *program_name)
{
	struct yang_module *module;
	int sysrepo_fd, ret;

	sysrepo_threads = list_new();

	ret = sr_fd_watcher_init(&sysrepo_fd, NULL);
	if (ret != SR_ERR_OK) {
		zlog_err("sr_fd_watcher_init(): %s", sr_strerror(ret));
		goto cleanup;
	}

	/* Connect to Sysrepo. */
	ret = sr_connect(program_name, SR_CONN_DEFAULT, &connection);
	if (ret != SR_ERR_OK) {
		zlog_err("sr_connect(): %s", sr_strerror(ret));
		goto cleanup;
	}

	/* Start session. */
	ret = sr_session_start(connection, SR_DS_RUNNING, SR_SESS_DEFAULT,
			       &session);
	if (ret != SR_ERR_OK) {
		zlog_err("sr_session_start(): %s", sr_strerror(ret));
		goto cleanup;
	}

	/* Perform subscriptions. */
	RB_FOREACH (module, yang_modules, &yang_modules) {
		frr_sr_subscribe_config(module);
		frr_sr_subscribe_state(module);
		frr_sr_subscribe_rpc(module);
		frr_sr_subscribe_action(module);
	}

	hook_register(nb_notification_send, frr_sr_notification_send);

	frr_sr_fd_add(SR_FD_INPUT_READY, sysrepo_fd);

	return 0;

cleanup:
	frr_sr_finish();

	return -1;
}

static int frr_sr_finish(void)
{
	struct yang_module *module;

	RB_FOREACH (module, yang_modules, &yang_modules) {
		if (module->sr_subscription == NULL)
			continue;
		sr_unsubscribe(session, module->sr_subscription);
	}

	if (session)
		sr_session_stop(session);
	if (connection)
		sr_disconnect(connection);

	sysrepo_threads->del = (void (*)(void *))frr_sr_fd_free;
	list_delete_and_null(&sysrepo_threads);
	sr_fd_watcher_cleanup();

	return 0;
}

static int frr_sr_module_late_init(struct thread_master *tm)
{
	master = tm;

	if (frr_sr_init(frr_get_progname()) < 0) {
		zlog_err("failed to initialize the sysrepo module");
		return -1;
	}

	hook_register(frr_fini, frr_sr_finish);

	return 0;
}

static int frr_sr_module_init(void)
{
	hook_register(frr_late_init, frr_sr_module_late_init);

	return 0;
}

FRR_MODULE_SETUP(.name = "frr_sysrepo", .version = FRR_VERSION,
		 .description = "FRR sysrepo integration module",
		 .init = frr_sr_module_init, )
