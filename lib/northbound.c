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
#include "hash.h"
#include "command.h"
#include "db.h"
#include "northbound.h"

struct lyd_node *running_config;
struct lyd_node *candidate_config;

DEFINE_HOOK(nb_notification_send, (const char *xpath, struct list *arguments),
	    (xpath, arguments));

/* Prototypes. */
static struct nb_transaction *nb_transaction_new(struct lyd_node *config,
						 enum nb_client client,
						 char *comment);
static void nb_transaction_free(struct nb_transaction *transaction);
static void nb_transaction_add_change(struct nb_transaction *transaction,
				      enum nb_operation operation,
				      const struct lyd_node *dnode);
static void nb_transaction_del_change(struct nb_transaction *transaction,
				      struct nb_config_change *cfg_change);
static int nb_transaction_process(enum nb_event event,
				  struct nb_transaction *transaction);

/* Hash table used to optimize lookups to the running configuration. */
static struct hash *frr_config_hash;

struct frr_config_node {
	char xpath[XPATH_MAXLEN];
	struct lyd_node *dnode;
};

static int frr_config_hash_cmp(const void *value1, const void *value2)
{
	const struct frr_config_node *c1 = value1;
	const struct frr_config_node *c2 = value2;

	return strmatch(c1->xpath, c2->xpath);
}

static unsigned int frr_config_hash_key(void *value)
{
	return string_hash_make(value);
}

static void *frr_config_hash_alloc(void *p)
{
	struct frr_config_node *new, *key = p;

	new = XCALLOC(MTYPE_TMP, sizeof(*new));
	strlcpy(new->xpath, key->xpath, sizeof(new->xpath));

	return new;
}

static void frr_config_hash_free(void *arg)
{
	XFREE(MTYPE_TMP, arg);
}

/*
 * This is a global lock used to prevent multiple configuration
 * transactions from happening concurrently.
 */
static bool transaction_in_progress;

DEFINE_MTYPE_STATIC(LIB, NB_OPTION, "Northbound Option")

int debug_northbound;

static inline int nb_option_compare(const struct nb_option *a,
				    const struct nb_option *b)
{
	return strcmp(a->xpath, b->xpath);
}

RB_GENERATE(nb_options, nb_option, entry, nb_option_compare);

static struct nb_options nb_options = RB_INITIALIZER(&nb_options);

struct nb_option *nb_option_new(struct yang_module *module,
				struct lys_node *snode)
{
	struct nb_option *option;
	char *xpath;
	struct lys_node *sparent, *sparent_list;

	sparent = yang_find_real_parent(snode);
	sparent_list = yang_find_parent_list(snode);

	option = XCALLOC(MTYPE_NB_OPTION, sizeof(*option));
	option->module = module;
	option->snode = snode;
	option->priority = NB_DFLT_PRIORITY;
	lys_set_private(snode, option); /* back pointer */
	if (sparent)
		option->parent = sparent->priv;
	if (sparent_list)
		option->parent_list = sparent_list->priv;

	xpath = lys_data_path(snode);
	strlcpy(option->xpath, xpath, sizeof(option->xpath));
	free(xpath);

	if (RB_INSERT(nb_options, &nb_options, option) != NULL) {
		zlog_err("%s: northbound option already exists: %s", __func__,
			 option->xpath);
		exit(1);
	}

	return option;
}

void nb_option_del(struct nb_option *option)
{
	RB_REMOVE(nb_options, &nb_options, option);
	XFREE(MTYPE_NB_OPTION, option);
}

struct nb_option *nb_option_find(const char *xpath)
{
	const struct lys_node *snode;

	snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 0);
	if (snode == NULL) {
		zlog_err("%s: couldn't find schema information for '%s'",
			 __func__, xpath);
		return NULL;
	}

	return snode->priv;
}

static int nb_option_validate_cb(const struct lys_node *snode,
				 const char *xpath, enum nb_operation operation,
				 int callback_implemented, bool optional)
{
	bool valid;

	valid = nb_operation_is_valid(operation, snode);

	if (!valid && callback_implemented)
		zlog_err("unneeded '%s' callback for '%s'",
			 nb_operation_name(operation), xpath);

	if (!optional && valid && !callback_implemented) {
		zlog_err("missing '%s' callback for '%s'",
			 nb_operation_name(operation), xpath);
		return 1;
	}

	return 0;
}

/*
 * Check if the required callbacks were implemented given the properties
 * of the YANG data option.
 */
