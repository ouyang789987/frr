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

#include "libfrr.h"
#include "log.h"
#include "lib_errors.h"
#include "command.h"
#include "db.h"
#include "northbound.h"
#include "northbound_cli.h"
#include "northbound_db.h"

DEFINE_MTYPE_STATIC(LIB, NB_NODE, "Northbound Node")
DEFINE_MTYPE_STATIC(LIB, NB_CONFIG, "Northbound Configuration")

/* Running configuration - shouldn't be modified directly. */
struct nb_config *running_config;

/*
 * Global lock used to prevent multiple configuration transactions from
 * happening concurrently.
 */
static bool transaction_in_progress;

static int nb_configuration_callback(const enum nb_event event,
				     struct nb_config_change *change);
static struct nb_transaction *nb_transaction_new(struct nb_config *config,
						 struct nb_config_cbs *changes,
						 enum nb_client client,
						 const char *comment);
static void nb_transaction_free(struct nb_transaction *transaction);
static int nb_transaction_process(enum nb_event event,
				  struct nb_transaction *transaction);
static void nb_transaction_apply_finish(struct nb_transaction *transaction);

static void nb_node_new_cb(const struct lys_node *snode, void *arg1, void *arg2)
{
	struct nb_node *nb_node;
	struct lys_node *sparent, *sparent_list;

	nb_node = XCALLOC(MTYPE_NB_NODE, sizeof(*nb_node));
	yang_snode_get_path(snode, YANG_PATH_DATA, nb_node->xpath,
			    sizeof(nb_node->xpath));
	nb_node->priority = NB_DFLT_PRIORITY;
	sparent = yang_snode_real_parent(snode);
	if (sparent)
		nb_node->parent = sparent->priv;
	sparent_list = yang_snode_parent_list(snode);
	if (sparent_list)
		nb_node->parent_list = sparent_list->priv;

	/*
	 * Link the northbound node and the libyang schema node with one
	 * another.
	 */
	nb_node->snode = snode;
	lys_set_private(snode, nb_node);
}

static void nb_node_del_cb(const struct lys_node *snode, void *arg1, void *arg2)
{
	struct nb_node *nb_node;

	nb_node = snode->priv;
	lys_set_private(snode, NULL);
	XFREE(MTYPE_NB_NODE, nb_node);
}

struct nb_node *nb_node_find(const char *xpath)
{
	const struct lys_node *snode;

	/*
	 * Use libyang to find the schema node associated to the xpath and get
	 * the northbound node from there (snode private pointer).
	 */
	snode = ly_ctx_get_node(ly_native_ctx, NULL, xpath, 0);
	if (!snode)
		return NULL;

	return snode->priv;
}

static int nb_node_validate_cb(const struct nb_node *nb_node,
			       enum nb_operation operation,
			       int callback_implemented, bool optional)
{
	bool valid;

	valid = nb_operation_is_valid(operation, nb_node->snode);

	if (!valid && callback_implemented)
		flog_warn(EC_LIB_NB_CB_UNNEEDED,
			  "unneeded '%s' callback for '%s'",
			  nb_operation_name(operation), nb_node->xpath);

	if (!optional && valid && !callback_implemented) {
		flog_err(EC_LIB_NB_CB_MISSING, "missing '%s' callback for '%s'",
			 nb_operation_name(operation), nb_node->xpath);
		return 1;
	}

	return 0;
}

/*
 * Check if the required callbacks were implemented for the given northbound
 * node.
 */
static unsigned int nb_node_validate_cbs(const struct nb_node *nb_node)

{
	unsigned int error = 0;

	error += nb_node_validate_cb(nb_node, NB_OP_CREATE,
				     !!nb_node->cbs.create, false);
	error += nb_node_validate_cb(nb_node, NB_OP_MODIFY,
				     !!nb_node->cbs.modify, false);
	error += nb_node_validate_cb(nb_node, NB_OP_DELETE,
				     !!nb_node->cbs.delete, false);
	error += nb_node_validate_cb(nb_node, NB_OP_MOVE, !!nb_node->cbs.move,
				     false);
	error += nb_node_validate_cb(nb_node, NB_OP_APPLY_FINISH,
				     !!nb_node->cbs.apply_finish, true);
	error += nb_node_validate_cb(nb_node, NB_OP_GET_ELEM,
				     !!nb_node->cbs.get_elem, false);
	error += nb_node_validate_cb(nb_node, NB_OP_GET_NEXT,
				     !!nb_node->cbs.get_next, false);
	error += nb_node_validate_cb(nb_node, NB_OP_GET_KEYS,
				     !!nb_node->cbs.get_keys, false);
	error += nb_node_validate_cb(nb_node, NB_OP_LOOKUP_ENTRY,
				     !!nb_node->cbs.lookup_entry, false);
	error += nb_node_validate_cb(nb_node, NB_OP_RPC, !!nb_node->cbs.rpc,
				     false);

	return error;
}

