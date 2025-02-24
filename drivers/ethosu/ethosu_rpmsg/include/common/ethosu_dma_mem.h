/*
 * SPDX-FileCopyrightText: Copyright 2023-2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 */

#ifndef ETHOSU_DMA_MEM_H
#define ETHOSU_DMA_MEM_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <linux/types.h>

/****************************************************************************
 * Types
 ****************************************************************************/

struct device;

/**
 * struct ethosu_dma_mem - DMA memory allocation
 * @dev:	Device
 * @size:	Size of the allocation
 * @cpu_addr:	Kernel mapped address
 * @dma_addr:	DMA address
 */
struct ethosu_dma_mem {
	struct device *dev;
	size_t        size;
	void          *cpu_addr;
	dma_addr_t    dma_addr;
};

/****************************************************************************
 * Functions
 ****************************************************************************/

struct ethosu_dma_mem *ethosu_dma_mem_alloc(struct device *dev,
					    size_t size);

void ethosu_dma_mem_free(struct ethosu_dma_mem **dma_mem);

#endif /* ETHOSU_DMA_MEM_H */
