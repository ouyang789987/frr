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

#ifndef _FRR_NORTHBOUND_H_
#define _FRR_NORTHBOUND_H_

#include "hook.h"
#include "yang.h"
#include "linklist.h"
#include "openbsd-tree.h"
#include "northbound_wrappers.h"

/* Forward declaration(s). */
struct vty;

/* Northbound events. */
enum nb_event {
	/*
	 * The configuration callback is supposed to verify that the changes are
	 * valid and prepare all resources required to apply them.
	 */
	NB_EV_PREPARE,

	/*
	 * Transaction has failed, the configuration callback needs to release
	 * all resources previously allocated.
	 */
	NB_EV_ABORT,

	/*
	 * The configuration changes need to be applied. The changes can't be
	 * rejected at this point.
	 */
	NB_EV_APPLY,
};

/*
 * Northbound operations.
 *
 * Please refer to 'nb_callbacks' structure below for more details.
 */
enum nb_operation {
	NB_OP_CREATE,
	NB_OP_MODIFY,
	NB_OP_DELETE,
	NB_OP_MOVE,
	NB_OP_APPLY_FINISH,
	NB_OP_GET_ELEM,
	NB_OP_GET_NEXT,
	NB_OP_GET_KEYS,
	NB_OP_LOOKUP_ENTRY,
	NB_OP_RPC,
};

union nb_resource {
	int fd;
	void *ptr;
};

struct nb_callbacks {
	/*
	 * Configuration callback.
	 *
	 * A presence container, list entry, leaf-list entry or leaf of type
	 * empty has been created.
	 */
	int (*create)(enum nb_event event, const struct lyd_node *dnode,
		      union nb_resource *resource);

	/*
	 * Configuration callback.
	 *
	 * The value of a leaf has been modified.
	 *
	 * List keys don't need to implement this callback. When a list key is
	 * modified, the 'delete' callback is called for the containing list
	 * with the old key value, then the 'create' callback is called with the
	 * new key value.
	 */
	int (*modify)(enum nb_event event, const struct lyd_node *dnode,
		      union nb_resource *resource);

	/*
	 * Configuration callback.
	 *
	 * A presence container, list entry, leaf-list entry or optional leaf
	 * has been deleted.
	 */
	int (*delete)(enum nb_event event, const struct lyd_node *dnode);

	/*
	 * Configuration callback.
	 *
	 * A list entry or leaf-list entry has been moved. Only applicable when
	 * the "ordered-by user" statement is present.
	 */
	int (*move)(enum nb_event event, const struct lyd_node *dnode);

	/*
	 * Optional configuration callback.
	 *
	 * It's called after all other callbacks during the apply phase
	 * (NB_EV_APPLY). If multiple options specify the same 'apply_finish'
	 * callback, it will be called only once.
	 */
	void (*apply_finish)(void);

	/*
	 * Operational data callback.
	 *
	 * The callback function should return the value of a specific leaf or
	 * inform if a typeless value (presence containers or leafs of type
	 * empty) exists or not.
	 */
	struct yang_data *(*get_elem)(const char *xpath, void *list_entry);

	/*
	 * Operational data callback.
	 *
	 * The callback function should return the next entry in the list. The
	 * 'element' parameter will be NULL on the first invocation.
	 */
	void *(*get_next)(void *element);

	/*
	 * Operational data callback.
	 *
	 * The callback function should fill the 'keys' parameter based on the
	 * given element.
	 */
	int (*get_keys)(void *element, struct yang_list_keys *keys);

	/*
	 * Lookup callback for both configuration and operational data.
	 *
	 * The callback function should return a list entry based on the list
	 * keys given as a parameter.
	 */
	void *(*lookup_entry)(struct yang_list_keys *keys);

	/*
	 * RPC and action callback.
	 *
	 * Both 'input' and 'output' are lists of 'yang_data' structures. The
	 * callback should fetch all the input parameters from the 'input' list,
	 * and add output parameters to the 'output' list if necessary.
	 */
	int (*rpc)(const char *xpath, const struct list *input,
		   struct list *output);

	/*
	 * Optional callback to show the CLI command(s) associated with the
	 * given YANG data node.
	 */
	void (*cli_show)(struct vty *vty, struct lyd_node *config,
			 bool show_defaults);
};

struct nb_option {
	RB_ENTRY(nb_option) entry;

	/* YANG module this option belongs to. */
	struct yang_module *module;

	/*
	 * Schema information about this YANG option (e.g. type, default value,
	 * etc).
	 */
	struct lys_node *snode;

	/* Pointer to the parent option (presence container or list). */
	struct nb_option *parent;

	/* Pointer to the nearest parent list, if any. */
	struct nb_option *parent_list;