static int nb_option_validate_cbs(const struct nb_option *option)

{
	const struct lys_node *snode = option->snode;
	const char *xpath = option->xpath;
	const struct nb_callbacks *cbs = &option->cbs;
	int error = 0;

	error |= nb_option_validate_cb(snode, xpath, NB_OP_CREATE,
				       !!cbs->create, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_MODIFY,
				       !!cbs->modify, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_DELETE,
				       !!cbs->delete, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_MOVE, !!cbs->move,
				       false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_APPLY_FINISH,
				       !!cbs->apply_finish, true);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_GET_ELEM,
				       !!cbs->get_elem, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_GET_NEXT,
				       !!cbs->get_next, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_GET_KEYS,
				       !!cbs->get_keys, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_LOOKUP_ENTRY,
				       !!cbs->lookup_entry, false);
	error |= nb_option_validate_cb(snode, xpath, NB_OP_RPC, !!cbs->rpc,
				       false);

	return error;
}

static int nb_option_validate_priority(const struct nb_option *option)
{
	struct nb_option *option_parent = option->parent;
	uint32_t priority = option->priority;

	/* Top-level nodes can have any priority. */
	if (option->parent == NULL)
		return 0;

	if (priority < option_parent->priority) {
		zlog_err("node has higher priority than its parent [xpath %s]",
			 option->xpath);
		return 1;
	}

	return 0;
}

static void nb_option_validate(struct yang_module *module,
			       struct lys_node *snode, void *arg)
{
	struct nb_option *option = snode->priv;
	unsigned int *errors = arg;

	/* Validate callbacks and priority. */
	if (nb_option_validate_cbs(option) != 0
	    || nb_option_validate_priority(option) != 0)
		(*errors)++;
}

void nb_config_init(struct lyd_node **config)
{
	if (lyd_validate(config, LYD_OPT_CONFIG, ly_ctx) != 0) {
		zlog_err("%s: lyd_validate() failed", __func__);
		exit(1);
	}
}

void nb_config_free(struct lyd_node **config)
{
	lyd_free_withsiblings(*config);
	*config = NULL;
}

struct lyd_node *nb_config_dup(const struct lyd_node *config)
{
	return lyd_dup_withsiblings(config, 1);
}

int nb_config_edit(struct lyd_node *config, struct nb_option *option,
		   enum nb_operation operation, const char *xpath,
		   struct yang_data *previous, struct yang_data *data)
{
	struct nb_option *parent = option->parent;
	struct lyd_node *dnode;
	char xpath_edit[XPATH_MAXLEN];

	if (!nb_operation_is_valid(operation, option->snode)) {
		zlog_warn("%s: %s operation not valid for %s", __func__,
			  nb_operation_name(operation), xpath);
		return NB_ERR;
	}

	/* Use special notation for leaf-lists. */
	if (option->snode->nodetype == LYS_LEAFLIST)
		snprintf(xpath_edit, sizeof(xpath_edit), "%s[.='%s']", xpath,
			 data->value);
	else
		strlcpy(xpath_edit, xpath, sizeof(xpath_edit));

	switch (operation) {
	case NB_OP_CREATE:
	case NB_OP_MODIFY:
		if (parent && !nb_config_exists(config, parent->xpath)) {
			zlog_warn("%s: parent doesn't exist [xpath %s]",
				  __func__, xpath);
			return NB_ERR;
		}

		ly_errno = 0;
		/*
		 * Ideally we would use the LYD_PATH_OPT_NOPARENT flag to ensure
		 * we're not creating a child dnode before creating its parent
		 * first.
		 * But since we don't keep track of non-presence containers then
		 * we can't do this.
		 */
		dnode = lyd_new_path(config, ly_ctx, xpath_edit,
				     (void *)data->value, 0,
				     LYD_PATH_OPT_UPDATE);
		if (dnode == NULL && ly_errno) {
			zlog_err("%s: lyd_new_path() failed", __func__);
			return NB_ERR;
		}

		/*
		 * If a new node was created, call lyd_validate() only to create
		 * default child nodes.
		 */
		if (dnode) {
			lyd_schema_sort(dnode, 0);
			lyd_validate(&dnode, LYD_OPT_CONFIG, ly_ctx);
		}
		break;
	case NB_OP_DELETE:
		dnode = nb_config_get(config, xpath_edit);
		if (dnode == NULL) {
			/*
			 * Return a special error code so the caller can choose
			 * whether to ignore it or not.
			 */
			return NB_ERR_NOT_FOUND;
		}
		lyd_free(dnode);
		break;
	case NB_OP_MOVE:
		/* TODO: update configuration. */
		break;
	default:
		zlog_warn("%s: unknown operation (%u) [xpath %s]", __func__,
			  operation, xpath_edit);
		return NB_ERR;
	}

