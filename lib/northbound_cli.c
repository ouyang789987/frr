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
#include "version.h"
#include "log.h"
#include "command.h"
#include "termtable.h"
#include "db.h"
#include "northbound.h"
#ifndef VTYSH_EXTRACT_PL
#include "lib/northbound_cli_clippy.c"
#endif

static int nb_cli_discard(struct vty *vty);
static int nb_cli_commit(struct vty *vty, char *comment);

static void vty_show_libyang_errors(struct vty *vty)
{
	struct ly_err_item *ei = ly_err_first(ly_ctx);
	const char *path;

	if (ei == NULL)
		return;

	vty_out(vty, "\n");
	for (; ei; ei = ei->next)
		vty_out(vty, "%s\n", ei->msg);

	path = ly_errpath(ly_ctx);
	if (path)
		vty_out(vty, "YANG path: %s\n", path);

	ly_err_clean(ly_ctx, NULL);
}

int nb_cli_cfg_change(struct vty *vty, char *xpath_base,
		      struct cli_config_change changes[], size_t size)
{
	struct lyd_node *candidate_transitory;
	bool error = false;
	int ret;

	VTY_CHECK_XPATH();

	/*
	 * Create a copy of the candidate configuration. For consistency, we
	 * need to ensure that either all changes made by the command are
	 * accepted or none are.
	 */
	candidate_transitory = nb_config_dup(candidate_config);

	/* Edit candidate configuration. */
	for (unsigned int i = 0; i < size; i++) {
		struct cli_config_change *change = &changes[i];
		struct nb_option *option;
		char xpath[XPATH_MAXLEN];
		struct yang_data *data;
		int ret;

		/* Handle relative XPaths. */
		memset(xpath, 0, sizeof(xpath));
		if (vty->xpath_index > 0
		    && ((xpath_base && xpath_base[0] == '.')
			|| change->xpath[0] == '.'))
			strlcpy(xpath, VTY_GET_XPATH, sizeof(xpath));
		if (xpath_base) {
			if (xpath_base[0] == '.')
				xpath_base++;
			strlcat(xpath, xpath_base, sizeof(xpath));
		}
		if (change->xpath[0] == '.')
			strlcat(xpath, change->xpath + 1, sizeof(xpath));
		else
			strlcpy(xpath, change->xpath, sizeof(xpath));

		option = nb_option_find(xpath);
		if (option == NULL) {
			zlog_err("%s: configuration option not found: %s",
				 __func__, xpath);
			error = true;
			break;
		}

		if (change->value == NULL)
			change->value = yang_default_value(xpath);
		data = yang_data_new(xpath, change->value);

		/*
		 * Ignore "not found" errors when editing the candidate
		 * configuration.
		 */
		ret = nb_config_edit(candidate_transitory, option,
				     change->operation, xpath, NULL, data);
		yang_data_free(data);
		if (ret != NB_OK && ret != NB_ERR_NOT_FOUND) {
			error = true;
			break;
		}
	}

	if (error)
		nb_config_free(&candidate_transitory);
	else {
		nb_config_free(&candidate_config);
		candidate_config = candidate_transitory;
	}

	switch (frr_get_cli_mode()) {
	case FRR_CLI_CLASSIC:
		if (error) {
			vty_out(vty, "%% Configuration failed.\n\n");
			vty_show_libyang_errors(vty);
			return CMD_WARNING;
		}

		ret = nb_candidate_commit(candidate_config, NB_CLIENT_CLI,
					  false, NULL);
		if (ret != NB_OK && ret != NB_ERR_NO_CHANGES) {
			vty_out(vty, "%% Configuration failed.\n\n");
			vty_out(vty, "Please check the log files for more details.\n");
			return CMD_WARNING;
		}
		break;
	case FRR_CLI_TRANSACTIONAL:
		if (error) {
			vty_out(vty,
				"%% Failed to edit candidate configuration.\n\n");
			vty_show_libyang_errors(vty);
			return CMD_WARNING;
		}
		break;
	default:
		zlog_err("%s: unknown cli mode", __func__);
		exit(1);
	}

