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

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <common/ethosu_dma_mem.h>

#include <linux/err.h>
#include <linux/dma-mapping.h>

/****************************************************************************
 * Functions
 ****************************************************************************/

struct ethosu_dma_mem *ethosu_dma_mem_alloc(struct device *dev,
					    size_t size)
{
	struct ethosu_dma_mem *dma_mem;

	if (!size) {
		dev_err(dev, "DMA mem alloc. Invalid zero size");

		return ERR_PTR(-EINVAL);
	}

	dma_mem = devm_kzalloc(dev, sizeof(*dma_mem), GFP_KERNEL);
	if (!dma_mem) {
		dev_err(dev,
			"DMA mem alloc. Failed to allocate struct");

		return ERR_PTR(-ENOMEM);
	}

	dma_mem->dev = dev;
	dma_mem->size = size;
	dma_mem->cpu_addr = dma_alloc_coherent(dev, size, &dma_mem->dma_addr,
					       GFP_KERNEL);
	if (!dma_mem->cpu_addr) {
		dev_err(dev, "DMA mem alloc. Failed to allocate 0x%02zx bytes",
			size);
		memset(dma_mem, 0, sizeof(*dma_mem));
		devm_kfree(dev, dma_mem);

		return ERR_PTR(-ENOMEM);
	}

	return dma_mem;
}

void ethosu_dma_mem_free(struct ethosu_dma_mem **dma_mem)
{
	struct device *dev;
	struct ethosu_dma_mem *mem;

	if (!dma_mem || !*dma_mem)
		return;

	mem = *dma_mem;
	dev = mem->dev;

	memset(mem->cpu_addr, 0, mem->size);
	dma_free_coherent(dev, mem->size, mem->cpu_addr, mem->dma_addr);

	memset(mem, 0, sizeof(*mem));
	devm_kfree(dev, mem);

	*dma_mem = NULL;
}