	return NB_OK;
}

struct lyd_node *nb_config_get(const struct lyd_node *config, const char *xpath)
{
	struct ly_set *set;
	struct lyd_node *dnode = NULL;

	set = lyd_find_path(config, xpath);
	assert(set);
	if (set->number == 0)
		goto exit;

	if (set->number > 1) {
		zlog_warn("%s: found %u elements (expected 0 or 1) [xpath %s]",
			  __func__, set->number, xpath);
		goto exit;
	}

	dnode = set->set.d[0];

exit:
	ly_set_free(set);

	return dnode;
}

struct lyd_node *nb_config_get_running(const char *xpath)
{
	struct frr_config_node *config, s;

	strlcpy(s.xpath, xpath, sizeof(s.xpath));
	config = hash_lookup(frr_config_hash, &s);
	if (config == NULL)
		return NULL;

	return config->dnode;
}

bool nb_config_exists(const struct lyd_node *config, const char *xpath)
{
	struct ly_set *set;
	bool found = false;

	set = lyd_find_path(config, xpath);
	assert(set);
	found = set->number > 0;
	ly_set_free(set);

	return found;
}

static void nb_config_diff_new_subtree(struct nb_transaction *transaction,
				       const struct lyd_node *dnode)
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

			nb_transaction_add_change(transaction, operation,
						  child);
			break;
		case LYS_CONTAINER:
		case LYS_LIST:
			if (nb_operation_is_valid(NB_OP_CREATE, child->schema))
				nb_transaction_add_change(transaction,
							  NB_OP_CREATE, child);
			nb_config_diff_new_subtree(transaction, child);
			break;
		default:
			break;
		}
	}
}

static int nb_config_diff(struct nb_transaction *transaction,
			  struct lyd_node *config1, struct lyd_node *config2)
{
	struct lyd_difflist *diff;

	diff = lyd_diff(config1, config2, LYD_DIFFOPT_WITHDEFAULTS);
	if (diff == NULL) {
		zlog_warn("%s: lyd_diff() failed", __func__);
		return NB_ERR;
	}

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

		nb_transaction_add_change(transaction, operation, dnode);

		if (type == LYD_DIFF_CREATED
		    && (dnode->schema->nodetype == LYS_CONTAINER
			|| dnode->schema->nodetype == LYS_LIST))
			nb_config_diff_new_subtree(transaction, dnode);
	}

	lyd_free_diff(diff);

	return NB_OK;
}

/* Rebuild the running config hash table from scratch. */
static void nb_rebuild_config_hash_table(void)
{
	struct lyd_node *root, *next, *dnode;

	hash_clean(frr_config_hash, frr_config_hash_free);
	hash_free(frr_config_hash);
	frr_config_hash = hash_create(frr_config_hash_key, frr_config_hash_cmp,
				      "FRR configuration");
	LY_TREE_FOR (running_config, root) {
		LY_TREE_DFS_BEGIN (root, next, dnode) {
			char *xpath;
			struct frr_config_node *config, s;

			xpath = lyd_path(dnode);
			strlcpy(s.xpath, xpath, sizeof(s.xpath));
			free(xpath);
			config = hash_get(frr_config_hash, &s,
					  frr_config_hash_alloc);
			config->dnode = dnode;

			LY_TREE_DFS_END(root, next, dnode);
		}
	}
}

int nb_candidate_validate(struct lyd_node **config)
{
	if (lyd_validate(config, LYD_OPT_STRICT | LYD_OPT_CONFIG, ly_ctx) != 0)
		return NB_ERR;

	return NB_OK;
}