	return CMD_SUCCESS;
}

int nb_cli_rpc(const char *xpath, struct list *input, struct list *output)
{
	struct nb_option *option;
	int ret;

	option = nb_option_find(xpath);
	if (!option) {
		zlog_warn("%s: unknown data [xpath %s]", __func__, xpath);
		return CMD_WARNING;
	}

	ret = option->cbs.rpc(xpath, input, output);
	switch (ret) {
	case NB_OK:
		return CMD_SUCCESS;
	default:
		return CMD_WARNING;
	}
}

static int nb_cli_commit(struct vty *vty, char *comment)
{
	int ret;

	if (vty_exclusive_lock != NULL && vty_exclusive_lock != vty) {
		vty_out(vty, "%% Configuration is locked by another VTY.\n\n");
		return CMD_WARNING;
	}

	ret = nb_candidate_commit(candidate_config, NB_CLIENT_CLI, true,
				  comment);
	if (comment)
		XFREE(MTYPE_TMP, comment);

	/* Map northbound return code to CLI return code. */
	switch (ret) {
	case NB_OK:
		vty_out(vty, "%% Configuration committed successfully.\n\n");
		return CMD_SUCCESS;
	case NB_ERR_NO_CHANGES:
		vty_out(vty, "%% No configuration changes to commit.\n\n");
		return CMD_SUCCESS;
	default:
		vty_out(vty, "%% Failed to commit candidate configuration\n\n");
		vty_out(vty, "Please check the log files for more details.\n");
		return CMD_WARNING;
	}
}

static int nb_cli_commit_check(struct vty *vty)
{
	int ret;

	ret = nb_candidate_validate(&candidate_config);
	if (ret != NB_OK) {
		vty_out(vty, "%% Failed to validate candidate configuration.\n\n");
		vty_show_libyang_errors(vty);
		return CMD_WARNING;
	}

	vty_out(vty, "%% Candidate configuration validated successfully.\n\n");

	return CMD_SUCCESS;
}

static int nb_cli_discard(struct vty *vty)
{
	nb_config_free(&candidate_config);
	candidate_config = nb_config_dup(running_config);

	return CMD_SUCCESS;
}

static int nb_cli_candidate_load_file(struct vty *vty,
				      enum nb_cfg_format format,
				      const char *path, bool replace)
{
	struct lyd_node *loaded_config;
	int ly_format;

	switch (format) {
	case NB_CFG_FMT_CMDS:
		if (replace) {
			/*
			 * XXX: the candidate will be reset even when we fail to
			 * load the new configuration.
			 */
			nb_config_free(&candidate_config);
			nb_config_init(&candidate_config);
		}
		vty_read_config(path, config_default);
		break;
	case NB_CFG_FMT_JSON:
	case NB_CFG_FMT_XML:
		ly_format = (format == NB_CFG_FMT_JSON) ? LYD_JSON : LYD_XML;
		loaded_config =
			lyd_parse_path(ly_ctx, path, ly_format, LYD_OPT_CONFIG);
		if (loaded_config == NULL) {
			zlog_warn("%s: lyd_parse_path() failed", __func__);
			vty_out(vty, "%% Failed to load configuration:\n\n");
			vty_show_libyang_errors(vty);
			return CMD_WARNING;
		}

		if (replace) {
			nb_config_free(&candidate_config);
			candidate_config = loaded_config;
		} else if (lyd_merge(candidate_config, loaded_config, 0) != 0) {
			zlog_warn("%s: lyd_merge() failed", __func__);
			vty_out(vty,
				"%% Failed to merge the loaded configuration:\n\n");
			vty_show_libyang_errors(vty);
			return CMD_WARNING;
		}
		break;
	}

	return CMD_SUCCESS;
}

