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
#include "log_int.h"
#include "yang.h"
#include "northbound.h"

DEFINE_MTYPE(LIB, YANG_MODULE, "YANG module")
DEFINE_MTYPE(LIB, YANG_DATA, "YANG data structure")

/* libyang container */
struct ly_ctx *ly_ctx;

static inline int yang_module_compare(const struct yang_module *a,
				      const struct yang_module *b)
{
	return strcmp(a->name, b->name);
}

RB_GENERATE(yang_modules, yang_module, entry, yang_module_compare)

struct yang_modules yang_modules = RB_INITIALIZER(&yang_modules);

struct yang_module *yang_module_new(const char *module_name)
{
	struct yang_module *module;
	const struct lys_module *module_info;
	char path[MAXPATHLEN];

	snprintf(path, sizeof(path), "%s/%s.yang", YANG_MODELS_PATH,
		 module_name);

	module_info = lys_parse_path(ly_ctx, path, LYS_IN_YANG);
	if (!module_info) {
		zlog_err("%s: failed to load data model: %s", __func__, path);
		exit(1);
	}

	module = XCALLOC(MTYPE_YANG_MODULE, sizeof(*module));
	module->name = module_name;
	module->info = module_info;

	if (RB_INSERT(yang_modules, &yang_modules, module) != NULL) {
		zlog_err("%s: YANG module is loaded already: %s", __func__,
			 module_name);
		exit(1);
	}

	return module;
}

void yang_module_del(struct yang_module *module)
{
	/*
	 * We shouldn't call ly_ctx_remove_module() here because this function
	 * also removes other modules that depend on it.
	 *
	 * ly_ctx_destroy() will release all memory for us.
	 */
	RB_REMOVE(yang_modules, &yang_modules, module);
	XFREE(MTYPE_YANG_MODULE, module);
}

struct yang_module *yang_module_find(const char *module_name)
{
	struct yang_module s;

	s.name = module_name;
	return RB_FIND(yang_modules, &yang_modules, &s);
}

/* Iterate through all schema nodes from all loaded YANG modules. */
void yang_snodes_iterate(void (*func)(struct yang_module *, struct lys_node *,
				      void *),
			 void *arg)
{
	struct yang_module *module;

	RB_FOREACH (module, yang_modules, &yang_modules) {
		struct lys_node *root, *next, *snode;

		LY_TREE_FOR (module->info->data, root) {
			LY_TREE_DFS_BEGIN (root, next, snode) {
				if (snode->nodetype == LYS_CHOICE
				    || snode->nodetype == LYS_CASE)
					goto next;

				(*func)(module, snode, arg);

			next:
				LY_TREE_DFS_END(root, next, snode);
			}
		}
	}
}

/* Find nearest parent presence container or list. */
struct lys_node *yang_find_real_parent(const struct lys_node *snode)
{
	struct lys_node *parent = snode->parent;

	while (parent) {
		struct lys_node_container *scontainer;

		switch (parent->nodetype) {
		case LYS_CONTAINER:
			scontainer = (struct lys_node_container *)parent;
			if (scontainer->presence)
				return parent;
			break;
		case LYS_LIST:
			return parent;
		default:
			break;
		}
		parent = parent->parent;
	}

	return NULL;
}

/* Find nearest parent list. */
struct lys_node *yang_find_parent_list(const struct lys_node *snode)
{
	struct lys_node *parent = snode->parent;

	while (parent) {
		switch (parent->nodetype) {
		case LYS_LIST:
			return parent;
		default:
			break;
		}
		parent = parent->parent;
	}

	return NULL;
}

const char *yang_default_value(const char *xpath)
{
	const struct lys_node *snode;
	struct lys_node_leaf *sleaf;

	snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 0);
	if (snode == NULL) {
		zlog_warn("%s: couldn't find schema information for '%s'",
			  __func__, xpath);
		return NULL;
	}

	switch (snode->nodetype) {
	case LYS_LEAF:
		sleaf = (struct lys_node_leaf *)snode;

		/* NOTE: this might be null. */
		return sleaf->dflt;
	case LYS_LEAFLIST:
		/* TODO */
		return NULL;
	default:
		return NULL;
	}
}