static unsigned int nb_node_validate_priority(const struct nb_node *nb_node)
{
	/* Top-level nodes can have any priority. */
	if (!nb_node->parent)
		return 0;

	if (nb_node->priority < nb_node->parent->priority) {
		flog_err(EC_LIB_NB_CB_INVALID_PRIO,
			 "node has higher priority than its parent [xpath %s]",
			 nb_node->xpath);
		return 1;
	}

	return 0;
}

static void nb_node_validate(const struct lys_node *snode, void *arg1,
			     void *arg2)
{
	struct nb_node *nb_node = snode->priv;
	unsigned int *errors = arg1;

	/* Validate callbacks and priority. */
	*errors += nb_node_validate_cbs(nb_node);
	*errors += nb_node_validate_priority(nb_node);
}

struct nb_config *nb_config_new(struct lyd_node *dnode)
{
	struct nb_config *config;

	config = XCALLOC(MTYPE_NB_CONFIG, sizeof(*config));
	if (dnode)
		config->dnode = dnode;
	else
		config->dnode = yang_dnode_new(ly_native_ctx, true);
	config->version = 0;

	return config;
}

void nb_config_free(struct nb_config *config)
{
	if (config->dnode)
		yang_dnode_free(config->dnode);
	XFREE(MTYPE_NB_CONFIG, config);
}

struct nb_config *nb_config_dup(const struct nb_config *config)
{
	struct nb_config *dup;

	dup = XCALLOC(MTYPE_NB_CONFIG, sizeof(*dup));
	dup->dnode = yang_dnode_dup(config->dnode);
	dup->version = config->version;

	return dup;
}

int nb_config_merge(struct nb_config *config_dst, struct nb_config *config_src,
		    bool preserve_source)
{
	int ret;

	ret = lyd_merge(config_dst->dnode, config_src->dnode, LYD_OPT_EXPLICIT);
	if (ret != 0)
		flog_warn(EC_LIB_LIBYANG, "%s: lyd_merge() failed", __func__);

	if (!preserve_source)
		nb_config_free(config_src);

	return (ret == 0) ? NB_OK : NB_ERR;
}

void nb_config_replace(struct nb_config *config_dst,
		       struct nb_config *config_src, bool preserve_source)
{
	/* Update version. */
	if (config_src->version != 0)
		config_dst->version = config_src->version;

	/* Update dnode. */
	yang_dnode_free(config_dst->dnode);
	if (preserve_source) {
		config_dst->dnode = yang_dnode_dup(config_src->dnode);
	} else {
		config_dst->dnode = config_src->dnode;
		config_src->dnode = NULL;
		nb_config_free(config_src);
	}
}

/* Generate the nb_config_cbs tree. */
static inline int nb_config_cb_compare(const struct nb_config_cb *a,
				       const struct nb_config_cb *b)
{
	/* Sort by priority first. */
	if (a->nb_node->priority < b->nb_node->priority)
		return -1;
	if (a->nb_node->priority > b->nb_node->priority)
		return 1;

	/*
	 * Use XPath as a tie-breaker. This will naturally sort parent nodes
	 * before their children.
	 */
	return strcmp(a->xpath, b->xpath);
}
RB_GENERATE(nb_config_cbs, nb_config_cb, entry, nb_config_cb_compare);

static void nb_config_diff_add_change(struct nb_config_cbs *changes,
				      enum nb_operation operation,
				      const struct lyd_node *dnode)
{
	struct nb_config_change *change;

	change = XCALLOC(MTYPE_TMP, sizeof(*change));
	change->cb.operation = operation;
	change->cb.nb_node = dnode->schema->priv;
	yang_dnode_get_path(dnode, change->cb.xpath, sizeof(change->cb.xpath));
	change->cb.dnode = dnode;

	RB_INSERT(nb_config_cbs, changes, &change->cb);
}

static void nb_config_diff_del_changes(struct nb_config_cbs *changes)
{
	while (!RB_EMPTY(nb_config_cbs, changes)) {
		struct nb_config_change *change;

		change = (struct nb_config_change *)RB_ROOT(nb_config_cbs,
							    changes);
		RB_REMOVE(nb_config_cbs, changes, &change->cb);
		XFREE(MTYPE_TMP, change);
	}
}