static int nb_cli_candidate_load_transaction(struct vty *vty,
					     uint32_t transaction_id,
					     bool replace)
{
	struct lyd_node *loaded_config;

	loaded_config = nb_db_transaction_load(transaction_id);
	if (loaded_config == NULL) {
		vty_out(vty, "%% Transaction %u does not exist.\n\n",
			transaction_id);
		return CMD_WARNING;
	}

	if (replace) {
		nb_config_free(&candidate_config);
		candidate_config = loaded_config;
	} else if (lyd_merge(candidate_config, loaded_config, 0) != 0) {
		zlog_warn("%s: lyd_merge() failed", __func__);
		vty_out(vty, "%% Failed to merge the loaded configuration:\n\n");
		vty_show_libyang_errors(vty);
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

void nb_cli_show_dnode_cmds(struct vty *vty, struct lyd_node *root,
			    bool with_defaults)
{
	struct lyd_node *next, *child;

	LY_TREE_DFS_BEGIN (root, next, child) {
		struct nb_option *option;

		option = child->schema->priv;

		/* Skip default values. */
		if (!with_defaults && yang_node_is_default(child))
			goto next;

		if (option->cbs.cli_show)
			(*option->cbs.cli_show)(vty, child, with_defaults);

	next:
		LY_TREE_DFS_END(root, next, child);
	}
}

static void nb_cli_show_config_cmds(struct vty *vty, struct lyd_node *config,
				    bool with_defaults)
{
	struct lyd_node *root;

	vty_out(vty, "Configuration:\n");
	vty_out(vty, "!\n");
	vty_out(vty, "frr version %s\n", FRR_VER_SHORT);
	vty_out(vty, "frr defaults %s\n", DFLT_NAME);

	LY_TREE_FOR (config, root)
		nb_cli_show_dnode_cmds(vty, root, with_defaults);

	vty_out(vty, "!\n");
	vty_out(vty, "end\n");
}

static void nb_cli_show_config_json(struct vty *vty, struct lyd_node *config,
				    bool with_defaults)
{
	char *strp;
	int options;

	options = LYP_FORMAT | LYP_WITHSIBLINGS;
	if (with_defaults)
		options |= LYP_WD_ALL;
	else
		options |= LYP_WD_TRIM;

	if (lyd_print_mem(&strp, config, LYD_JSON, options) == 0 && strp) {
		vty_out(vty, "%s", strp);
		free(strp);
	}
}

static void nb_cli_show_config_xml(struct vty *vty, struct lyd_node *config,
				   bool with_defaults)
{
	char *strp;
	int options;

	options = LYP_FORMAT | LYP_WITHSIBLINGS;
	if (with_defaults)
		options |= LYP_WD_ALL;
	else
		options |= LYP_WD_TRIM;

	if (lyd_print_mem(&strp, config, LYD_XML, options) == 0 && strp) {
		vty_out(vty, "%s", strp);
		free(strp);
	}
}

void nb_cli_show_config(struct vty *vty, struct lyd_node *config,
			enum nb_cfg_format format, bool with_defaults)
{
	switch (format) {
	case NB_CFG_FMT_CMDS:
		nb_cli_show_config_cmds(vty, config, with_defaults);
		break;
	case NB_CFG_FMT_JSON:
		nb_cli_show_config_json(vty, config, with_defaults);
		break;
	case NB_CFG_FMT_XML:
		nb_cli_show_config_xml(vty, config, with_defaults);
		break;
	}
}

static int nb_write_config(struct lyd_node *config, enum nb_cfg_format format,
			   char *path, size_t pathlen)
{
	int fd;
	struct vty *file_vty;

	snprintf(path, pathlen, "/tmp/frr.tmp.XXXXXXXX");
	fd = mkstemp(path);
	if (fd < 0) {
		zlog_warn("%s: mkstemp() failed: %s", __func__,
			  safe_strerror(errno));
		return -1;
	}

	/* Make vty for configuration file. */
	file_vty = vty_new();
	file_vty->wfd = fd;
	file_vty->type = VTY_FILE;
	if (config)
		nb_cli_show_config(file_vty, config, format, false);
	vty_close(file_vty);

	return 0;
}

static int nb_cli_show_config_compare(struct vty *vty, struct lyd_node *config1,
				      struct lyd_node *config2,
				      enum nb_cfg_format format)
{
	char config1_path[MAXPATHLEN];
	char config2_path[MAXPATHLEN];
	char command[BUFSIZ];
	FILE *fp;
	char line[1024];
	int lineno = 0;

	if (nb_write_config(config1, format, config1_path, sizeof(config1_path))
	    != 0) {
		vty_out(vty, "%% Failed to process configurations.\n\n");
		return CMD_WARNING;
	}
	if (nb_write_config(config2, format, config2_path, sizeof(config2_path))
	    != 0) {
		vty_out(vty, "%% Failed to process configurations.\n\n");
		unlink(config1_path);
		return CMD_WARNING;
	}

	snprintf(command, sizeof(command), "diff -u %s %s", config1_path,
		 config2_path);
	fp = popen(command, "r");
	if (fp == NULL) {
		vty_out(vty, "%% Failed to generate configuration diff.\n\n");
		unlink(config1_path);
		unlink(config2_path);
		return CMD_WARNING;
	}
	/* Print diff line by line. */
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (lineno++ < 2)
			continue;
		vty_out(vty, "%s", line);
	}
	pclose(fp);

	unlink(config1_path);
	unlink(config2_path);

	return CMD_SUCCESS;
}

DEFPY (config_commit,
       config_commit_cmd,
       "commit",
       "Commit changes into the running configuration\n")
{
	return nb_cli_commit(vty, NULL);
}

DEFPY (config_commit_comment,
       config_commit_comment_cmd,
       "commit comment LINE...",
       "Commit changes into the running configuration\n"
       "Assign a comment to this commit\n"
       "Comment for this commit (Max 80 characters)\n")
{
	char *comment;
	int idx = 0;

	argv_find(argv, argc, "LINE", &idx);
	comment = argv_concat(argv, argc, idx);

	return nb_cli_commit(vty, comment);
}

DEFPY (config_commit_check,
       config_commit_check_cmd,
       "commit check",
       "Commit changes into the running configuration\n"
       "Check if the configuration changes are valid\n")
{
	return nb_cli_commit_check(vty);
}

DEFPY (config_discard,
       config_discard_cmd,
       "discard",
       "Discard changes in the candidate configuration\n")
{
	return nb_cli_discard(vty);
}

DEFPY (config_load,
       config_load_cmd,
       "configuration load\
          <\
	    file [<json$json|xml$xml>] FILENAME$filename\
	    |transaction (1-4294967296)$tid\
	  >\
	  [replace$replace]",
       "Configuration related settings\n"
       "Load configuration into candidate\n"
       "Load configuration file into candidate\n"
       "Load configuration file in JSON format\n"
       "Load configuration file in XML format\n"
       "Configuration file name (full path)\n"
       "Load configuration from transaction into candidate\n"
       "Transaction ID\n"
       "Replace instead of merge\n")
{
	if (filename) {
		enum nb_cfg_format format;

		if (json)
			format = NB_CFG_FMT_JSON;
		else if (xml)
			format = NB_CFG_FMT_XML;
		else
			format = NB_CFG_FMT_CMDS;

		return nb_cli_candidate_load_file(vty, format, filename,
						  !!replace);
	}

	return nb_cli_candidate_load_transaction(vty, tid, !!replace);
}

DEFPY (show_config_running,
       show_config_running_cmd,
       "show configuration running [<json$json|xml$xml>] [with-defaults$with_defaults]",
       SHOW_STR
       "Configuration information\n"
       "Running configuration\n"
       "Change output format to JSON\n"
       "Change output format to XML\n"
       "Show default values\n")

{
	enum nb_cfg_format format;

	if (json)
		format = NB_CFG_FMT_JSON;
	else if (xml)
		format = NB_CFG_FMT_XML;
	else
		format = NB_CFG_FMT_CMDS;

	nb_cli_show_config(vty, running_config, format, !!with_defaults);

	return CMD_SUCCESS;
}

DEFPY (show_config_candidate,
       show_config_candidate_cmd,
       "show configuration candidate [<json$json|xml$xml>]\
          [<\
	    with-defaults$with_defaults\
	    |changes$changes\
	   >]",
       SHOW_STR
       "Configuration information\n"
       "Candidate configuration\n"
       "Change output format to JSON\n"
       "Change output format to XML\n"
       "Show default values\n"
       "Show changes applied in the candidate configuration\n")

{
	enum nb_cfg_format format;

	if (json)
		format = NB_CFG_FMT_JSON;
	else if (xml)
		format = NB_CFG_FMT_XML;
	else
		format = NB_CFG_FMT_CMDS;

	if (changes)
		return nb_cli_show_config_compare(vty, running_config,
						  candidate_config, format);

	nb_cli_show_config(vty, candidate_config, format, !!with_defaults);

	return CMD_SUCCESS;
}

DEFPY (show_config_compare,
       show_config_compare_cmd,
       "show configuration compare\
          <\
	    candidate$c1_candidate\
	    |running$c1_running\
	    |transaction (1-4294967296)$c1_tid\
	  >\
          <\
	    candidate$c2_candidate\
	    |running$c2_running\
	    |transaction (1-4294967296)$c2_tid\
	  >\
	  [<json$json|xml$xml>]",
       SHOW_STR
       "Configuration information\n"
       "Compare two different configurations\n"
       "Candidate configuration\n"
       "Running configuration\n"
       "Configuration transaction\n"
       "Transaction ID\n"
       "Candidate configuration\n"
       "Running configuration\n"
       "Configuration transaction\n"
       "Transaction ID\n"
       "Change output format to JSON\n"
       "Change output format to XML\n")
{
	enum nb_cfg_format format;
	struct lyd_node *config1;
	struct lyd_node *config2;

	if (c1_candidate)
		config1 = candidate_config;
	else if (c1_running)
		config1 = running_config;
	else {
		config1 = nb_db_transaction_load(c1_tid);
		if (config1 == NULL) {
			vty_out(vty, "%% Transaction %u does not exist\n\n",
				(unsigned int)c1_tid);
			return CMD_WARNING;
		}
	}

	if (c2_candidate)
		config2 = candidate_config;
	else if (c2_running)
		config2 = running_config;
	else {
		config2 = nb_db_transaction_load(c2_tid);
		if (config2 == NULL) {
			vty_out(vty, "%% Transaction %u does not exist\n\n",
				(unsigned int)c2_tid);
			return CMD_WARNING;
		}
	}

	if (json)
		format = NB_CFG_FMT_JSON;
	else if (xml)
		format = NB_CFG_FMT_XML;
	else
		format = NB_CFG_FMT_CMDS;

	return nb_cli_show_config_compare(vty, config1, config2, format);
}

/*
 * Stripped down version of the "show configuration compare" command.
 * The "candidate" option is not present so the command can be installed in
 * the enable node.
 */
DEFPY (show_config_compare_without_candidate,
       show_config_compare_without_candidate_cmd,
       "show configuration compare\
          <\
	    running$c1_running\
	    |transaction (1-4294967296)$c1_tid\
	  >\
          <\
	    running$c2_running\
	    |transaction (1-4294967296)$c2_tid\
	  >\
	 [<json$json|xml$xml>]",
       SHOW_STR
       "Configuration information\n"
       "Compare two different configurations\n"
       "Running configuration\n"
       "Configuration transaction\n"
       "Transaction ID\n"
       "Running configuration\n"
       "Configuration transaction\n"
       "Transaction ID\n"
       "Change output format to JSON\n"
       "Change output format to XML\n")
{
	enum nb_cfg_format format;
	struct lyd_node *config1;
	struct lyd_node *config2;

	if (c1_running)
		config1 = running_config;
	else {
		config1 = nb_db_transaction_load(c1_tid);
		if (config1 == NULL) {
			vty_out(vty, "%% Transaction %u does not exist.\n\n",
				(unsigned int)c1_tid);
			return CMD_WARNING;
		}
	}

	if (c2_running)
		config2 = running_config;
	else {
		config2 = nb_db_transaction_load(c2_tid);
		if (config2 == NULL) {
			vty_out(vty, "%% Transaction %u does not exist.\n\n",
				(unsigned int)c2_tid);
			return CMD_WARNING;
		}
	}

	if (json)
		format = NB_CFG_FMT_JSON;
	else if (xml)
		format = NB_CFG_FMT_XML;
	else
		format = NB_CFG_FMT_CMDS;

	return nb_cli_show_config_compare(vty, config1, config2, format);
}

DEFPY (config_database_max_transactions,
       config_database_max_transactions_cmd,
       "configuration database max-transactions (1-100)$max",
       "Configuration related settings\n"
       "Configuration database\n"
       "Set maximum number of transactions to store\n"
       "Number of transactions\n")
{
#ifdef HAVE_CONFIG_ROLLBACKS
	if (nb_db_set_max_transactions(max) != NB_OK) {
		vty_out(vty,
			"%% Failed to update the maximum number of transactions.\n\n");
		return CMD_WARNING;
	}
	vty_out(vty,
		"%% Maximum number of transactions updated successfully.\n\n");
#else
	vty_out(vty,
		"%% FRR was compiled without --enable-config-rollbacks.\n\n");
#endif /* HAVE_CONFIG_ROLLBACKS */

	return CMD_SUCCESS;
}

#ifdef HAVE_CONFIG_ROLLBACKS
static void nb_cli_show_transactions(struct vty *vty)
{
	struct ttable *tt;
	struct sqlite3_stmt *ss;

	/* Prepare table. */
	tt = ttable_new(&ttable_styles[TTSTYLE_BLANK]);
	ttable_add_row(tt, "Transaction ID|Client|Date|Comment");
	tt->style.cell.rpad = 2;
	tt->style.corner = '+';
	ttable_restyle(tt);
	ttable_rowseps(tt, 0, BOTTOM, true, '-');

	/* Send SQL query and parse the result. */
	ss = db_prepare(
		"                                                              \
		SELECT                                                         \
		  id, client, date, comment                                    \
		FROM                                                           \
		  transactions                                                 \
		ORDER BY                                                       \
		  id DESC                                                      \
		;");
	while (db_run(ss) == SQLITE_ROW) {
		int transaction_id;
		const char *client_name;
		const char *date;
		const char *comment;
		int ret;

		ret = db_loadf(ss, "%i%s%s%s", &transaction_id, &client_name,
			       &date, &comment);
		if (ret != 0)
			continue;

		ttable_add_row(tt, "%d|%s|%s|%s", transaction_id, client_name,
			       date, comment);
	}

	/* Dump the generated table. */
	if (tt->nrows > 1) {
		char *table;

		table = ttable_dump(tt, "\n");
		vty_out(vty, "%s\n", table);
		XFREE(MTYPE_TMP, table);
	} else
		vty_out(vty, "No configuration transactions to display.\n\n");

	ttable_del(tt);
	db_finalize(&ss);
}
#endif /* HAVE_CONFIG_ROLLBACKS */

DEFPY (show_config_transaction,
       show_config_transaction_cmd,
       "show configuration transaction\
         [(1-4294967296)$transaction_id [<json$json|xml$xml>] [changes$changes]]",
       SHOW_STR
       "Configuration information\n"
       "Configuration transaction\n"
       "Transaction ID\n"
       "Change output format to JSON\n"
       "Change output format to XML\n"
       "Show changes compared to the previous transaction\n")
{
#ifdef HAVE_CONFIG_ROLLBACKS
	if (transaction_id) {
		struct lyd_node *config;
		enum nb_cfg_format format;

		if (json)
			format = NB_CFG_FMT_JSON;
		else if (xml)
			format = NB_CFG_FMT_XML;
		else
			format = NB_CFG_FMT_CMDS;

		config = nb_db_transaction_load(transaction_id);
		if (config == NULL) {
			vty_out(vty, "%% Transaction %u does not exist.\n\n",
				(unsigned int)transaction_id);
			return CMD_WARNING;
		}

		if (changes) {
			struct lyd_node *prev_config;

			/* NOTE: this can be NULL. */
			prev_config =
				nb_db_transaction_load(transaction_id - 1);

			return nb_cli_show_config_compare(vty, prev_config,
							  config, format);
		}

		nb_cli_show_config(vty, config, format, 0);

		return CMD_SUCCESS;
	}

	nb_cli_show_transactions(vty);
#else
	vty_out(vty,
		"%% FRR was compiled without --enable-config-rollbacks.\n\n");
#endif /* HAVE_CONFIG_ROLLBACKS */

	return CMD_SUCCESS;
}

#ifdef HAVE_CONFIG_ROLLBACKS
static int nb_cli_rollback_configuration(struct vty *vty,
					 uint32_t transaction_id)
{
	struct lyd_node *config;
	char comment[80];
	int ret;

	config = nb_db_transaction_load(transaction_id);
	if (config == NULL) {
		vty_out(vty, "%% Transaction %u does not exist.\n\n",
			transaction_id);
		return CMD_WARNING;
	}

	snprintf(comment, sizeof(comment), "Rollback to transaction %u",
		 transaction_id);

	ret = nb_candidate_commit(config, NB_CLIENT_CLI, true, comment);
	switch (ret) {
	case NB_OK:
		vty_out(vty,
			"%% Configuration was successfully rolled back.\n\n");
		return CMD_SUCCESS;
	case NB_ERR_NO_CHANGES:
		vty_out(vty,
			"%% Aborting - no configuration changes detected.\n\n");
		return CMD_WARNING;
	default:
		vty_out(vty, "%% Rollback failed.\n\n");
		vty_out(vty, "Please check the log files for more details.\n");
		return CMD_WARNING;
	}
}
#endif /* HAVE_CONFIG_ROLLBACKS */

DEFPY (rollback_config,
       rollback_config_cmd,
       "rollback configuration (1-4294967296)$transaction_id",
       "Rollback to a previous state\n"
       "Running configuration\n"
       "Transaction ID\n")
{
#ifdef HAVE_CONFIG_ROLLBACKS
	return nb_cli_rollback_configuration(vty, transaction_id);
#else
	vty_out(vty,
		"%% FRR was compiled without --enable-config-rollbacks.\n\n");
	return CMD_SUCCESS;
#endif /* HAVE_CONFIG_ROLLBACKS */
}

void nb_transactional_cli_install_default(int node)
{
	install_element(node, &config_commit_cmd);
	install_element(node, &config_commit_comment_cmd);
	install_element(node, &config_commit_check_cmd);
	install_element(node, &config_discard_cmd);
	install_element(node, &show_config_running_cmd);
	install_element(node, &show_config_candidate_cmd);
	install_element(node, &show_config_compare_cmd);
	install_element(node, &show_config_transaction_cmd);
}

void nb_transactional_cli_init(void)
{
	install_element(CONFIG_NODE, &config_load_cmd);
	install_element(CONFIG_NODE, &config_database_max_transactions_cmd);

	install_element(ENABLE_NODE, &show_config_running_cmd);
	install_element(ENABLE_NODE, &show_config_compare_without_candidate_cmd);
	install_element(ENABLE_NODE, &show_config_transaction_cmd);
	install_element(ENABLE_NODE, &rollback_config_cmd);
}