bool yang_node_is_default(const struct lyd_node *dnode)
{
	if (!yang_node_has_value(dnode->schema))
		return false;

	return (lyd_wd_default((struct lyd_node_leaf_list *)dnode) == 1);
}

bool yang_node_has_value(const struct lys_node *snode)
{
	struct lys_node_leaf *sleaf;

	switch (snode->nodetype) {
	case LYS_LEAF:
		sleaf = (struct lys_node_leaf *)snode;
		if (sleaf->type.base == LY_TYPE_EMPTY)
			return false;
		return true;
	case LYS_LEAFLIST:
		return true;
	default:
		return false;
	}
}

struct yang_data *yang_data_new(const char *xpath, const char *value)
{
	const struct lys_node *snode;
	struct yang_data *data;

	snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 0);
	if (snode == NULL)
		snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 1);
	if (snode == NULL) {
		zlog_err("%s: couldn't find schema information for '%s'",
			 __func__, xpath);
		exit(1);
	}

	data = XCALLOC(MTYPE_YANG_DATA, sizeof(*data));
	strlcpy(data->xpath, xpath, sizeof(data->xpath));
	data->snode = snode;
	if (value)
		data->value = strdup(value);

	return data;
}

void yang_data_free(struct yang_data *data)
{
	if (data->value)
		free(data->value);
	XFREE(MTYPE_YANG_DATA, data);
}

struct list *yang_data_list_new(void)
{
	struct list *list;

	list = list_new();
	list->del = (void (*)(void *))yang_data_free;

	return list;
}

bool yang_parse_children(const struct lyd_node *dnode,
			 struct yang_data children[], size_t size)
{
	struct lyd_node *child;
	bool all_defaults = true;

	LY_TREE_FOR (dnode->child, child) {
		struct nb_option *option = child->schema->priv;

		if (!yang_node_is_default(child))
			all_defaults = false;

		for (size_t i = 0; i < size; i++) {
			if (strmatch(option->xpath, children[i].xpath)) {
				children[i].value =
					(char *)yang_dnode_get_string(child);
				break;
			}
		}
	}

	return all_defaults;
}

void *yang_dnode_lookup_list_entry(const struct lyd_node *dnode)
{
	struct nb_option *option;
	struct yang_list_keys keys;
	void *list_entry;
	char *xpath;

	option = dnode->schema->priv;
	if (option->snode->nodetype != LYS_LIST) {
		option = option->parent_list;
		if (option == NULL)
			goto error;
	}

	yang_dnode_get_keys(dnode, &keys);
	list_entry = option->cbs.lookup_entry(&keys);
	if (list_entry == NULL)
		goto error;

	return list_entry;

error:
	xpath = lyd_path(dnode);
	zlog_warn("%s: failed to find list entry [xpath %s]", __func__, xpath);
	free(xpath);
	return NULL;
}

void yang_dnode_get_keys(const struct lyd_node *dnode,
			 struct yang_list_keys *keys)
{
	struct list *dnodes;
	struct lyd_node *dn;
	struct listnode *ln;
	uint8_t n = 0;

	/* Create list to store data nodes starting from the root node. */
	dnodes = list_new();
	for (dn = (struct lyd_node *)dnode; dn; dn = dn->parent) {
		if (dn->schema->nodetype != LYS_LIST)
			continue;
		listnode_add_head(dnodes, dn);
	}

	memset(keys, 0, sizeof(*keys));
	for (ALL_LIST_ELEMENTS_RO(dnodes, ln, dn)) {
		struct lyd_node *child;

		LY_TREE_FOR (dn->child, child) {
			if (!lys_is_key(
				    (const struct lys_node_leaf *)child->schema,
				    NULL))
				continue;
			strlcpy(keys->key[n].value,
				yang_dnode_get_string(child),
				sizeof(keys->key[n].value));
			n++;
		}
	}
	keys->num = n;
	list_delete_and_null(&dnodes);
}