/*
 * Helper function used when calculating the delta between two different
 * configurations. Given a new subtree, calculate all new YANG data nodes,
 * excluding default leafs and leaf-lists. This is a recursive function.
 */
static void nb_config_diff_new_subtree(const struct lyd_node *dnode,
				       struct nb_config_cbs *changes)
{
	struct lyd_node *child;

	LY_TREE_FOR (dnode->child, child) {
		enum nb_operation operation;

		switch (child->schema->nodetype) {
		case LYS_LEAF:
		case LYS_LEAFLIST:
			if (lyd_wd_default((struct lyd_node_leaf_list *)child))
				break;

			if (nb_operation_is_valid(NB_OP_CREATE, child->schema))
				operation = NB_OP_CREATE;
			else if (nb_operation_is_valid(NB_OP_MODIFY,
						       child->schema))
				operation = NB_OP_MODIFY;
			else
				continue;

			nb_config_diff_add_change(changes, operation, child);
			break;
		case LYS_CONTAINER:
		case LYS_LIST:
			if (nb_operation_is_valid(NB_OP_CREATE, child->schema))
				nb_config_diff_add_change(changes, NB_OP_CREATE,
							  child);
			nb_config_diff_new_subtree(child, changes);
			break;
		default:
			break;
		}
	}
}

/* Calculate the delta between two different configurations. */
static void nb_config_diff(const struct nb_config *config1,
			   const struct nb_config *config2,
			   struct nb_config_cbs *changes)
{
	struct lyd_difflist *diff;

	diff = lyd_diff(config1->dnode, config2->dnode,
			LYD_DIFFOPT_WITHDEFAULTS);
	assert(diff);

	for (int i = 0; diff->type[i] != LYD_DIFF_END; i++) {
		LYD_DIFFTYPE type;
		struct lyd_node *dnode;
		enum nb_operation operation;

		type = diff->type[i];

		switch (type) {
		case LYD_DIFF_CREATED:
			dnode = diff->second[i];

			if (nb_operation_is_valid(NB_OP_CREATE, dnode->schema))
				operation = NB_OP_CREATE;
			else if (nb_operation_is_valid(NB_OP_MODIFY,
						       dnode->schema))
				operation = NB_OP_MODIFY;
			else
				continue;
			break;
		case LYD_DIFF_DELETED:
			dnode = diff->first[i];
			operation = NB_OP_DELETE;
			break;
		case LYD_DIFF_CHANGED:
			dnode = diff->second[i];
			operation = NB_OP_MODIFY;
			break;
		case LYD_DIFF_MOVEDAFTER1:
		case LYD_DIFF_MOVEDAFTER2:
		default:
			continue;
		}

		nb_config_diff_add_change(changes, operation, dnode);

		if (type == LYD_DIFF_CREATED
		    && CHECK_FLAG(dnode->schema->nodetype,
				  LYS_CONTAINER | LYS_LIST))
			nb_config_diff_new_subtree(dnode, changes);
	}

	lyd_free_diff(diff);
}

int nb_candidate_edit(struct nb_config *candidate,
		      const struct nb_node *nb_node,
		      enum nb_operation operation, const char *xpath,
		      const struct yang_data *previous,
		      const struct yang_data *data)
{
	struct lyd_node *dnode;
	char xpath_edit[XPATH_MAXLEN];

	if (!nb_operation_is_valid(operation, nb_node->snode)) {
		flog_warn(EC_LIB_NB_CANDIDATE_EDIT_ERROR,
			  "%s: %s operation not valid for %s", __func__,
			  nb_operation_name(operation), xpath);
		return NB_ERR;
	}

	/* Use special notation for leaf-lists (RFC 6020, section 9.13.5). */
	if (nb_node->snode->nodetype == LYS_LEAFLIST)
		snprintf(xpath_edit, sizeof(xpath_edit), "%s[.='%s']", xpath,
			 data->value);
	else
		strlcpy(xpath_edit, xpath, sizeof(xpath_edit));

	switch (operation) {
	case NB_OP_CREATE:
	case NB_OP_MODIFY:
		ly_errno = 0;
		dnode = lyd_new_path(candidate->dnode, ly_native_ctx,
				     xpath_edit, (void *)data->value, 0,
				     LYD_PATH_OPT_UPDATE);
		if (!dnode && ly_errno) {
			flog_warn(EC_LIB_LIBYANG, "%s: lyd_new_path() failed",
				  __func__);
			return NB_ERR;
		}

		/*
		 * If a new node was created, call lyd_validate() only to create
		 * default child nodes.
		 */
		if (dnode) {
			lyd_schema_sort(dnode, 0);
			lyd_validate(&dnode, LYD_OPT_CONFIG, ly_native_ctx);
		}
		break;
	case NB_OP_DELETE:
		dnode = yang_dnode_get(candidate->dnode, xpath_edit);
		if (!dnode)
			/*
			 * Return a special error code so the caller can choose
			 * whether to ignore it or not.
			 */
			return NB_ERR_NOT_FOUND;
		lyd_free(dnode);
		break;
	case NB_OP_MOVE:
		/* TODO: update configuration. */
		break;
	default:
		flog_warn(EC_LIB_DEVELOPMENT,
			  "%s: unknown operation (%u) [xpath %s]", __func__,
			  operation, xpath_edit);
		return NB_ERR;
	}

	return NB_OK;
}

