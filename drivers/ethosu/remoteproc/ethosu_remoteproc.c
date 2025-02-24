/*
 * SPDX-FileCopyrightText: Copyright 2021-2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
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

#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/version.h>
#include <linux/workqueue.h>

/****************************************************************************
 * Defines
 ****************************************************************************/

#define DMA_ADDR_BITS 32 /* Number of address bits */

#define ETHOSU_RPROC_DRIVER_VERSION "0.0.1"

#define DEFAULT_FW_FILE "arm-ethos-u65.fw"
#define DEFAULT_AUTO_BOOT false

/* firmware naming module parameter */
static char fw_filename_param[256] = DEFAULT_FW_FILE;

/* As the remoteproc is setup at probe, just allow the filename readonly */
module_param_string(filename, fw_filename_param, sizeof(fw_filename_param),
		    0444);
MODULE_PARM_DESC(filename,
		 "Filename for firmware image for Ethos-U remoteproc");

static bool auto_boot = DEFAULT_AUTO_BOOT;
module_param(auto_boot, bool, 0);
MODULE_PARM_DESC(auto_boot, "Set to one to auto boot at load.");

#define RSC_MAPPING RSC_VENDOR_START + 1

/**
 * struct fw_rsc_map_range - memory map range
 * @da:		Start device address of the memory address range
 * @pa:		Start physical address of the memory address range
 * @len:	length of memory address range
 *
 * Memory range to translate between physical and device addresses.
 */
struct fw_rsc_map_range {
	uint32_t da;
	uint32_t pa;
	uint32_t len;
} __packed;

/**
 * struct fw_rsc_mapping - memory map for address translation
 * @num_ranges:	Number of ranges in the memory map
 * @range:	Array of the ranges in the memory map
 *
 * This resource entry requests the host to provide information for how to
 * translate between physical and device addresses.
 */
struct fw_rsc_mapping {
	uint8_t                 num_ranges;
	struct fw_rsc_map_range range[0];
} __packed;

struct ethosu_rproc {
	struct device           *dev;
	struct reset_control    *rstc;
	struct mbox_client      mbox_client;
	struct mbox_chan        *ch_rx;
	struct mbox_chan        *ch_tx;
	struct workqueue_struct *wq;
	struct work_struct      work;
};

/* declaration is in remoteproc_internal.h */
extern irqreturn_t rproc_vq_interrupt(struct rproc *rproc,
				      int vq_id);

static void ethosu_mbox_bottom(struct work_struct *work)
{
	struct ethosu_rproc *erproc = container_of(
		work, struct ethosu_rproc, work);
	struct rproc *rproc = dev_get_drvdata(erproc->dev);

	dev_dbg(&rproc->dev, "Handle interrupt");

	rproc_vq_interrupt(rproc, 0);
}

static void ethosu_mbox_top(struct mbox_client *client,
			    void *message)
{
	struct ethosu_rproc *erproc = container_of(
		client, struct ethosu_rproc, mbox_client);

	queue_work(erproc->wq, &erproc->work);
}

static int ethosu_mem_alloc(struct rproc *rproc,
			    struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	if (mem->is_iomem)
		va = (__force void *)devm_ioremap(dev, mem->dma, mem->len);
	else
		va = devm_memremap(dev, mem->dma, mem->len, MEMREMAP_WC);

	if (IS_ERR_OR_NULL(va)) {
		dev_err(dev, "Failed to remap address. pa=%pa, len=%zu",
			&mem->dma,
			mem->len);

		return -ENOMEM;
	}

	mem->va = va;

	return 0;
}

static int ethosu_mem_release(struct rproc *rproc,
			      struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;

	if (!mem->va) {
		dev_warn(dev,
			 "Memory release. No mapping for memory %s pa=%p len=%zu",
			 mem->name, &mem->dma, mem->len);
		goto done;
	}

	if (mem->is_iomem)
		devm_iounmap(dev, (__force __iomem void *)mem->va);
	else
		devm_memunmap(dev, mem->va);

done:

	return 0;
}

