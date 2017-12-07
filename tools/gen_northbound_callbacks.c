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

#define REALLY_NEED_PLAIN_GETOPT 1

#include <zebra.h>

#include <unistd.h>

#include "yang.h"
#include "northbound.h"

static const char *module_name;

static void __attribute__((noreturn)) usage(int status)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-h] MODULE [AUGMENTED-MODULE]...\n", __progname);
	exit(status);
}

static struct nb_callback_info {
	int operation;
	bool optional;
	char return_type[32];
	char return_value[32];
	char arguments[128];
} nb_callbacks[] = {
	{
		.operation = NB_OP_CREATE,
		.return_type = "int ",
		.return_value = "NB_OK",
		.arguments =
			"enum nb_event event, const struct lyd_node *dnode, union nb_resource *resource",
	},
	{
		.operation = NB_OP_MODIFY,
		.return_type = "int ",
		.return_value = "NB_OK",
		.arguments =
			"enum nb_event event, const struct lyd_node *dnode, union nb_resource *resource",
	},
	{
		.operation = NB_OP_DELETE,
		.return_type = "int ",
		.return_value = "NB_OK",
		.arguments = "enum nb_event event, const struct lyd_node *dnode",
	},
	{
		.operation = NB_OP_MOVE,
		.return_type = "int ",
		.return_value = "NB_OK",
		.arguments = "enum nb_event event, const struct lyd_node *dnode",
	},
	{
		.operation = NB_OP_APPLY_FINISH,
		.optional = true,
		.return_type = "void ",
		.return_value = "",
		.arguments = "void",
	},
	{
		.operation = NB_OP_GET_ELEM,
		.return_type = "struct yang_data *",
		.return_value = "NULL",
		.arguments = "const char *xpath, void *list_entry",
	},
	{
		.operation = NB_OP_GET_NEXT,
		.return_type = "void *",
		.return_value = "NULL",
		.arguments = "void *element",
	},
	{
		.operation = NB_OP_GET_KEYS,
		.return_type = "int ",
		.return_value = "NB_OK",
		.arguments = "void *element, struct yang_list_keys *keys",
	},
	{
		.operation = NB_OP_LOOKUP_ENTRY,
		.return_type = "void *",
		.return_value = "NULL",
		.arguments = "struct yang_list_keys *keys",
	},
	{
		.operation = NB_OP_RPC,
		.return_type = "int ",
		.return_value = "NB_OK",
		.arguments =
			"const char *xpath, const struct list *input, struct list *output",
	},
	{
		/* sentinel */
		.operation = -1,
	},
};

static void generate_callback_name(struct lys_node *snode,
				   enum nb_operation operation, char *buffer,
				   size_t size)
{
	struct list *snodes;
	struct listnode *ln;
	char *p;

	snodes = list_new();
	for (; snode; snode = lys_parent(snode)) {
		/* Skip schema-only snodes. */
		if (snode->nodetype & (LYS_USES | LYS_CHOICE | LYS_CASE
				       | LYS_INPUT | LYS_OUTPUT))
			continue;

		listnode_add_head(snodes, snode);
	}

	memset(buffer, 0, size);
	for (ALL_LIST_ELEMENTS_RO(snodes, ln, snode)) {
		strlcat(buffer, snode->name, size);
		strlcat(buffer, "_", size);
	}
	strlcat(buffer, nb_operation_name(operation), size);
	list_delete_and_null(&snodes);

	/* Replace all occurrences of '-' by '_'. */
	p = buffer;
	while ((p = strchr(p, '-')) != NULL)
		*p++ = '_';
}

static void generate_callbacks(struct yang_module *module,
			       struct lys_node *snode, void *arg)
{
	struct lys_node_container *container;
	bool first = true;

	if (!strmatch(snode->module->name, module_name))
		return;

	switch (snode->nodetype) {
	case LYS_CONTAINER:
		container = (struct lys_node_container *)snode;
		if (!container->presence)
			return;
		break;
	case LYS_LEAF:
	case LYS_LEAFLIST:
	case LYS_LIST:
	case LYS_NOTIF:
	case LYS_RPC:
		break;
	default:
		return;
	}

	for (struct nb_callback_info *cb = &nb_callbacks[0];
	     cb->operation != -1; cb++) {
		char cb_name[BUFSIZ];

		if (cb->optional
		    || !nb_operation_is_valid(cb->operation, snode))
			continue;

		if (first) {
			char *xpath;
			xpath = lys_data_path(snode);
			printf("/*\n"
			       " * XPath: %s\n"
			       " */\n",
			       xpath);
			free(xpath);
			first = false;
		}

		generate_callback_name(snode, cb->operation, cb_name,
				       sizeof(cb_name));
		printf("static %s%s(%s)\n"
		       "{\n"
		       "\t/* TODO: implement me. */\n"
		       "\treturn %s;\n"
		       "}\n\n",
		       nb_callbacks[cb->operation].return_type, cb_name,
		       nb_callbacks[cb->operation].arguments,
		       nb_callbacks[cb->operation].return_value);
	}
}

static void generate_nb_options(struct yang_module *module,
			       struct lys_node *snode, void *arg)
{
	struct lys_node_container *container;
	bool first = true;

	if (!strmatch(snode->module->name, module_name))
		return;

	switch (snode->nodetype) {
	case LYS_CONTAINER:
		container = (struct lys_node_container *)snode;
		if (!container->presence)
			return;
		break;
	case LYS_LEAF:
	case LYS_LEAFLIST:
	case LYS_LIST:
	case LYS_NOTIF:
	case LYS_RPC:
		break;
	default:
		return;
	}

	for (struct nb_callback_info *cb = &nb_callbacks[0];
	     cb->operation != -1; cb++) {
		char cb_name[BUFSIZ];

		if (cb->optional
		    || !nb_operation_is_valid(cb->operation, snode))
			continue;

		if (first) {
			char *xpath;
			xpath = lys_data_path(snode);
			printf("\t\t{\n"
			       "\t\t\t.xpath = \"%s\",\n",
			       xpath);
			free(xpath);
			first = false;
		}

		generate_callback_name(snode, cb->operation, cb_name,
				       sizeof(cb_name));
		printf("\t\t\t.cbs.%s = %s,\n", nb_operation_name(cb->operation), cb_name);
	}

	if (!first)
		printf("\t\t},\n");
}

int main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "h")) != -1) {
		switch (opt) {
		case 'h':
			usage(EXIT_SUCCESS);
			/* NOTREACHED */
		default:
			usage(EXIT_FAILURE);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0)
		usage(EXIT_FAILURE);

	yang_init(NULL, 0);

	module_name = argv[0];
	while (argc > 0) {
		yang_module_new(argv[0]);
		argc--;
		argv++;
	}

	/* Generate callback functions. */
	yang_snodes_iterate(generate_callbacks, NULL);

	/* Generate initialization function. */
	printf("/*\n"
	       " * Initialize northbound options.\n"
	       " */\n"
	       "void xxx_northbound_init(void)\n"
	       "{\n"
	       "\t/* clang-format off */\n"
	       "\tstruct nb_option options[] = {\n");
	yang_snodes_iterate(generate_nb_options, NULL);
	printf("\t};\n"
	       "\t/* clang-format on */\n\n"
	       "\tnb_load_callbacks(options, array_size(options));\n"
	       "}\n");

	yang_terminate();

	return 0;
}