bool nb_candidate_needs_update(const struct nb_config *candidate)
{
	if (candidate->version < running_config->version)
		return true;

	return false;
}

int nb_candidate_update(struct nb_config *candidate)
{
	struct nb_config *updated_config;

	updated_config = nb_config_dup(running_config);
	if (nb_config_merge(updated_config, candidate, true) != NB_OK)
		return NB_ERR;

	nb_config_replace(candidate, updated_config, false);

	return NB_OK;
}

/*
 * The northbound configuration callbacks use the 'priv' pointer present in the
 * libyang lyd_node structure to store pointers to FRR internal variables
 * associated to YANG lists and presence containers. Before commiting a
 * candidate configuration, we must restore the 'priv' pointers stored in the
 * running configuration since they might be lost while editing the candidate.
 */
static void nb_candidate_restore_priv_pointers(struct nb_config *candidate)
{
	struct lyd_node *root, *next, *dnode_iter;

	LY_TREE_FOR (running_config->dnode, root) {
		LY_TREE_DFS_BEGIN (root, next, dnode_iter) {
			struct lyd_node *dnode_candidate;
			char xpath[XPATH_MAXLEN];

			if (!dnode_iter->priv)
				goto next;

			yang_dnode_get_path(dnode_iter, xpath, sizeof(xpath));
			dnode_candidate =
				yang_dnode_get(candidate->dnode, xpath);
			if (dnode_candidate)
				yang_dnode_set_entry(dnode_candidate,
						     dnode_iter->priv);

		next:
			LY_TREE_DFS_END(root, next, dnode_iter);
		}
	}
}

/*
 * Perform YANG syntactic and semantic validation.
 *
 * WARNING: lyd_validate() can change the configuration as part of the
 * validation process.
 */
static int nb_candidate_validate_yang(struct nb_config *candidate)
{
	if (lyd_validate(&candidate->dnode, LYD_OPT_STRICT | LYD_OPT_CONFIG,
			 ly_native_ctx)
	    != 0)
		return NB_ERR_VALIDATION;

	return NB_OK;
}

/* Perform code-level validation using the northbound callbacks. */
static int nb_candidate_validate_changes(struct nb_config *candidate,
					 struct nb_config_cbs *changes)
{
	struct nb_config_cb *cb;

	nb_candidate_restore_priv_pointers(candidate);
	RB_FOREACH (cb, nb_config_cbs, changes) {
		struct nb_config_change *change = (struct nb_config_change *)cb;
		int ret;

		ret = nb_configuration_callback(NB_EV_VALIDATE, change);
		if (ret != NB_OK)
			return NB_ERR_VALIDATION;
	}

	return NB_OK;
}

int nb_candidate_validate(struct nb_config *candidate)
{
	struct nb_config_cbs changes;
	int ret;

	if (nb_candidate_validate_yang(candidate) != NB_OK)
		return NB_ERR_VALIDATION;

	RB_INIT(nb_config_cbs, &changes);
	nb_config_diff(running_config, candidate, &changes);
	ret = nb_candidate_validate_changes(candidate, &changes);
	nb_config_diff_del_changes(&changes);

	return ret;
}

int nb_candidate_commit_prepare(struct nb_config *candidate,
				enum nb_client client, const char *comment,
				struct nb_transaction **transaction)
{
	struct nb_config_cbs changes;

	if (nb_candidate_validate_yang(candidate) != NB_OK) {
		flog_warn(EC_LIB_NB_CANDIDATE_INVALID,
			  "%s: failed to validate candidate configuration",
			  __func__);
		return NB_ERR_VALIDATION;
	}

