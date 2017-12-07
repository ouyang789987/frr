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
#include "northbound.h"

/* TODO: use thread-local storage. */
static char cfg_base_xpath[XPATH_MAXLEN];

void cfg_set_base_xpath(const char *xpath_fmt, ...)
{
	char xpath[XPATH_MAXLEN];
	va_list ap;

	va_start(ap, xpath_fmt);
	vsnprintf(xpath, sizeof(xpath), xpath_fmt, ap);
	va_end(ap);

	strlcpy(cfg_base_xpath, xpath, sizeof(cfg_base_xpath));
}

static void cfg_check_relative_xpath(char *xpath)
{
	char xpath_copy[XPATH_MAXLEN];

	if (xpath[0] != '.')
		return;

	strlcpy(xpath_copy, xpath, sizeof(xpath_copy));
	snprintf(xpath, XPATH_MAXLEN, "%s%s", cfg_base_xpath, xpath_copy + 1);
}

static struct lyd_node *cfg_get(const char *xpath_fmt, va_list ap)
{
	char xpath[XPATH_MAXLEN];

	vsnprintf(xpath, sizeof(xpath), xpath_fmt, ap);
	cfg_check_relative_xpath(xpath);

	return nb_config_get_running(xpath);
}

struct lyd_node *cfg_get_dnode(const char *xpath_fmt, ...)
{
	struct lyd_node *dnode;
	va_list ap;

	va_start(ap, xpath_fmt);
	dnode = cfg_get(xpath_fmt, ap);
	va_end(ap);

	return dnode;
}

/*
 * Check if a given configuration option exists.
 */
bool cfg_exists(const char *xpath_fmt, ...)
{
	char xpath[XPATH_MAXLEN];
	va_list ap;

	va_start(ap, xpath_fmt);
	vsnprintf(xpath, sizeof(xpath), xpath_fmt, ap);
	va_end(ap);

	cfg_check_relative_xpath(xpath);

	return (nb_config_get_running(xpath) != NULL);
}

/*
 * Check if a given configuration option is set to its default value. Only
 * applicable to YANG leafs.
 */
bool cfg_is_default(const char *xpath_fmt, ...)
{
	struct lyd_node *dnode;
	va_list ap;

	va_start(ap, xpath_fmt);
	dnode = cfg_get(xpath_fmt, ap);
	va_end(ap);

	if (dnode == NULL)
		return false;

	return yang_node_is_default(dnode);
}

void cfg_iterate(const char *xpath,
		 void (*func)(const struct lyd_node *, void *), void *arg)
{
	struct ly_set *set;

	set = lyd_find_path(running_config, xpath);
	assert(set);
	for (size_t i = 0; i < set->number; i++)
		(*func)(set->set.d[i], arg);

	ly_set_free(set);
}

/*
 * Abort execution when we failed to get the value of a configuration option
 * that was supposed to exist.
 */
static void __attribute__((noreturn))
cfg_get_failed(const char *xpath_fmt, va_list ap)
{
	char xpath[XPATH_MAXLEN];

	vsnprintf(xpath, sizeof(xpath), xpath_fmt, ap);
	cfg_check_relative_xpath(xpath);

	zlog_err("Failed to fetch configuration: %s", xpath);
	zlog_backtrace(LOG_ERR);
	abort();
}

/*
 * Primitive type: bool.
 */
static bool _cfg_get_optional_bool(bool *value, const char *xpath_fmt,
				   va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_bool(dnode);
	return true;
}

