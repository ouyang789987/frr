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

#ifndef _FRR_YANG_H_
#define _FRR_YANG_H_

#include "memory.h"

#include <libyang/libyang.h>
#ifdef HAVE_SYSREPO
#include <sysrepo.h>
#endif

DECLARE_MTYPE(YANG_MODULE)
DECLARE_MTYPE(YANG_DATA)

/* Maximum XPath length. */
#define XPATH_MAXLEN 256

/* Maximum list key length. */
#define LIST_MAXKEYS 8

/* Maximum list key length. */
#define LIST_MAXKEYLEN 128

/* Maximum string length of an YANG value. */
#define YANG_VALUE_MAXLEN 1024

struct yang_module {
	RB_ENTRY(yang_module) entry;
	const char *name;
	const struct lys_module *info;
#ifdef HAVE_CONFD
	int confd_hash;
#endif
#ifdef HAVE_SYSREPO
	sr_subscription_ctx_t *sr_subscription;
#endif
};
RB_HEAD(yang_modules, yang_module);
RB_PROTOTYPE(yang_modules, yang_module, entry, yang_module_compare);

struct yang_data {
	/* XPath identifier of the data element. */
	char xpath[XPATH_MAXLEN];

	/*
	 * Schema information (necessary to interpret certain values like
	 * enums).
	 */
	const struct lys_node *snode;

	/* Value encoded as a raw string. */
	char *value;
};

struct yang_list_key {
	/*
	 * Schema information (necessary to interpret certain values like
	 * enums).
	 */
	struct lys_node *snode;

	/* Value encoded as a raw string. */
	char value[LIST_MAXKEYLEN];
};

struct yang_list_keys {
	/* Number os keys (max: LIST_MAXKEYS). */
	uint8_t num;

	/* Key values. */
	struct yang_list_key key[LIST_MAXKEYS];
};

extern struct ly_ctx *ly_ctx;
extern struct yang_modules yang_modules;

/* prototypes */
extern struct yang_module *yang_module_new(const char *module_name);
extern void yang_module_del(struct yang_module *module);
extern struct yang_module *yang_module_find(const char *module_name);
extern void yang_snodes_iterate(void (*func)(struct yang_module *,
					     struct lys_node *, void *),
				void *arg);
extern struct lys_node *yang_find_real_parent(const struct lys_node *snode);
extern struct lys_node *yang_find_parent_list(const struct lys_node *snode);
extern const char *yang_default_value(const char *xpath);
extern bool yang_node_is_default(const struct lyd_node *dnode);
extern bool yang_node_has_value(const struct lys_node *snode);
extern struct yang_data *yang_data_new(const char *xpath, const char *value);
extern void yang_data_free(struct yang_data *data);
extern struct list *yang_data_list_new(void);
extern bool yang_parse_children(const struct lyd_node *dnode,
				struct yang_data children[], size_t size);
extern void *yang_dnode_lookup_list_entry(const struct lyd_node *dnode);
extern void yang_dnode_get_keys(const struct lyd_node *dnode,
				struct yang_list_keys *keys);
extern int yang_xpath_get_keys(const char *xpath, struct yang_list_keys *keys);
extern void yang_init(const char *modules[], size_t nmodules);
extern void yang_terminate(void);

#endif /* _FRR_YANG_H_ */