	RB_INIT(nb_config_cbs, &changes);
	nb_config_diff(running_config, candidate, &changes);
	if (RB_EMPTY(nb_config_cbs, &changes))
		return NB_ERR_NO_CHANGES;

	if (nb_candidate_validate_changes(candidate, &changes) != NB_OK) {
		flog_warn(EC_LIB_NB_CANDIDATE_INVALID,
			  "%s: failed to validate candidate configuration",
			  __func__);
		nb_config_diff_del_changes(&changes);
		return NB_ERR_VALIDATION;
	}

	*transaction = nb_transaction_new(candidate, &changes, client, comment);
	if (*transaction == NULL) {
		flog_warn(EC_LIB_NB_TRANSACTION_CREATION_FAILED,
			  "%s: failed to create transaction", __func__);
		nb_config_diff_del_changes(&changes);
		return NB_ERR_LOCKED;
	}

	return nb_transaction_process(NB_EV_PREPARE, *transaction);
}

void nb_candidate_commit_abort(struct nb_transaction *transaction)
{
	(void)nb_transaction_process(NB_EV_ABORT, transaction);
	nb_transaction_free(transaction);
}

void nb_candidate_commit_apply(struct nb_transaction *transaction,
			       bool save_transaction, uint32_t *transaction_id)
{
	(void)nb_transaction_process(NB_EV_APPLY, transaction);
	nb_transaction_apply_finish(transaction);

	/* Replace running by candidate. */
	transaction->config->version++;
	nb_config_replace(running_config, transaction->config, true);

	/* Record transaction. */
	if (save_transaction
	    && nb_db_transaction_save(transaction, transaction_id) != NB_OK)
		flog_warn(EC_LIB_NB_TRANSACTION_RECORD_FAILED,
			  "%s: failed to record transaction", __func__);

	nb_transaction_free(transaction);
}

int nb_candidate_commit(struct nb_config *candidate, enum nb_client client,
			bool save_transaction, const char *comment,
			uint32_t *transaction_id)
{
	struct nb_transaction *transaction = NULL;
	int ret;

	ret = nb_candidate_commit_prepare(candidate, client, comment,
					  &transaction);
	/*
	 * Apply the changes if the preparation phase succeeded. Otherwise abort
	 * the transaction.
	 */
	if (ret == NB_OK)
		nb_candidate_commit_apply(transaction, save_transaction,
					  transaction_id);
	else if (transaction != NULL)
		nb_candidate_commit_abort(transaction);

	return ret;
}

static void nb_log_callback(const enum nb_event event,
			    enum nb_operation operation, const char *xpath,
			    const char *value)
{
	zlog_debug(
		"northbound callback: event [%s] op [%s] xpath [%s] value [%s]",
		nb_event_name(event), nb_operation_name(operation), xpath,
		value);
}

/*
 * Call the northbound configuration callback associated to a given
 * configuration change.
 */
static int nb_configuration_callback(const enum nb_event event,
				     struct nb_config_change *change)
{
	enum nb_operation operation = change->cb.operation;
	const char *xpath = change->cb.xpath;
	const struct nb_node *nb_node = change->cb.nb_node;
	const struct lyd_node *dnode = change->cb.dnode;
	union nb_resource *resource;
	int ret = NB_ERR;

	if (debug_northbound) {
		const char *value = "(none)";

		if (dnode && !yang_snode_is_typeless_data(dnode->schema))
			value = yang_dnode_get_string(dnode, NULL);

		nb_log_callback(event, operation, xpath, value);
	}

	if (event == NB_EV_VALIDATE)
		resource = NULL;
	else
		resource = &change->resource;

	switch (operation) {
	case NB_OP_CREATE:
		ret = (*nb_node->cbs.create)(event, dnode, resource);
		break;
	case NB_OP_MODIFY:
		ret = (*nb_node->cbs.modify)(event, dnode, resource);
		break;
	case NB_OP_DELETE:
		ret = (*nb_node->cbs.delete)(event, dnode);
		break;
	case NB_OP_MOVE:
		ret = (*nb_node->cbs.move)(event, dnode);
		break;
	default:
		break;
	}

	if (ret != NB_OK)
		flog_warn(
			EC_LIB_NB_CB_CONFIG,
			"%s: error processing configuration change: error [%s] event [%s] operation [%s] xpath [%s]",
			__func__, nb_err_name(ret), nb_event_name(event),
			nb_operation_name(operation), xpath);

	return ret;
}