	/* Full XPath of this YANG option, without predicates. */
	char xpath[XPATH_MAXLEN];

	/* Priority - lower priorities are processed first. */
	uint32_t priority;

	/* Callbacks implemented for this option. */
	struct nb_callbacks cbs;

#ifdef HAVE_CONFD
	/* ConfD hash value corresponding to this schema node. */
	int confd_hash;
#endif
};
RB_HEAD(nb_options, nb_option);
RB_PROTOTYPE(nb_options, nb_option, entry, nb_option_compare);

/* Northbound error codes. */
enum { NB_OK = 0,
       NB_ERR,
       NB_ERR_NO_CHANGES,
       NB_ERR_NOT_FOUND,
       NB_ERR_LOCKED,
       NB_ERR_RESOURCE,
};

/* Default priority. */
#define NB_DFLT_PRIORITY (UINT32_MAX / 2)

/* Default maximum of configuration rollbacks. */
#define NB_DLFT_MAX_CONFIG_ROLLBACKS 20

/* Possible formats in which a configuration can be displayed. */
enum nb_cfg_format {
	NB_CFG_FMT_CMDS = 0,
	NB_CFG_FMT_JSON,
	NB_CFG_FMT_XML,
};

/* Northbound clients. */
enum nb_client {
	NB_CLIENT_CLI = 0,
	NB_CLIENT_CONFD,
	NB_CLIENT_SYSREPO,
};

struct nb_config_change {
	RB_ENTRY(nb_config_change) entry;
	enum nb_operation operation;
	struct nb_option *option;
	char xpath[XPATH_MAXLEN];
	const struct lyd_node *dnode;
	union nb_resource resource;
	bool prepare_ok;
};
RB_HEAD(nb_config_changes, nb_config_change);
RB_PROTOTYPE(nb_config_changes, nb_config_change, entry,
	     nb_config_change_compare);

struct nb_transaction {
	enum nb_client client;
	char comment[80];
	struct lyd_node *config;
	struct nb_config_changes changes;
};

DECLARE_HOOK(nb_notification_send, (const char *xpath, struct list *arguments),
	     (xpath, arguments))

extern int debug_northbound;
extern struct lyd_node *running_config;
extern struct lyd_node *candidate_config;

/* northbound.c */
extern struct nb_option *nb_option_new(struct yang_module *module,
				       struct lys_node *snode);
extern void nb_option_del(struct nb_option *option);
extern struct nb_option *nb_option_find(const char *xpath);
extern void nb_config_init(struct lyd_node **config);
extern void nb_config_free(struct lyd_node **config);
extern struct lyd_node *nb_config_dup(const struct lyd_node *config);
extern int nb_config_edit(struct lyd_node *config, struct nb_option *option,
			  enum nb_operation operation, const char *xpath,
			  struct yang_data *previous, struct yang_data *data);
extern struct lyd_node *nb_config_get(const struct lyd_node *config,
				      const char *xpath);
extern struct lyd_node *nb_config_get_running(const char *xpath);
extern bool nb_config_exists(const struct lyd_node *config, const char *xpath);
extern int nb_candidate_validate(struct lyd_node **config);
extern int nb_candidate_commit(struct lyd_node *config, enum nb_client client,
			       bool save_transaction, char *comment);
extern int nb_db_transaction_save(struct nb_transaction *transaction);
extern struct lyd_node *nb_db_transaction_load(uint32_t transaction_id);
extern int nb_db_set_max_transactions(unsigned int max);
extern bool nb_operation_is_valid(enum nb_operation operation,
				  const struct lys_node *snode);
extern int nb_notification_send(const char *xpath, struct list *arguments);
extern const char *nb_event_name(enum nb_event event);
extern const char *nb_operation_name(enum nb_operation operation);
extern const char *nb_client_name(enum nb_client client);
extern void nb_load_callbacks(struct nb_option[], size_t size);
extern void nb_validate_callbacks(void);
extern void nb_init(void);
extern void nb_terminate(void);

/* northbound_cli.c */
struct cli_config_change {
	char xpath[XPATH_MAXLEN];
	const char *value;
	enum nb_operation operation;
};

extern int nb_cli_cfg_change(struct vty *vty, char *xpath_list,
			     struct cli_config_change changes[], size_t size);
extern int nb_cli_rpc(const char *xpath, struct list *input,
		      struct list *output);
extern void nb_cli_show_dnode_cmds(struct vty *vty, struct lyd_node *dnode,
				   bool show_defaults);
extern void nb_cli_show_config(struct vty *vty, struct lyd_node *config,
			       enum nb_cfg_format format, bool show_defaults);
extern void nb_transactional_cli_install_default(int node);
extern void nb_transactional_cli_init(void);

#endif /* _FRR_NORTHBOUND_H_ */
