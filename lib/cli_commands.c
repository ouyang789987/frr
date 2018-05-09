/*
 * Copyright (C) 1997, 1998, 1999 Kunihiro Ishiguro <kunihiro@zebra.org>
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

#include "if.h"
#include "vrf.h"
#include "log.h"
#include "prefix.h"
#include "command.h"
#include "northbound.h"
#include "libfrr.h"

#include "cli_commands.h"
#ifndef VTYSH_EXTRACT_PL
#include "lib/cli_commands_clippy.c"
#endif

/*
 * XPath: /frr-interface:lib/interface
 */
DEFPY (interface,
       interface_cmd,
       "interface IFNAME [vrf NAME$vrfname]",
       "Select an interface to configure\n"
       "Interface's name\n"
       VRF_CMD_HELP_STR)
{
	char xpath_list[XPATH_MAXLEN];
	struct cli_config_change changes[] = {
		{
			.xpath = ".",
			.operation = NB_OP_CREATE,
		},
	};
	vrf_id_t vrf_id;
	struct interface *ifp;
	int ret;

	if (vrfname == NULL)
		vrfname = "Default-IP-Routing-Table";

	/*
	 * This command requires special handling to maintain backward
	 * compatibility. If a VRF name is not specified, it means we're willing
	 * to accept any interface with the given name on any VRF. If no
	 * interface is found, then a new one should be created on the default
	 * VRF.
	 */
	VRF_GET_ID(vrf_id, vrfname);
	ifp = if_lookup_by_name_all_vrf(ifname);
	if (ifp) {
		if (ifp->vrf_id == vrf_id || vrf_id == VRF_DEFAULT) {
			strlcpy(xpath_list, ifp->xpath, sizeof(xpath_list));
		} else {
			vty_out(vty, "%% interface %s not in %s\n", ifname,
				vrfname);
			return CMD_WARNING_CONFIG_FAILED;
		}
	} else {
		snprintf(xpath_list, sizeof(xpath_list),
			 "/frr-interface:lib/interface[name='%s'][vrf='%s']",
			 ifname, vrfname);
	}

	ret = nb_cli_cfg_change(vty, xpath_list, changes, array_size(changes));
	if (ret == CMD_SUCCESS) {
		VTY_PUSH_XPATH(INTERFACE_NODE, xpath_list);

		/*
		 * For backward compatibility with old commands we still need
		 * to use the qobj infrastructure. This can be removed once
		 * all interface-level commands are converted to the new
		 * northbound model.
		 */
		ifp = if_lookup_by_name(ifname, vrf_id);
		VTY_PUSH_CONTEXT(INTERFACE_NODE, ifp);
	}

	return ret;
}

/* TODO: should be DEFPY_NOSH */
DEFPY (no_interface,
       no_interface_cmd,
       "no interface IFNAME [vrf NAME$vrfname]",
       NO_STR
       "Delete a pseudo interface's configuration\n"
       "Interface's name\n"
       VRF_CMD_HELP_STR)
{
	char xpath_list[XPATH_MAXLEN];
	struct cli_config_change changes[] = {
		{
			.xpath = ".",
			.operation = NB_OP_CREATE,
		},
	};

	if (vrfname == NULL)
		vrfname = "Default-IP-Routing-Table";

	snprintf(xpath_list, sizeof(xpath_list),
		 "/frr-interface:lib/interface[name='%s'][vrf='%s']", ifname,
		 vrfname);

	return nb_cli_cfg_change(vty, xpath_list, changes, array_size(changes));
}

void cli_show_interface(struct vty *vty, struct lyd_node *dnode,
			bool show_defaults)
{
	struct yang_data children[] = {
		{
			.xpath = "/frr-interface:lib/interface/name",
		},
		{
			.xpath = "/frr-interface:lib/interface/vrf",
		},
	};

	yang_parse_children(dnode, children, array_size(children));

	vty_out(vty, "!\n");
	vty_out(vty, "interface %s", children[0].value);
	if (!strmatch(children[1].value, VRF_DEFAULT_NAME))
		vty_out(vty, " vrf %s", children[1].value);
	vty_out(vty, "\n");
}

/*
 * XPath: /frr-interface:lib/interface
 */
DEFPY (interface_desc,
       interface_desc_cmd,
       "description LINE...",
       "Interface specific description\n"
       "Characters describing this interface\n")
{
	struct cli_config_change changes[] = {
		{
			.xpath = "./description",
			.operation = NB_OP_MODIFY,
			.value = argv_concat(argv, argc, 1),
		},
	};

	return nb_cli_cfg_change(vty, NULL, changes, array_size(changes));
}

DEFPY  (no_interface_desc,
	no_interface_desc_cmd,
	"no description",
	NO_STR
	"Interface specific description\n")
{
	struct cli_config_change changes[] = {
		{
			.xpath = "./description",
			.operation = NB_OP_DELETE,
		},
	};

	return nb_cli_cfg_change(vty, NULL, changes, array_size(changes));
}

void cli_show_interface_desc(struct vty *vty, struct lyd_node *dnode,
			     bool show_defaults)
{
	vty_out(vty, " description %s\n", yang_dnode_get_string(dnode));
}

/* -------------------------------------------------------------------------- */

static void if_autocomplete(vector comps, struct cmd_token *token)
{
	struct interface *ifp;
	struct vrf *vrf;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		FOR_ALL_INTERFACES (vrf, ifp) {
			vector_set(comps, XSTRDUP(MTYPE_COMPLETION, ifp->name));
		}
	}
}

static const struct cmd_variable_handler if_var_handlers[] = {
	{/* "interface NAME" */
	 .varname = "interface",
	 .completions = if_autocomplete},
	{.tokenname = "IFNAME", .completions = if_autocomplete},
	{.tokenname = "INTERFACE", .completions = if_autocomplete},
	{.completions = NULL}};

void if_cmd_init(void)
{
	if_northbound_init();

	cmd_variable_handler_register(if_var_handlers);

	install_element(CONFIG_NODE, &interface_cmd);
	install_element(CONFIG_NODE, &no_interface_cmd);

	install_default(INTERFACE_NODE);
	install_element(INTERFACE_NODE, &interface_desc_cmd);
	install_element(INTERFACE_NODE, &no_interface_desc_cmd);
}
