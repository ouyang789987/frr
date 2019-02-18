/*
 * Copyright (C) 2019  NetDEF, Inc.
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

#include "command.h"
#include "linklist.h"
#include "memory.h"
#include "isis_memory.h"
#include "prefix.h"
#include "vty.h"

#include "isisd/isisd.h"
#include "isisd/isis_memory.h"
#include "isisd/isis_ppr.h"
#include "isisd/isis_tlvs.h"

const char *isis_pprflags2str(uint8_t flags)
{
	static char buf[BUFSIZ];

	if (flags == 0)
		return "-";

	snprintf(buf, sizeof(buf), "%s%s%s%s",
		 CHECK_FLAG(flags, ISIS_PPR_FLAG_D) ? "D" : "",
		 CHECK_FLAG(flags, ISIS_PPR_FLAG_S) ? "S" : "",
		 CHECK_FLAG(flags, ISIS_PPR_FLAG_A) ? "A" : "",
		 CHECK_FLAG(flags, ISIS_PPR_FLAG_LAST) ? "L" : "");

	return buf;
}

const char *isis_ppridtype2str(enum ppr_id_type type)
{
	static char buf[BUFSIZ];

	switch (type) {
	case PPR_ID_TYPE_MPLS:
		return "MPLS";
	case PPR_ID_TYPE_IPV4:
		return "Native IPv4";
	case PPR_ID_TYPE_IPV6:
		return "Native IPv6";
	case PPR_ID_TYPE_SRV6:
		return "SRv6";
	default:
		snprintf(buf, sizeof(buf), "Unknown (%" PRIu8 ")", type);
		return buf;
	}
}

const char *isis_pprid2str(const struct ppr_id *i)
{
	static char buf[BUFSIZ];

	switch (i->type) {
	case PPR_ID_TYPE_MPLS:
		snprintf(buf, sizeof(buf), "%u", i->id.mpls);
		break;
	case PPR_ID_TYPE_IPV4:
	case PPR_ID_TYPE_IPV6:
	case PPR_ID_TYPE_SRV6:
		prefix2str(&i->id.prefix, buf, sizeof(buf));
		break;
	default:
		snprintf(buf, sizeof(buf), "Unknown");
		break;
	}

	return buf;
}

const char *isis_ppridalgo2str(uint8_t algorithm)
{
	static char buf[BUFSIZ];

	switch (algorithm) {
	case ISIS_ALGORITHM_SPF:
		return "SPF";
	case ISIS_ALGORITHM_STRICT_SPF:
		return "Strict SPF";
	default:
		snprintf(buf, sizeof(buf), "Unknown (%" PRIu8 ")", algorithm);
		return buf;
	}
}

const char *isis_ppridflags2str(uint16_t flags)
{
	static char buf[BUFSIZ];

	if (flags == 0)
		return "-";

	snprintf(buf, sizeof(buf), "%s%s",
		 CHECK_FLAG(flags, ISIS_PPR_ID_FLAG_LOOSE) ? "L" : "",
		 CHECK_FLAG(flags, ISIS_PPR_ID_FLAG_ALL) ? "A" : "");

	return buf;
}

const char *isis_pprpdetype2str(enum ppr_pde_type type)
{
	static char buf[BUFSIZ];

	switch (type) {
	case PPR_PDE_TYPE_TOPOLOGICAL:
		return "Topological";
	case PPR_PDE_TYPE_NON_TOPOLOGICAL:
		return "Non-Topological";
	default:
		snprintf(buf, sizeof(buf), "Unknown (%" PRIu8 ")", type);
		return buf;
	}
}

const char *isis_pprpdeidtype2str(enum ppr_pde_id_type type)
{
	static char buf[BUFSIZ];

	switch (type) {
	case PPR_PDE_ID_TYPE_SID_LABEL:
		return "SID/Label";
	case PPR_PDE_ID_TYPE_SRMPLS_PREFIX_SID:
		return "SR-MPLS Prefix SID";
	case PPR_PDE_ID_TYPE_SRMPLS_ADJ_SID:
		return "SR-MPLS Adjacency SID";
	case PPR_PDE_ID_TYPE_IPV4:
		return "IPv4 Address";
	case PPR_PDE_ID_TYPE_IPV6:
		return "IPv6 Address";
	case PPR_PDE_ID_TYPE_SRV6_NODE_SID:
		return "SRv6 Node SID";
	case PPR_PDE_ID_TYPE_SRV6_ADJ_SID:
		return "SRv6 Adjacency-SID";
	default:
		snprintf(buf, sizeof(buf), "Unknown (%" PRIu8 ")", type);
		return buf;
	}
}

const char *isis_pprpdeid2str(const struct ppr_pde *pde)
{
	static char buf[BUFSIZ];

	switch (pde->id_type) {
	case PPR_PDE_ID_TYPE_SID_LABEL:
	case PPR_PDE_ID_TYPE_SRMPLS_PREFIX_SID:
	case PPR_PDE_ID_TYPE_SRMPLS_ADJ_SID:
		snprintf(buf, sizeof(buf), "%u", pde->id_value.mpls);
		break;
	case PPR_PDE_ID_TYPE_IPV4:
		inet_ntop(AF_INET, &pde->id_value.ipv4, buf, sizeof(buf));
		break;
	case PPR_PDE_ID_TYPE_IPV6:
	case PPR_PDE_ID_TYPE_SRV6_NODE_SID:
	case PPR_PDE_ID_TYPE_SRV6_ADJ_SID:
		inet_ntop(AF_INET6, &pde->id_value.ipv6, buf, sizeof(buf));
		break;
	default:
		snprintf(buf, sizeof(buf), "Unknown");
		break;
	}

	return buf;
}

const char *isis_pprpdeflags2str(uint16_t flags)
{
	static char buf[BUFSIZ];

	if (flags == 0)
		return "-";

	snprintf(buf, sizeof(buf), "%s%s",
		 CHECK_FLAG(flags, ISIS_PPR_PDE_FLAG_LOOSE) ? "L" : "",
		 CHECK_FLAG(flags, ISIS_PPR_PDE_FLAG_DEST) ? "D" : "");

	return buf;
}

void isis_ppr_init(void)
{
}