static struct nb_transaction *nb_transaction_new(struct nb_config *config,
						 struct nb_config_cbs *changes,
						 enum nb_client client,
						 const char *comment)
{
	struct nb_transaction *transaction;

	if (transaction_in_progress) {
		flog_warn(
			EC_LIB_NB_TRANSACTION_CREATION_FAILED,
			"%s: error - there's already another transaction in progress",
			__func__);
		return NULL;
	}
	transaction_in_progress = true;

	transaction = XCALLOC(MTYPE_TMP, sizeof(*transaction));
	transaction->client = client;
	if (comment)
		strlcpy(transaction->comment, comment,
			sizeof(transaction->comment));
	transaction->config = config;
	transaction->changes = *changes;

	return transaction;
}

static void nb_transaction_free(struct nb_transaction *transaction)
{
	nb_config_diff_del_changes(&transaction->changes);
	XFREE(MTYPE_TMP, transaction);
	transaction_in_progress = false;
}

/* Process all configuration changes associated to a transaction. */
static int nb_transaction_process(enum nb_event event,
				  struct nb_transaction *transaction)
{
	struct nb_config_cb *cb;

	RB_FOREACH (cb, nb_config_cbs, &transaction->changes) {
		struct nb_config_change *change = (struct nb_config_change *)cb;
		int ret;

		/*
		 * Only try to release resources that were allocated
		 * successfully.
		 */
		if (event == NB_EV_ABORT && change->prepare_ok == false)
			break;

		/* Call the appropriate callback. */
		ret = nb_configuration_callback(event, change);
		switch (event) {
		case NB_EV_PREPARE:
			if (ret != NB_OK)
				return ret;
			change->prepare_ok = true;
			break;
		case NB_EV_ABORT:
		case NB_EV_APPLY:
			/*
			 * At this point it's not possible to reject the
			 * transaction anymore, so any failure here can lead to
			 * inconsistencies and should be treated as a bug.
			 * Operations prone to errors, like validations and
			 * resource allocations, should be performed during the
			 * 'prepare' phase.
			 */
			break;
		default:
			break;
		}
	}

	return NB_OK;
}

static struct nb_config_cb *
nb_apply_finish_cb_new(struct nb_config_cbs *cbs, const char *xpath,
		       const struct nb_node *nb_node,
		       const struct lyd_node *dnode)
{
	struct nb_config_cb *cb;

	cb = XCALLOC(MTYPE_TMP, sizeof(*cb));
	strlcpy(cb->xpath, xpath, sizeof(cb->xpath));
	cb->nb_node = nb_node;
	cb->dnode = dnode;
	RB_INSERT(nb_config_cbs, cbs, cb);

	return cb;
}

static struct nb_config_cb *
nb_apply_finish_cb_find(struct nb_config_cbs *cbs, const char *xpath,
			const struct nb_node *nb_node)
{
	struct nb_config_cb s;

	strlcpy(s.xpath, xpath, sizeof(s.xpath));
	s.nb_node = nb_node;
	return RB_FIND(nb_config_cbs, cbs, &s);
}

/* Call the 'apply_finish' callbacks. */
static void nb_transaction_apply_finish(struct nb_transaction *transaction)
{
	struct nb_config_cbs cbs;
	struct nb_config_cb *cb;

	/* Initialize tree of 'apply_finish' callbacks. */
	RB_INIT(nb_config_cbs, &cbs);

	/* Identify the 'apply_finish' callbacks that need to be called. */
	RB_FOREACH (cb, nb_config_cbs, &transaction->changes) {
		struct nb_config_change *change = (struct nb_config_change *)cb;
		const struct lyd_node *dnode = change->cb.dnode;

		/*
		 * Iterate up to the root of the data tree. When a node is being
		 * deleted, skip its 'apply_finish' callback if one is defined
		 * (the 'apply_finish' callbacks from the node ancestors should
		 * be called though).
		 */
		if (change->cb.operation == NB_OP_DELETE) {
			char xpath[XPATH_MAXLEN];

			dnode = dnode->parent;
			if (!dnode)
				break;

			/*
			 * The dnode from 'delete' callbacks point to elements
			 * from the running configuration. Use yang_dnode_get()
			 * to get the corresponding dnode from the candidate
			 * configuration that is being committed.
			 */
			yang_dnode_get_path(dnode, xpath, sizeof(xpath));
			dnode = yang_dnode_get(transaction->config->dnode,
					       xpath);
		}
		while (dnode) {
			char xpath[XPATH_MAXLEN];
			struct nb_node *nb_node;

			nb_node = dnode->schema->priv;
			if (!nb_node->cbs.apply_finish)
				goto next;

			/*
			 * Don't call the callback more than once for the same
			 * data node.
			 */
			yang_dnode_get_path(dnode, xpath, sizeof(xpath));
			if (nb_apply_finish_cb_find(&cbs, xpath, nb_node))
				goto next;

			nb_apply_finish_cb_new(&cbs, xpath, nb_node, dnode);

		next:
			dnode = dnode->parent;
		}
	}

	/* Call the 'apply_finish' callbacks, sorted by their priorities. */
	RB_FOREACH (cb, nb_config_cbs, &cbs) {
		if (debug_northbound)
			nb_log_callback(NB_EV_APPLY, NB_OP_APPLY_FINISH,
					cb->xpath, NULL);

		(*cb->nb_node->cbs.apply_finish)(cb->dnode);
	}

	/* Release memory. */
	while (!RB_EMPTY(nb_config_cbs, &cbs)) {
		cb = RB_ROOT(nb_config_cbs, &cbs);
		RB_REMOVE(nb_config_cbs, &cbs, cb);
		XFREE(MTYPE_TMP, cb);
	}
}