int nb_candidate_commit(struct lyd_node *config, enum nb_client client,
			bool save_transaction, char *comment)
{
	struct nb_transaction *transaction;
	int ret;

	ret = nb_candidate_validate(&config);
	if (ret != NB_OK) {
		zlog_warn("%s: failed to validate candidate configuration",
			  __func__);
		return NB_ERR;
	}

	transaction = nb_transaction_new(config, client, comment);
	if (transaction == NULL) {
		zlog_warn("%s: failed to create transaction", __func__);
		return NB_ERR_LOCKED;
	}

	ret = nb_config_diff(transaction, running_config, config);
	if (ret != NB_OK) {
		zlog_warn("%s: failed to compare configurations", __func__);
		goto exit;
	}

	if (RB_EMPTY(nb_config_changes, &transaction->changes)) {
		ret = NB_ERR_NO_CHANGES;
		goto exit;
	}

	/*
	 * If the preparation was ok, then apply the changes. Otherwise abort
	 * the transaction.
	 */
	ret = nb_transaction_process(NB_EV_PREPARE, transaction);
	if (ret == NB_OK) {
		struct lyd_node *previous_running_config;

		/* Replace running by candidate. */
		previous_running_config = running_config;
		running_config = nb_config_dup(config);
		nb_rebuild_config_hash_table();

		(void)nb_transaction_process(NB_EV_APPLY, transaction);

		/*
		 * This needs to be done after calling nb_transaction_process()
		 * because the northbound callbacks can receive pointers to
		 * nodes that were deleted from the running configuration.
		 */
		nb_config_free(&previous_running_config);

		if (save_transaction
		    && nb_db_transaction_save(transaction) != NB_OK)
			zlog_err("%s: failed to save transaction", __func__);
	} else {
		(void)nb_transaction_process(NB_EV_ABORT, transaction);
	}

exit:
	nb_transaction_free(transaction);

	return ret;
}

static int nb_callback(const enum nb_event event,
		       struct nb_config_change *change)
{
	enum nb_operation operation = change->operation;
	const char *xpath = change->xpath;
	struct nb_option *option = change->option;
	int ret = NB_ERR;

	if (debug_northbound) {
		const char *value = NULL;

		if (change->dnode && yang_node_has_value(change->dnode->schema))
			value = yang_dnode_get_string(change->dnode);

		zlog_debug("%s: event [%s] op [%s] xpath [%s] value [%s]",
			   __func__, nb_event_name(event),
			   nb_operation_name(operation), xpath,
			   value ? value : "(null)");
	}

	switch (operation) {
	case NB_OP_CREATE:
		ret = (*option->cbs.create)(event, change->dnode,
					    &change->resource);
		break;
	case NB_OP_MODIFY:
		ret = (*option->cbs.modify)(event, change->dnode,
					    &change->resource);
		break;
	case NB_OP_DELETE:
		ret = (*option->cbs.delete)(event, change->dnode);
		break;
	case NB_OP_MOVE:
		ret = (*option->cbs.move)(event, change->dnode);
		break;
	default:
		break;
	}

	if (ret != NB_OK)
		zlog_warn("%s: error processing '%s' callback [xpath %s]",
			  __func__, nb_operation_name(operation), xpath);

	return ret;
}

static inline int nb_config_change_compare(const struct nb_config_change *a,
					   const struct nb_config_change *b)
{
	/* Sort by priority first. */
	if (a->option->priority < b->option->priority)
		return -1;
	if (a->option->priority > b->option->priority)
		return 1;

	/*
	 * Use XPath as a tie-breaker. This will naturally sort parent nodes
	 * before their children.
	 */
	return strcmp(a->xpath, b->xpath);
}

RB_GENERATE(nb_config_changes, nb_config_change, entry,
	    nb_config_change_compare);

static struct nb_transaction *nb_transaction_new(struct lyd_node *config,
						 enum nb_client client,
						 char *comment)
{
	struct nb_transaction *transaction;

	if (transaction_in_progress) {
		zlog_warn("failed to create new configuration transaction");
		return NULL;
	}
	transaction_in_progress = true;

	transaction = XCALLOC(MTYPE_TMP, sizeof(*transaction));
	transaction->client = client;
	if (comment)
		strlcpy(transaction->comment, comment,
			sizeof(transaction->comment));
	transaction->config = config;
	RB_INIT(nb_config_changes, &transaction->changes);

	return transaction;
}

static void nb_transaction_free(struct nb_transaction *transaction)
{
	while (!RB_EMPTY(nb_config_changes, &transaction->changes)) {
		struct nb_config_change *change;

		change = RB_ROOT(nb_config_changes, &transaction->changes);
		nb_transaction_del_change(transaction, change);
	}

	XFREE(MTYPE_TMP, transaction);
	transaction_in_progress = false;
}