int yang_xpath_get_keys(const char *xpath, struct yang_list_keys *keys)
{
	struct nb_option *option;
	const struct lys_node *snode;
	struct lys_node_list *slist;
	struct lys_node_leaf *key;
	char format[XPATH_MAXLEN];
	int n;

	snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 0);
	if (snode == NULL) {
		zlog_warn("%s: couldn't find schema information for '%s'",
			  __func__, xpath);
		return -1;
	}

	if (snode->nodetype != LYS_LIST)
		snode = yang_find_parent_list(snode);
	if (snode == NULL)
		return 0;

	slist = (struct lys_node_list *)snode;
	option = slist->priv;

	memset(keys, 0, sizeof(*keys));
	strlcpy(format, option->xpath, sizeof(format));
	for (uint8_t i = 0; i < slist->keys_size; i++) {
		key = slist->keys[i];
		snprintf(format + strlen(format),
			 sizeof(format) - strlen(format), "[%s='%%[^']']",
			 key->name);
		keys->key[i].snode = (struct lys_node *)key;
	}
	keys->num = slist->keys_size;

	n = sscanf(xpath, format, keys->key[0].value, keys->key[1].value,
		   keys->key[2].value, keys->key[3].value, keys->key[4].value,
		   keys->key[5].value, keys->key[6].value, keys->key[7].value);
	if (n < 0) {
		zlog_warn("%s: sscanf() failed: %s", __func__,
			  safe_strerror(errno));
		return -1;
	}
	if (n != slist->keys_size) {
		zlog_warn("%s: read %d keys, expected %d", __func__, n,
			  slist->keys_size);
		return -1;
	}

	return 0;
}

static void ly_log_cb_dummy(LY_LOG_LEVEL level, const char *msg,
			    const char *path)
{
	/* Do nothing. */
}

static void ly_log_cb(LY_LOG_LEVEL level, const char *msg, const char *path)
{
	int priority;

	switch (level) {
	case LY_LLERR:
		priority = LOG_ERR;
		break;
	case LY_LLWRN:
		priority = LOG_WARNING;
		break;
	case LY_LLVRB:
		priority = LOG_DEBUG;
		break;
	default:
		return;
	}

	if (path)
		zlog(priority, "libyang: %s (%s)", msg, path);
	else
		zlog(priority, "libyang: %s", msg);
}

static void yang_option_init(struct yang_module *module, struct lys_node *snode,
			     void *arg)
{
	nb_option_new(module, snode);
}

/*
 * Initialize libyang container.
 */
void yang_init(const char *modules[], size_t nmodules)
{
	static char ly_plugin_dir[PATH_MAX];
	const char *const *ly_loaded_plugins;
	const char *ly_plugin;
	bool found_ly_frr_types = false;

	snprintf(ly_plugin_dir, sizeof(ly_plugin_dir), "%s=%s",
		 "LIBYANG_USER_TYPES_PLUGINS_DIR", LIBYANG_PLUGINS_PATH);
	putenv(ly_plugin_dir);

	ly_ctx = ly_ctx_new(NULL, 0);
	if (ly_ctx == NULL) {
		zlog_err("ly_ctx_new");
		exit(1);
	}

	/*
	 * Detect if the required libyang plugin(s) were loaded successfully.
	 */
	ly_loaded_plugins = ly_get_loaded_plugins();
	for (size_t i = 0; (ly_plugin = ly_loaded_plugins[i]); i++) {
		if (strmatch(ly_plugin, "frr_user_types")) {
			found_ly_frr_types = true;
			break;
		}
	}
	if (!found_ly_frr_types) {
		zlog_err("%s: failed to load frr_user_types.so", __func__);
		exit(1);
	}

	ly_set_log_clb(ly_log_cb_dummy, 0);
	ly_ctx_set_searchdir(ly_ctx, YANG_MODELS_PATH);

	ly_set_log_clb(ly_log_cb, 1);
	ly_log_options(LY_LOLOG | LY_LOSTORE);
	ly_err_clean(ly_ctx, NULL);

	/* Load daemon YANG modules. */
	for (size_t i = 0; i < nmodules; i++)
		yang_module_new(modules[i]);

	/* Parse and process all loaded YANG nodes. */
	yang_snodes_iterate(yang_option_init, NULL);
}

void yang_terminate(void)
{
	struct yang_module *module;

	while (!RB_EMPTY(yang_modules, &yang_modules)) {
		module = RB_ROOT(yang_modules, &yang_modules);
		yang_module_del(module);
	}

	ly_ctx_unset_searchdirs(ly_ctx, -1);
	ly_ctx_destroy(ly_ctx, NULL);
}