bool nb_operation_is_valid(enum nb_operation operation,
			   const struct lys_node *snode)
{
	struct lys_node_container *scontainer;
	struct lys_node_leaf *sleaf;

	switch (operation) {
	case NB_OP_CREATE:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_W))
			return false;

		switch (snode->nodetype) {
		case LYS_LEAF:
			sleaf = (struct lys_node_leaf *)snode;
			if (sleaf->type.base != LY_TYPE_EMPTY)
				return false;
			break;
		case LYS_CONTAINER:
			scontainer = (struct lys_node_container *)snode;
			if (!scontainer->presence)
				return false;
			break;
		case LYS_LIST:
		case LYS_LEAFLIST:
			break;
		default:
			return false;
		}
		return true;
	case NB_OP_MODIFY:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_W))
			return false;

		switch (snode->nodetype) {
		case LYS_LEAF:
			sleaf = (struct lys_node_leaf *)snode;
			if (sleaf->type.base == LY_TYPE_EMPTY)
				return false;

			/* List keys can't be modified. */
			if (lys_is_key(sleaf, NULL))
				return false;
			break;
		default:
			return false;
		}
		return true;
	case NB_OP_DELETE:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_W))
			return false;

		switch (snode->nodetype) {
		case LYS_LEAF:
			sleaf = (struct lys_node_leaf *)snode;

			/* List keys can't be deleted. */
			if (lys_is_key(sleaf, NULL))
				return false;

			/*
			 * Only optional leafs can be deleted, or leafs whose
			 * parent is a case statement.
			 */
			if (snode->parent->nodetype == LYS_CASE)
				return true;
			if (sleaf->when)
				return true;
			if (CHECK_FLAG(sleaf->flags, LYS_MAND_TRUE)
			    || sleaf->dflt)
				return false;
			break;
		case LYS_CONTAINER:
			scontainer = (struct lys_node_container *)snode;
			if (!scontainer->presence)
				return false;
			break;
		case LYS_LIST:
		case LYS_LEAFLIST:
			break;
		default:
			return false;
		}
		return true;
	case NB_OP_MOVE:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_W))
			return false;

		switch (snode->nodetype) {
		case LYS_LIST:
		case LYS_LEAFLIST:
			if (!CHECK_FLAG(snode->flags, LYS_USERORDERED))
				return false;
			break;
		default:
			return false;
		}
		return true;
	case NB_OP_APPLY_FINISH:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_W))
			return false;
		return true;
	case NB_OP_GET_ELEM:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_R))
			return false;

		switch (snode->nodetype) {
		case LYS_LEAF:
			break;
		case LYS_CONTAINER:
			scontainer = (struct lys_node_container *)snode;
			if (!scontainer->presence)
				return false;
			break;
		default:
			return false;
		}
		return true;
	case NB_OP_GET_NEXT:
	case NB_OP_GET_KEYS:
	case NB_OP_LOOKUP_ENTRY:
		if (!CHECK_FLAG(snode->flags, LYS_CONFIG_R))
			return false;

		switch (snode->nodetype) {
		case LYS_LIST:
			break;
		default:
			return false;
		}
		return true;
	case NB_OP_RPC:
		if (CHECK_FLAG(snode->flags, LYS_CONFIG_W | LYS_CONFIG_R))
			return false;

		switch (snode->nodetype) {
		case LYS_RPC:
		case LYS_ACTION:
			break;
		default:
			return false;
		}
		return true;
	default:
		return false;
	}
}

