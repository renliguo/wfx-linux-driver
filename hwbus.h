/*
 * Common hwbus abstraction layer interface for WFX wireless driver
 *
 * Copyright (c) 2017, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef WFX_HWBUS_H
#define WFX_HWBUS_H

#define WFX_REG_CONFIG        0x0
#define WFX_REG_CONTROL       0x1
#define WFX_REG_IN_OUT_QUEUE  0x2
#define WFX_REG_AHB_DPORT     0x3
#define WFX_REG_BASE_ADDR     0x4
#define WFX_REG_SRAM_DPORT    0x5
#define WFX_REG_SET_GEN_R_W   0x6
#define WFX_REG_FRAME_OUT     0x7

struct hwbus_ops {
	int (*copy_from_io)(void *bus_priv, unsigned int addr, void *dst, size_t count);
	int (*copy_to_io)(void *bus_priv, unsigned int addr, const void *src, size_t count);
	void (*lock)(void *bus_priv);
	void (*unlock)(void *bus_priv);
	size_t (*align_size)(void *bus_priv, size_t size);
};

#endif /* WFX_HWBUS_H */