static void nb_transaction_add_change(struct nb_transaction *transaction,
				      enum nb_operation operation,
				      const struct lyd_node *dnode)
{
	struct nb_config_change *change;
	char *xpath;

	xpath = lyd_path(dnode);

	change = XCALLOC(MTYPE_TMP, sizeof(*change));
	change->operation = operation;
	change->option = dnode->schema->priv;
	strlcpy(change->xpath, xpath, sizeof(change->xpath));
	change->dnode = dnode;

	RB_INSERT(nb_config_changes, &transaction->changes, change);
	free(xpath);
}

static void nb_transaction_del_change(struct nb_transaction *transaction,
				      struct nb_config_change *change)
{
	RB_REMOVE(nb_config_changes, &transaction->changes, change);
	XFREE(MTYPE_TMP, change);
}

static int nb_transaction_process(enum nb_event event,
				  struct nb_transaction *transaction)
{
	struct nb_config_change *change;
	void (*apply_finish_cb)(void);
	struct list *apply_finish_list;

	/* Create list of 'apply_finish' callbacks. */
	if (event == NB_EV_APPLY)
		apply_finish_list = list_new();

	RB_FOREACH (change, nb_config_changes, &transaction->changes) {
		int ret;

		/*
		 * Only try to release resources that were allocated
		 * successfully.
		 */
		if (event == NB_EV_ABORT && change->prepare_ok == false)
			continue;

		/* Call the appropriate callback. */
		ret = nb_callback(event, change);
		switch (event) {
		case NB_EV_PREPARE:
			if (ret != NB_OK)
				return ret;
			change->prepare_ok = true;
			break;
		case NB_EV_ABORT:
		case NB_EV_APPLY:
			/* Ignore error (shouldn't happen). */
			break;
		}

		/*
		 * Save all 'apply_finish' callbacks to a list, ignoring
		 * duplicates.
		 */
		if (event == NB_EV_APPLY) {
			apply_finish_cb = change->option->cbs.apply_finish;
			if (apply_finish_cb
			    && !listnode_lookup(apply_finish_list,
						apply_finish_cb))
				listnode_add(apply_finish_list,
					     apply_finish_cb);
		}
	}

	/*
	 * Call the 'apply_finish' callbacks now that we're done with the
	 * regular callbacks.
	 */
	if (event == NB_EV_APPLY) {
		struct listnode *node;

		for (ALL_LIST_ELEMENTS_RO(apply_finish_list, node,
					  apply_finish_cb))
			(*apply_finish_cb)();

		list_delete_and_null(&apply_finish_list);
	}

	return NB_OK;
}

static int nb_db_init(void)
{
#ifdef HAVE_CONFIG_ROLLBACKS
	int ret;

	ret = db_execute(
		"                                                              \
		BEGIN TRANSACTION;                                             \
		  CREATE TABLE IF NOT EXISTS transactions(                     \
		    id             INTEGER  PRIMARY KEY AUTOINCREMENT NOT NULL,\
		    client         CHAR(32)             NOT NULL,              \
		    date           DATETIME             DEFAULT CURRENT_TIMESTAMP,\
		    comment        CHAR(80)             ,                      \
		    configuration  TEXT                 NOT NULL               \
		  );                                                           \
		  CREATE TRIGGER IF NOT EXISTS delete_tail                     \
		    AFTER INSERT ON transactions                               \
		    FOR EACH ROW                                               \
		    BEGIN                                                      \
		    DELETE                                                     \
		    FROM                                                       \
		      transactions                                             \
		    WHERE                                                      \
		      id%%%u=NEW.id%%%u AND id!=NEW.id;                        \
		    END;                                                       \
		COMMIT                                                         \
		;",
		NB_DLFT_MAX_CONFIG_ROLLBACKS, NB_DLFT_MAX_CONFIG_ROLLBACKS);
	if (ret != 0)
		return NB_ERR;
#endif /* HAVE_CONFIG_ROLLBACKS */

	return NB_OK;
}

