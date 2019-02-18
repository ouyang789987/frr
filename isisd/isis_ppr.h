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

#ifndef ISIS_PPR_H
#define ISIS_PPR_H

#include <zebra.h>

#include "openbsd-tree.h"

#include "isisd/isisd.h"
#include "isisd/isis_tlvs.h"

#define ISIS_PPR_FLAGS_MASK		0xf0
#define ISIS_PPR_ID_FLAGS_MASK		0xc000
#define ISIS_PPR_PDE_FLAGS_MASK		0xc000

#define ISIS_ALGORITHM_SPF		1
#define ISIS_ALGORITHM_STRICT_SPF	2

extern const char *isis_pprflags2str(uint8_t flags);
extern const char *isis_ppridtype2str(enum ppr_id_type type);
extern const char *isis_pprid2str(const struct ppr_id *i);
extern const char *isis_ppridalgo2str(uint8_t algorithm);
extern const char *isis_ppridflags2str(uint16_t flags);
extern const char *isis_pprpdetype2str(enum ppr_pde_type type);
extern const char *isis_pprpdeidtype2str(enum ppr_pde_id_type type);
extern const char *isis_pprpdeid2str(const struct ppr_pde *p);
extern const char *isis_pprpdeflags2str(uint16_t flags);
extern void isis_ppr_init(void);

extern struct list *ppr_tlvs;

#endif /* ISIS_PPR_H */
