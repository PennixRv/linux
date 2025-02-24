/*
 * SPDX-FileCopyrightText: Copyright 2020-2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
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

#include <common/ethosu_buffer.h>

#include <common/ethosu_device.h>
#include <common/ethosu_dma_mem.h>
#include <uapi/ethosu.h>

#include <linux/anon_inodes.h>
#include <linux/dma-mapping.h>
#include <linux/of_address.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/remoteproc.h>
#include <linux/uaccess.h>

/****************************************************************************
 * Variables
 ****************************************************************************/

static int ethosu_buffer_release(struct inode *inode,
				 struct file *file);

static int ethosu_buffer_mmap(struct file *file,
			      struct vm_area_struct *vma);

static loff_t ethosu_buffer_llseek(struct file *file,
				   loff_t offset,
				   int whence);

static const struct file_operations ethosu_buffer_fops = {
	.release = &ethosu_buffer_release,
	.mmap    = &ethosu_buffer_mmap,
	.llseek  = &ethosu_buffer_llseek,
};

/****************************************************************************
 * Functions
 ****************************************************************************/

static bool ethosu_buffer_verify(struct file *file)
{
	return file->f_op == &ethosu_buffer_fops;
}

static void ethosu_buffer_destroy(struct kref *kref)
{
	struct ethosu_buffer *buf =
		container_of(kref, struct ethosu_buffer, kref);
	struct device *dev = buf->dev;

	dev_dbg(dev, "Buffer destroy. buf=0x%pK", buf);

	ethosu_dma_mem_free(&buf->dma_mem);

	memset(buf, 0, sizeof(*buf));
	devm_kfree(dev, buf);
}

static int ethosu_buffer_release(struct inode *inode,
				 struct file *file)
{
	struct ethosu_buffer *buf = file->private_data;
	struct device *dev = buf->dev;

	dev_dbg(dev, "Buffer release. file=0x%pK, buf=0x%pK\n",
		file, buf);

	ethosu_buffer_put(buf);

	return 0;
}

static int ethosu_buffer_mmap(struct file *file,
			      struct vm_area_struct *vma)
{
	struct ethosu_buffer *buf = file->private_data;
	struct device *dev = buf->dev;
	int ret;

	dev_dbg(dev, "Buffer mmap. file=0x%pK, buf=0x%pK\n",
		file, buf);

	ret = dma_mmap_coherent(dev, vma, buf->dma_mem->cpu_addr,
				buf->dma_mem->dma_addr, buf->dma_mem->size);

	return ret;
}

static loff_t ethosu_buffer_llseek(struct file *file,
				   loff_t offset,
				   int whence)
{
	struct ethosu_buffer *buf = file->private_data;

	if (offset != 0)
		return -EINVAL;

	/*
	 * SEEK_END and SEEK_SET is supported with a zero offset to allow buffer
	 * size discovery using seek functions e.g.
	 * size = lseek(buf_fd, 0, SEEK_END);
	 * lseek(buf_fd, 0, SEEK_SET);
	 */
	switch (whence) {
	case SEEK_END:
		return buf->dma_mem->size;
	case SEEK_SET:
		return 0;
	default:
		return -EINVAL;
	}
}

int ethosu_buffer_create(struct device *dev,
			 size_t size)
{
	struct ethosu_buffer *buf;
	int ret = -ENOMEM;

	if (!size) {
		dev_err(dev, "Buffer create. Invalid zero size");

		return -EINVAL;
	}

	buf = devm_kzalloc(dev, sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		dev_err(dev, "Buffer create. Failed to allocate struct");

		return -ENOMEM;
	}

	buf->dev = dev;
	kref_init(&buf->kref);

	buf->dma_mem = ethosu_dma_mem_alloc(dev, size);
	if (IS_ERR(buf->dma_mem)) {
		ret = PTR_ERR(buf->dma_mem);
		dev_err(dev,
			"Buffer create. Failed to allocate DMA memory. ret=%d",
			ret);
		goto free_buf;
	}

	ret = anon_inode_getfd("ethosu-buffer", &ethosu_buffer_fops, buf,
			       O_RDWR | O_CLOEXEC);
	if (ret < 0) {
		dev_err(dev,
			"Buffer create. Failed to get file descriptor. ret=%d",
			ret);
		goto free_dma;
	}

	buf->file = fget(ret);
	buf->file->f_mode |= FMODE_LSEEK;

	fput(buf->file);

	dev_dbg(dev,
		"Buffer create. file=0x%pK, fd=%d, buf=0x%pK, size=%zu, cpu_addr=0x%pK, dma_addr=0x%llx, phys_addr=0x%llx\n",
		buf->file, ret, buf, size, buf->dma_mem->cpu_addr,
		buf->dma_mem->dma_addr, virt_to_phys(buf->dma_mem->cpu_addr));

	return ret;

free_dma:
	ethosu_dma_mem_free(&buf->dma_mem);

free_buf:
	memset(buf, 0, sizeof(*buf));
	devm_kfree(dev, buf);

	return ret;
}

struct ethosu_buffer *ethosu_buffer_get_from_fd(int fd)
{
	struct ethosu_buffer *buf;
	struct file *file;

	file = fget(fd);
	if (!file)
		return ERR_PTR(-EINVAL);

	if (!ethosu_buffer_verify(file)) {
		fput(file);

		return ERR_PTR(-EINVAL);
	}

	buf = file->private_data;
	ethosu_buffer_get(buf);
	fput(file);

	return buf;
}

void ethosu_buffer_get(struct ethosu_buffer *buf)
{
	kref_get(&buf->kref);
}

void ethosu_buffer_put(struct ethosu_buffer *buf)
{
	kref_put(&buf->kref, ethosu_buffer_destroy);
}