DEFINE_HOOK(nb_notification_send, (const char *xpath, struct list *arguments),
	    (xpath, arguments));

int nb_notification_send(const char *xpath, struct list *arguments)
{
	int ret;

	ret = hook_call(nb_notification_send, xpath, arguments);
	if (arguments)
		list_delete(&arguments);

	return ret;
}

const char *nb_event_name(enum nb_event event)
{
	switch (event) {
	case NB_EV_VALIDATE:
		return "validate";
	case NB_EV_PREPARE:
		return "prepare";
	case NB_EV_ABORT:
		return "abort";
	case NB_EV_APPLY:
		return "apply";
	default:
		return "unknown";
	}
}

const char *nb_operation_name(enum nb_operation operation)
{
	switch (operation) {
	case NB_OP_CREATE:
		return "create";
	case NB_OP_MODIFY:
		return "modify";
	case NB_OP_DELETE:
		return "delete";
	case NB_OP_MOVE:
		return "move";
	case NB_OP_APPLY_FINISH:
		return "apply_finish";
	case NB_OP_GET_ELEM:
		return "get_elem";
	case NB_OP_GET_NEXT:
		return "get_next";
	case NB_OP_GET_KEYS:
		return "get_keys";
	case NB_OP_LOOKUP_ENTRY:
		return "lookup_entry";
	case NB_OP_RPC:
		return "rpc";
	default:
		return "unknown";
	}
}

const char *nb_err_name(enum nb_error error)
{
	switch (error) {
	case NB_OK:
		return "ok";
	case NB_ERR:
		return "generic error";
	case NB_ERR_NO_CHANGES:
		return "no changes";
	case NB_ERR_NOT_FOUND:
		return "element not found";
	case NB_ERR_LOCKED:
		return "resource is locked";
	case NB_ERR_VALIDATION:
		return "validation error";
	case NB_ERR_RESOURCE:
		return "failed to allocate resource";
	case NB_ERR_INCONSISTENCY:
		return "internal inconsistency";
	default:
		return "unknown";
	}
}

const char *nb_client_name(enum nb_client client)
{
	switch (client) {
	case NB_CLIENT_CLI:
		return "CLI";
	case NB_CLIENT_CONFD:
		return "ConfD";
	case NB_CLIENT_SYSREPO:
		return "Sysrepo";
	default:
		return "unknown";
	}
}

static void nb_load_callbacks(const struct frr_yang_module_info *module)
{
	for (size_t i = 0; module->nodes[i].xpath; i++) {
		struct nb_node *nb_node;
		uint32_t priority;

		nb_node = nb_node_find(module->nodes[i].xpath);
		if (!nb_node) {
			flog_warn(EC_LIB_YANG_UNKNOWN_DATA_PATH,
				  "%s: unknown data path: %s", __func__,
				  module->nodes[i].xpath);
			continue;
		}

		nb_node->cbs = module->nodes[i].cbs;
		priority = module->nodes[i].priority;
		if (priority != 0)
			nb_node->priority = priority;
	}
}

void nb_init(const struct frr_yang_module_info *modules[], size_t nmodules)
{
	unsigned int errors = 0;

	/* Load YANG modules. */
	for (size_t i = 0; i < nmodules; i++)
		yang_module_load(modules[i]->name);

	/* Create a nb_node for all YANG schema nodes. */
	yang_all_snodes_iterate(nb_node_new_cb, 0, NULL, NULL);

	/* Load northbound callbacks. */
	for (size_t i = 0; i < nmodules; i++)
		nb_load_callbacks(modules[i]);

	/* Validate northbound callbacks. */
	yang_all_snodes_iterate(nb_node_validate, 0, &errors, NULL);
	if (errors > 0) {
		flog_err(
			EC_LIB_NB_CBS_VALIDATION,
			"%s: failed to validate northbound callbacks: %u error(s)",
			__func__, errors);
		exit(1);
	}

	/* Initialize the northbound database (used for the rollback log). */
	if (nb_db_init() != NB_OK)
		flog_warn(EC_LIB_NB_DATABASE,
			  "%s: failed to initialize northbound database",
			  __func__);

	/* Create an empty running configuration. */
	running_config = nb_config_new(NULL);

	/* Initialize the northbound CLI. */
	nb_cli_init();
}

void nb_terminate(void)
{
	/* Terminate the northbound CLI. */
	nb_cli_terminate();

	/* Delete all nb_node's from all YANG modules. */
	yang_all_snodes_iterate(nb_node_del_cb, 0, NULL, NULL);

	/* Delete the running configuration. */
	nb_config_free(running_config);
}