static int ethosu_add_carveout(struct rproc *rproc,
			       const phys_addr_t pa,
			       const size_t size,
			       const char *name,
			       bool is_iomem)
{
	struct device *dev = rproc->dev.parent;
	dma_addr_t da;
	struct rproc_mem_entry *mem;

	da = translate_phys_to_dma(dev, pa);
	dev_dbg(dev, "PA to DA. pa=0x%pa, da=0x%pad", &pa, &da);
	if (da == DMA_MAPPING_ERROR) {
		dev_err(dev, "No mapping found for PA. pa=%pa, size=%zu", &pa,
			size);

		return -ENOMEM;
	}

	mem = rproc_mem_entry_init(dev, NULL, pa, size, da, ethosu_mem_alloc,
				   ethosu_mem_release, name);
	if (!mem)
		return -ENOMEM;

	mem->is_iomem = is_iomem;

	dev_dbg(dev, "Add carveout mapping. dma=%pad, da=%x, va=%p, len=%zu",
		&mem->dma, mem->da, mem->va, mem->len);

	rproc_add_carveout(rproc, mem);

	return 0;
}

static int ethosu_rproc_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct resource res;
	int i;
	int ret;

	/* Add carveout for each 'reg' device tree entry */
	for (i = 0; of_address_to_resource(np, i, &res) == 0; i++) {
		dev_dbg(dev, "Found resource. start=%llx, size=%llx",
			res.start, resource_size(&res));

		ret = ethosu_add_carveout(rproc, res.start,
					  resource_size(&res), res.name,
					  !strncmp(res.name, "rom", 3));
		if (ret)
			return ret;
	}

	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		struct reserved_mem *res_mem = of_reserved_mem_lookup(it.node);

		if (!res_mem) {
			dev_err(dev, "Failed to look up memory region. node=%p",
				it.node);

			return -EINVAL;
		}

		dev_dbg(dev,
			"Found memory region. pa=%llx, size=%llu, name=%s",
			res_mem->base, res_mem->size, it.node->name);

		ret = ethosu_add_carveout(rproc, res_mem->base, res_mem->size,
					  it.node->name, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int ethosu_rproc_start(struct rproc *rproc)
{
	struct ethosu_rproc *erproc = (struct ethosu_rproc *)rproc->priv;
	struct device *dev = erproc->dev;

	dev_info(dev, "Starting up Ethos-U subsystem CPU");

	return reset_control_deassert(erproc->rstc);
}

static int ethosu_rproc_stop(struct rproc *rproc)
{
	struct ethosu_rproc *erproc = (struct ethosu_rproc *)rproc->priv;
	struct device *dev = erproc->dev;

	dev_info(dev, "Stopping Ethos-U subsystem CPU");

	return reset_control_assert(erproc->rstc);
}

static void ethosu_rproc_kick(struct rproc *rproc,
			      int vqid)
{
	struct ethosu_rproc *erproc = (struct ethosu_rproc *)rproc->priv;

	dev_dbg(&rproc->dev, "Kicking Ethos-U remoteproc vqid: %d!", vqid);

	mbox_send_message(erproc->ch_tx, (void *)&vqid);
}

static int ethosu_rproc_handle_rsc(struct rproc *rproc,
				   u32 rsc_type,
				   void *rsc,
				   int offset,
				   int avail)
{
	struct ethosu_rproc *erproc = (struct ethosu_rproc *)rproc->priv;
	struct device *dev = erproc->dev;
	struct fw_rsc_mapping *mapping = rsc;
	const struct bus_dma_region *map;
	size_t num_ranges = 0U;
	size_t i;

	if (rsc_type != RSC_MAPPING)
		return RSC_IGNORED;

	if (struct_size(mapping, range, mapping->num_ranges) > avail) {
		dev_err(dev, "mapping rsc is truncated\n");

		return -EINVAL;
	}

	for (map = dev->dma_range_map; map->size; ++map)
		num_ranges++;

	if (num_ranges > mapping->num_ranges) {
		dev_err(dev,
			"Mapping rsc doesn't have enough room for DMA ranges\n");

		return -EINVAL;
	}

	for (i = 0U; i < num_ranges; ++i) {
		struct fw_rsc_map_range *range = &mapping->range[i];
		map = &dev->dma_range_map[i];

		range->da = map->dma_start;
		range->pa = map->cpu_start;
		range->len = map->size;
	}

	dev_dbg(dev, "handle_rsc: Mapping rsc setup");

	return RSC_HANDLED;
}

static const struct rproc_ops ethosu_rproc_ops = {
	.prepare    = &ethosu_rproc_prepare,
	.start      = &ethosu_rproc_start,
	.stop       = &ethosu_rproc_stop,
	.kick       = &ethosu_rproc_kick,
	.handle_rsc = &ethosu_rproc_handle_rsc,
};

static int ethosu_mailbox_init(struct ethosu_rproc *erproc)
{
	struct device *dev = erproc->dev;
	struct mbox_client *cl = &erproc->mbox_client;

	INIT_WORK(&erproc->work, ethosu_mbox_bottom);

	erproc->wq = create_singlethread_workqueue("ethosu_rproc_wq");
	if (!erproc->wq) {
		dev_err(dev, "Failed to create work queue");

		return -EINVAL;
	}

	cl->dev = dev;
	cl->rx_callback = ethosu_mbox_top;
	cl->tx_prepare = NULL;
	cl->tx_done = NULL;
	cl->tx_block = true;
	cl->knows_txdone = false;
	cl->tx_tout = 500;

	erproc->ch_rx = mbox_request_channel_byname(cl, "rx");
	if (IS_ERR(erproc->ch_rx)) {
		dev_err(dev, "Failed to request mbox chan rx");

		return PTR_ERR(erproc->ch_rx);
	}

	erproc->ch_tx = mbox_request_channel_byname(cl, "tx");
	if (IS_ERR(erproc->ch_tx)) {
		dev_dbg(dev, "Using same channel for RX and TX");
		erproc->ch_tx = erproc->ch_rx;
	}

	return 0;
}

static const struct of_device_id ethosu_rproc_match[] = {
	{ .compatible = "arm,ethosu-rproc" },
	{ /* sentinel */ },
};

static int ethosu_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct ethosu_rproc *erproc;
	struct rproc *rproc;
	int ret;

	/* map the first 'memory-region' for DMA-mapping */
	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return ret;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(DMA_ADDR_BITS));

	rproc = devm_rproc_alloc(dev, np->name, &ethosu_rproc_ops,
				 fw_filename_param,
				 sizeof(*erproc));
	if (!rproc)
		return -ENOMEM;

	platform_set_drvdata(pdev, rproc);

	/* Configure rproc */
	rproc->has_iommu = false;
	rproc->auto_boot = auto_boot;

	/* Configure Ethos-U rproc */
	erproc = rproc->priv;
	erproc->dev = dev;

	/* Get the reset handler for the subsystem */
	erproc->rstc = devm_reset_control_get_exclusive_by_index(dev, 0);
	if (IS_ERR(erproc->rstc)) {
		dev_err(&pdev->dev, "Failed to get reset controller.");

		return PTR_ERR(erproc->rstc);
	}

	/* Allocate and initialize mailbox client */
	ret = ethosu_mailbox_init(erproc);
	if (ret)
		return ret;

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "Failed to add rproc");
		goto free_mbox;
	}

	return 0;

free_mbox:
	if (erproc->wq)
		destroy_workqueue(erproc->wq);

	mbox_free_channel(erproc->ch_rx);

	if (erproc->ch_tx != erproc->ch_rx)
		mbox_free_channel(erproc->ch_tx);

	return ret;
}

static void ethosu_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct ethosu_rproc *erproc = rproc->priv;

	if (erproc->wq)
		destroy_workqueue(erproc->wq);

	if (erproc->ch_tx != erproc->ch_rx)
		mbox_free_channel(erproc->ch_tx);

	mbox_free_channel(erproc->ch_rx);

	rproc_del(rproc);

	return;
}

static struct platform_driver ethosu_rproc_driver = {
	.probe                  = ethosu_rproc_probe,
	.remove                 = ethosu_rproc_remove,
	.driver                 = {
		.name           = "ethosu-rproc",
		.of_match_table = of_match_ptr(ethosu_rproc_match),
	},
};

module_platform_driver(ethosu_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Arm Ltd");
MODULE_DESCRIPTION("Arm Ethos-U NPU RemoteProc Driver");
MODULE_VERSION(ETHOSU_RPROC_DRIVER_VERSION);