bool cfg_get_bool(const char *xpath_fmt, ...)
{
	bool value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_bool(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_bool(bool *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_bool(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

bool yang_str2bool(const char *value)
{
	return strmatch(value, "true");
}

struct yang_data *yang_data_new_bool(const char *xpath, bool value)
{
	return yang_data_new(xpath, (value == true) ? "true" : "false");
}

bool yang_dnode_get_bool(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.bln;
}

/*
 * Primitive type: dec64.
 */
static bool _cfg_get_optional_dec64(double *value, const char *xpath_fmt,
				    va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_dec64(dnode);
	return true;
}

double cfg_get_dec64(const char *xpath_fmt, ...)
{
	double value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_dec64(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_dec64(double *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_dec64(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

double yang_str2dec64(const char *value)
{
#if 0
	const struct lys_node *snode;

	snode = ly_ctx_get_node(ly_ctx, NULL, xpath, 0);
	if (snode == NULL) {
		zlog_warn("%s: couldn't find schema information for '%s'",
			  __func__, xpath);
		return -1;
	}
#endif

	return strtoll(value, NULL, 10);
}

struct yang_data *yang_data_new_dec64(const char *xpath, int64_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%" PRId64, value);
	return yang_data_new(xpath, value_str);
}

double yang_dnode_get_dec64(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;
	struct lys_node_leaf *sleaf;
	double value;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	value = dleaf->value.dec64 * 1.0;

	/* shift decimal points */
	sleaf = (struct lys_node_leaf *)dleaf->schema;
	for (size_t i = 0; i < sleaf->type.info.dec64.dig; i++)
		value *= 0.1;

	return value;
}

/*
 * Primitive type: enum.
 */
static bool _cfg_get_optional_enum(int *value, const char *xpath_fmt,
				   va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_enum(dnode);
	return true;
}

int cfg_get_enum(const char *xpath_fmt, ...)
{
	int value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_enum(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_enum(int *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_enum(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

int yang_dnode_get_enum(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.enm->value;
}

/*
 * Primitive type: int8.
 */
static bool _cfg_get_optional_int8(int8_t *value, const char *xpath_fmt,
				   va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_int8(dnode);
	return true;
}

int8_t cfg_get_int8(const char *xpath_fmt, ...)
{
	int8_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_int8(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_int8(int8_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_int8(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

int8_t yang_str2int8(const char *value)
{
	return strtol(value, NULL, 10);
}

struct yang_data *yang_data_new_int8(const char *xpath, int8_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%d", value);
	return yang_data_new(xpath, value_str);
}

int8_t yang_dnode_get_int8(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.int8;
}

/*
 * Primitive type: int16.
 */
static bool _cfg_get_optional_int16(int16_t *value, const char *xpath_fmt,
				    va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_int16(dnode);
	return true;
}

int16_t cfg_get_int16(const char *xpath_fmt, ...)
{
	int16_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_int16(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_int16(int16_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_int16(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

int16_t yang_str2int16(const char *value)
{
	return strtol(value, NULL, 10);
}

struct yang_data *yang_data_new_int16(const char *xpath, int16_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%d", value);
	return yang_data_new(xpath, value_str);
}

int16_t yang_dnode_get_int16(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.int16;
}

/*
 * Primitive type: int32.
 */
static bool _cfg_get_optional_int32(int32_t *value, const char *xpath_fmt,
				    va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_int32(dnode);
	return true;
}

int32_t cfg_get_int32(const char *xpath_fmt, ...)
{
	int32_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_int32(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_int32(int32_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_int32(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

int32_t yang_str2int32(const char *value)
{
	return strtol(value, NULL, 10);
}

struct yang_data *yang_data_new_int32(const char *xpath, int32_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%d", value);
	return yang_data_new(xpath, value_str);
}

int32_t yang_dnode_get_int32(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.int32;
}

/*
 * Primitive type: int64.
 */
static bool _cfg_get_optional_int64(int64_t *value, const char *xpath_fmt,
				    va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_int64(dnode);
	return true;
}

int64_t cfg_get_int64(const char *xpath_fmt, ...)
{
	int64_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_int64(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_int64(int64_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_int64(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

int64_t yang_str2int64(const char *value)
{
	return strtoll(value, NULL, 10);
}

struct yang_data *yang_data_new_int64(const char *xpath, int64_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%" PRId64, value);
	return yang_data_new(xpath, value_str);
}

int64_t yang_dnode_get_int64(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.int64;
}

/*
 * Primitive type: uint8.
 */
static bool _cfg_get_optional_uint8(uint8_t *value, const char *xpath_fmt,
				    va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_uint8(dnode);
	return true;
}

uint8_t cfg_get_uint8(const char *xpath_fmt, ...)
{
	uint8_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_uint8(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_uint8(uint8_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_uint8(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

uint8_t yang_str2uint8(const char *value)
{
	return strtoul(value, NULL, 10);
}

struct yang_data *yang_data_new_uint8(const char *xpath, uint8_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%u", value);
	return yang_data_new(xpath, value_str);
}

uint8_t yang_dnode_get_uint8(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.uint8;
}

/*
 * Primitive type: uint16.
 */
static bool _cfg_get_optional_uint16(uint16_t *value, const char *xpath_fmt,
				     va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_uint16(dnode);
	return true;
}

uint16_t cfg_get_uint16(const char *xpath_fmt, ...)
{
	uint16_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_uint16(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_uint16(uint16_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_uint16(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

uint16_t yang_str2uint16(const char *value)
{
	return strtoul(value, NULL, 10);
}

struct yang_data *yang_data_new_uint16(const char *xpath, uint16_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%u", value);
	return yang_data_new(xpath, value_str);
}

uint16_t yang_dnode_get_uint16(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.uint16;
}

/*
 * Primitive type: uint32.
 */
static bool _cfg_get_optional_uint32(uint32_t *value, const char *xpath_fmt,
				     va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_uint32(dnode);
	return true;
}

uint32_t cfg_get_uint32(const char *xpath_fmt, ...)
{
	uint32_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_uint32(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_uint32(uint32_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_uint32(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

uint32_t yang_str2uint32(const char *value)
{
	return strtoul(value, NULL, 10);
}

struct yang_data *yang_data_new_uint32(const char *xpath, uint32_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%u", value);
	return yang_data_new(xpath, value_str);
}

uint32_t yang_dnode_get_uint32(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.uint32;
}

/*
 * Primitive type: uint64.
 */
static bool _cfg_get_optional_uint64(uint64_t *value, const char *xpath_fmt,
				     va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_uint64(dnode);
	return true;
}

uint64_t cfg_get_uint64(const char *xpath_fmt, ...)
{
	uint64_t value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_uint64(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_uint64(uint64_t *value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_uint64(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

uint64_t yang_str2uint64(const char *value)
{
	return strtoull(value, NULL, 10);
}

struct yang_data *yang_data_new_uint64(const char *xpath, uint64_t value)
{
	char value_str[BUFSIZ];

	snprintf(value_str, sizeof(value_str), "%" PRIu64, value);
	return yang_data_new(xpath, value_str);
}

uint64_t yang_dnode_get_uint64(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value.uint64;
}

/*
 * Primitive type: string.
 *
 * All string wrappers can be used with non-string types.
 */
static bool _cfg_get_optional_string(const char **value, const char *xpath_fmt,
				     va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	*value = yang_dnode_get_string(dnode);

	return true;
}

const char *cfg_get_string(const char *xpath_fmt, ...)
{
	const char *value;
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_string(&value, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return value;
}

bool cfg_get_optional_string(const char **value, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_string(value, xpath_fmt, ap);
	va_end(ap);

	return found;
}

static bool _cfg_get_optional_string_buf(char *buffer, size_t size,
					 const char *xpath_fmt, va_list ap)
{
	struct lyd_node *dnode;
	const char *value;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	value = yang_dnode_get_string(dnode);
	if (strlcpy(buffer, value, size) >= size) {
		char xpath[XPATH_MAXLEN];

		vsnprintf(xpath, sizeof(xpath), xpath_fmt, ap);
		zlog_warn("%s: value was truncated [xpath %s]", __func__,
			  xpath);
	}

	return true;
}

char *cfg_get_string_buf(char *buffer, size_t size, const char *xpath_fmt, ...)
{
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_string_buf(buffer, size, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);

	return buffer;
}

bool cfg_get_optional_string_buf(char *buffer, size_t size,
				 const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_string_buf(buffer, size, xpath_fmt, ap);
	va_end(ap);

	return found;
}

struct yang_data *yang_data_new_string(const char *xpath, const char *value)
{
	return yang_data_new(xpath, value);
}

const char *yang_dnode_get_string(const struct lyd_node *dnode)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	return dleaf->value_str;
}

/*
 * Derived type: ipv4.
 */
static bool _cfg_get_optional_ipv4(struct in_addr *addr, const char *xpath_fmt,
				   va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	yang_dnode_get_ipv4(dnode, addr);

	return true;
}

void cfg_get_ipv4(struct in_addr *addr, const char *xpath_fmt, ...)
{
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_ipv4(addr, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);
}

bool cfg_get_optional_ipv4(struct in_addr *addr, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_ipv4(addr, xpath_fmt, ap);
	va_end(ap);

	return found;
}

void yang_str2ipv4(const char *value, struct in_addr *addr)
{
	(void)inet_pton(AF_INET, value, addr);
}

struct yang_data *yang_data_new_ipv4(const char *xpath, struct in_addr *addr)
{
	char value_str[INET_ADDRSTRLEN];

	(void)inet_ntop(AF_INET, addr, value_str, sizeof(value_str));
	return yang_data_new(xpath, value_str);
}

void yang_dnode_get_ipv4(const struct lyd_node *dnode, struct in_addr *addr)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	memcpy(addr, dleaf->value.ptr, sizeof(*addr));
}

/*
 * Derived type: ipv4p.
 */
static bool _cfg_get_optional_ipv4p(union prefixptr prefix,
				    const char *xpath_fmt, va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	yang_dnode_get_ipv4p(dnode, prefix);

	return true;
}

void cfg_get_ipv4p(union prefixptr prefix, const char *xpath_fmt, ...)
{
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_ipv4p(prefix, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);
}

bool cfg_get_optional_ipv4p(union prefixptr prefix, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_ipv4p(prefix, xpath_fmt, ap);
	va_end(ap);

	return found;
}

void yang_str2ipv4p(const char *value, union prefixptr prefix)
{
	struct prefix_ipv4 *prefix4 = prefix.p4;

	(void)str2prefix_ipv4(value, prefix4);
	apply_mask_ipv4(prefix4);
}

struct yang_data *yang_data_new_ipv4p(const char *xpath, union prefixptr prefix)
{
	char value_str[PREFIX2STR_BUFFER];

	(void)prefix2str(prefix.p, value_str, sizeof(value_str));
	return yang_data_new(xpath, value_str);
}

void yang_dnode_get_ipv4p(const struct lyd_node *dnode, union prefixptr prefix)
{
	const struct lyd_node_leaf_list *dleaf;
	struct prefix_ipv4 *prefix4 = prefix.p4;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	memcpy(prefix4, dleaf->value.ptr, sizeof(*prefix4));
}

/*
 * Derived type: ipv6.
 */
static bool _cfg_get_optional_ipv6(struct in6_addr *addr, const char *xpath_fmt,
				   va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	yang_dnode_get_ipv6(dnode, addr);

	return true;
}

void cfg_get_ipv6(struct in6_addr *addr, const char *xpath_fmt, ...)
{
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_ipv6(addr, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);
}

bool cfg_get_optional_ipv6(struct in6_addr *addr, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_ipv6(addr, xpath_fmt, ap);
	va_end(ap);

	return found;
}

void yang_str2ipv6(const char *value, struct in6_addr *addr)
{
	(void)inet_pton(AF_INET6, value, addr);
}

struct yang_data *yang_data_new_ipv6(const char *xpath, struct in6_addr *addr)
{
	char value_str[INET6_ADDRSTRLEN];

	(void)inet_ntop(AF_INET6, addr, value_str, sizeof(value_str));
	return yang_data_new(xpath, value_str);
}

void yang_dnode_get_ipv6(const struct lyd_node *dnode, struct in6_addr *addr)
{
	const struct lyd_node_leaf_list *dleaf;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	memcpy(addr, dleaf->value.ptr, sizeof(*addr));
}

/*
 * Derived type: ipv6p.
 */
static bool _cfg_get_optional_ipv6p(union prefixptr prefix,
				    const char *xpath_fmt, va_list ap)
{
	struct lyd_node *dnode;

	dnode = cfg_get(xpath_fmt, ap);
	if (!dnode)
		return false;

	yang_dnode_get_ipv6p(dnode, prefix);

	return true;
}

void cfg_get_ipv6p(union prefixptr prefix, const char *xpath_fmt, ...)
{
	va_list ap;

	va_start(ap, xpath_fmt);
	if (!_cfg_get_optional_ipv6p(prefix, xpath_fmt, ap))
		cfg_get_failed(xpath_fmt, ap);
	va_end(ap);
}

bool cfg_get_optional_ipv6p(union prefixptr prefix, const char *xpath_fmt, ...)
{
	bool found;
	va_list ap;

	va_start(ap, xpath_fmt);
	found = _cfg_get_optional_ipv6p(prefix, xpath_fmt, ap);
	va_end(ap);

	return found;
}

void yang_str2ipv6p(const char *value, union prefixptr prefix)
{
	struct prefix_ipv6 *prefix6 = prefix.p6;

	(void)str2prefix_ipv6(value, prefix6);
	apply_mask_ipv6(prefix6);
}

struct yang_data *yang_data_new_ipv6p(const char *xpath, union prefixptr prefix)
{
	char value_str[PREFIX2STR_BUFFER];

	(void)prefix2str(prefix.p, value_str, sizeof(value_str));
	return yang_data_new(xpath, value_str);
}

void yang_dnode_get_ipv6p(const struct lyd_node *dnode, union prefixptr prefix)
{
	const struct lyd_node_leaf_list *dleaf;
	struct prefix_ipv6 *prefix6 = prefix.p6;

	dleaf = (const struct lyd_node_leaf_list *)dnode;
	memcpy(prefix6, dleaf->value.ptr, sizeof(*prefix6));
}