int nb_db_transaction_save(struct nb_transaction *transaction)
{
#ifdef HAVE_CONFIG_ROLLBACKS
	struct sqlite3_stmt *ss;
	const char *client_name;
	char *config_str;
	int ret = NB_ERR;

	ss = db_prepare(
		"                                                              \
		INSERT INTO transactions                                       \
		  (client, comment, configuration)                             \
		VALUES                                                         \
		  (?, ?, ?)                                                    \
		;");
	if (ss == NULL)
		return NB_ERR;

	client_name = nb_client_name(transaction->client);
	if (lyd_print_mem(&config_str, transaction->config, LYD_XML,
			  LYP_FORMAT | LYP_WITHSIBLINGS)
	    != 0)
		goto exit;

	ret = db_bindf(ss, "%s%s%s", client_name, strlen(client_name),
		       transaction->comment, strlen(transaction->comment),
		       config_str ? config_str : "",
		       config_str ? strlen(config_str) : 0);
	if (ret != 0)
		goto exit;

	ret = db_run(ss);
	if (ret != 0)
		goto exit;

	ret = NB_OK;

exit:
	db_finalize(&ss);
	free(config_str);

	return ret;
#else
	return NB_OK;
#endif /* HAVE_CONFIG_ROLLBACKS */
}

struct lyd_node *nb_db_transaction_load(uint32_t transaction_id)
{
	struct lyd_node *config = NULL;
#ifdef HAVE_CONFIG_ROLLBACKS
	const char *config_str;
	struct sqlite3_stmt *ss;
	int ret;

	ss = db_prepare(
		"                                                              \
		SELECT                                                         \
		  configuration                                                \
		FROM                                                           \
		  transactions                                                 \
		WHERE                                                          \
		  id=?                                                         \
		;");
	if (ss == NULL)
		return NULL;

	ret = db_bindf(ss, "%d", transaction_id);
	if (ret != 0)
		goto exit;

	ret = db_run(ss);
	if (ret != SQLITE_ROW)
		goto exit;

	ret = db_loadf(ss, "%s", &config_str);
	if (ret != 0)
		goto exit;

	config = lyd_parse_mem(ly_ctx, config_str, LYD_XML, LYD_OPT_CONFIG);
	if (config == NULL)
		zlog_warn("%s: lyd_parse_path() failed", __func__);

exit:
	db_finalize(&ss);
#endif /* HAVE_CONFIG_ROLLBACKS */

	return config;
}

int nb_db_set_max_transactions(unsigned int max)
{
#ifdef HAVE_CONFIG_ROLLBACKS
	int ret;

	/*
	 * Delete old entries if necessary and update the SQL trigger that
	 * auto-deletes old entries.
	 */
	ret = db_execute(
		"                                                              \
		BEGIN TRANSACTION;                                             \
		  DELETE                                                       \
		  FROM                                                         \
		    transactions                                               \
		  WHERE                                                        \
		    ROWID IN (                                                 \
		      SELECT                                                   \
		        ROWID                                                  \
		      FROM                                                     \
		        transactions                                           \
		      ORDER BY ROWID DESC LIMIT -1 OFFSET %u                   \
		    );                                                         \
		  DROP TRIGGER delete_tail;                                    \
		  CREATE TRIGGER delete_tail                                   \
		  AFTER INSERT ON transactions                                 \
		    FOR EACH ROW                                               \
		    BEGIN                                                      \
		    DELETE                                                     \
		    FROM                                                       \
		      transactions                                             \
		    WHERE                                                      \
		      id%%%u=NEW.id%%%u AND id!=NEW.id;                        \
		    END;                                                       \
		COMMIT                                                         \
		;",
		max, max, max);
	if (ret != 0)
		return NB_ERR;
#endif /* HAVE_CONFIG_ROLLBACKS */

	return NB_OK;
}

/* Validate if the northbound operation is valid for the given node. */
bool nb_operation_is_valid(enum nb_operation operation,
			   const struct lys_node *snode)
{
	struct lys_node_container *scontainer;
	struct lys_node_leaf *sleaf;

	switch (operation) {
	case NB_OP_CREATE:
		if (!(snode->flags & LYS_CONFIG_W))
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
		break;
	case NB_OP_MODIFY:
		if (!(snode->flags & LYS_CONFIG_W))
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
		break;
	case NB_OP_DELETE:
		if (!(snode->flags & LYS_CONFIG_W))
			return false;

		switch (snode->nodetype) {
		case LYS_LEAF:
			/*
			 * Only optional leafs can be deleted, or leafs whose
			 * parent is a case statement.
			 */
			if (snode->parent->nodetype == LYS_CASE)
				return true;
			sleaf = (struct lys_node_leaf *)snode;
			if ((sleaf->flags & LYS_MAND_TRUE) || sleaf->dflt)
				return false;

			/* List keys can't be deleted. */
			if (lys_is_key(sleaf, NULL))
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
		break;
	case NB_OP_MOVE:
		if (!(snode->flags & LYS_CONFIG_W))
			return false;

		switch (snode->nodetype) {
		case LYS_LIST:
		case LYS_LEAFLIST:
			if (!(snode->flags & LYS_USERORDERED))
				return false;
			break;
		default:
			return false;
		}
		break;
	case NB_OP_APPLY_FINISH:
		if (!(snode->flags & LYS_CONFIG_W))
			return false;
		break;
	case NB_OP_GET_ELEM:
		if (!(snode->flags & LYS_CONFIG_R))
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
		break;
	case NB_OP_GET_NEXT:
	case NB_OP_GET_KEYS:
		if (!(snode->flags & LYS_CONFIG_R))
			return false;

		switch (snode->nodetype) {
		case LYS_LIST:
			break;
		default:
			return false;
		}
		break;
	case NB_OP_LOOKUP_ENTRY:
		switch (snode->nodetype) {
		case LYS_LIST:
			break;
		default:
			return false;
		}
		break;
	case NB_OP_RPC:
		if (snode->flags & (LYS_CONFIG_W | LYS_CONFIG_R))
			return false;

		switch (snode->nodetype) {
		case LYS_RPC:
		case LYS_ACTION:
			break;
		default:
			return false;
		}
		break;
	}

	return true;
}

int nb_notification_send(const char *xpath, struct list *arguments)
{
	return hook_call(nb_notification_send, xpath, arguments);
}

const char *nb_event_name(enum nb_event event)
{
	switch (event) {
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

const char *nb_client_name(enum nb_client client)
{
	switch (client) {
	case NB_CLIENT_CLI:
		return "CLI";
	default:
		return "unknown";
	}
}

void nb_load_callbacks(struct nb_option options[], size_t size)
{
	for (size_t i = 0; i < size; i++) {
		struct nb_option *option;
		uint32_t priority;

		option = nb_option_find(options[i].xpath);
		if (option == NULL) {
			zlog_warn("%s: unknown data [xpath %s]", __func__,
				  options[i].xpath);
			continue;
		}

		option->cbs = options[i].cbs;
		priority = options[i].priority;
		if (priority != 0)
			option->priority = priority;
	}
}

void nb_validate_callbacks(void)
{
	unsigned int errors = 0;

	yang_snodes_iterate(nb_option_validate, &errors);
	if (errors > 0) {
		zlog_err("failed to validate northbound callbacks: %u error(s)",
			 errors);
		exit(1);
	}
}

/* Debug CLI commands. */
DEFUN(debug_nb, debug_nb_cmd, "debug northbound",
      DEBUG_STR "Northbound Debugging\n")
{
	debug_northbound = 1;

	return CMD_SUCCESS;
}

DEFUN(no_debug_nb, no_debug_nb_cmd, "no debug northbound",
      NO_STR DEBUG_STR "Northbound Debugging\n")
{
	debug_northbound = 0;

	return CMD_SUCCESS;
}

static int nb_debug_config_write(struct vty *vty)
{
	if (debug_northbound)
		vty_out(vty, "debug northbound\n");

	return 1;
}

static struct cmd_node nb_debug_node = {NORTHBOUND_DEBUG_NODE, "", 1};

void nb_init(void)
{
	if (nb_db_init() != NB_OK)
		zlog_err("%s: failed to initialize transactions table",
			 __func__);

	nb_config_init(&running_config);
	nb_config_init(&candidate_config);

	frr_config_hash = hash_create(frr_config_hash_key, frr_config_hash_cmp,
				      "FRR configuration");

	/* install vty commands */
	install_node(&nb_debug_node, nb_debug_config_write);
	install_element(CONFIG_NODE, &debug_nb_cmd);
	install_element(ENABLE_NODE, &debug_nb_cmd);
	install_element(CONFIG_NODE, &no_debug_nb_cmd);
	install_element(ENABLE_NODE, &no_debug_nb_cmd);
}

void nb_terminate(void)
{
	struct nb_option *option;

	hash_clean(frr_config_hash, frr_config_hash_free);
	hash_free(frr_config_hash);

	while (!RB_EMPTY(nb_options, &nb_options)) {
		option = RB_ROOT(nb_options, &nb_options);
		nb_option_del(option);
	}

	nb_config_free(&running_config);
	nb_config_free(&candidate_config);
}
