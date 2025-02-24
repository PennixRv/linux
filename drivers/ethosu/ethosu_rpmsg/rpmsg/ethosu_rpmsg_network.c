/*
 * SPDX-FileCopyrightText: Copyright 2020, 2022-2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
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

#include <rpmsg/ethosu_rpmsg_network.h>

#include <common/ethosu_device.h>
#include <common/ethosu_dma_mem.h>
#include <rpmsg/ethosu_rpmsg_inference.h>
#include <rpmsg/ethosu_rpmsg_network_info.h>
#include <uapi/ethosu.h>

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

/****************************************************************************
 * Variables
 ****************************************************************************/

static int ethosu_rpmsg_network_release(struct inode *inode,
					struct file *file);

static long ethosu_rpmsg_network_ioctl(struct file *file,
				       unsigned int cmd,
				       unsigned long arg);

static const struct file_operations ethosu_rpmsg_network_fops = {
	.release        = &ethosu_rpmsg_network_release,
	.unlocked_ioctl = &ethosu_rpmsg_network_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = &ethosu_rpmsg_network_ioctl,
#endif
};

/****************************************************************************
 * Functions
 ****************************************************************************/

static bool ethosu_rpmsg_network_verify(struct file *file)
{
	return file->f_op == &ethosu_rpmsg_network_fops;
}

static void ethosu_rpmsg_network_destroy(struct kref *kref)
{
	struct ethosu_rpmsg_network *net =
		container_of(kref, struct ethosu_rpmsg_network, kref);
	struct device *dev = net->dev;

	dev_dbg(dev, "Network destroy. net=0x%pK\n", net);

	if (net->dma_mem != NULL)
		ethosu_dma_mem_free(&net->dma_mem);

	memset(net, 0, sizeof(*net));
	devm_kfree(dev, net);
}

static int ethosu_rpmsg_network_release(struct inode *inode,
					struct file *file)
{
	struct ethosu_rpmsg_network *net = file->private_data;
	struct device *dev = net->dev;

	dev_dbg(dev, "Network release. file=0x%pK, net=0x%pK\n",
		file, net);

	ethosu_rpmsg_network_put(net);

	return 0;
}

static long ethosu_rpmsg_network_ioctl(struct file *file,
				       unsigned int cmd,
				       unsigned long arg)
{
	struct ethosu_rpmsg_network *net = file->private_data;
	struct device *dev = net->dev;
	void __user *udata = (void __user *)arg;
	int ret;

	ret = device_lock_interruptible(net->dev);
	if (ret)
		return ret;

	switch (cmd) {
	case ETHOSU_IOCTL_NETWORK_INFO: {
		struct ethosu_uapi_network_info uapi = { 0 };

		dev_dbg(dev, "Network ioctl: Network info. net=0x%pK", net);

		ret = ethosu_rpmsg_network_info_request(dev, net->mailbox, net,
							&uapi);
		if (ret)
			break;

		ret = copy_to_user(udata, &uapi, sizeof(uapi)) ? -EFAULT : 0;
		break;
	}
	case ETHOSU_IOCTL_INFERENCE_CREATE: {
		struct ethosu_uapi_inference_create uapi;

		if (copy_from_user(&uapi, udata, sizeof(uapi))) {
			dev_err(dev,
				"Network ioctl: Failed to copy inference request");
			ret = -EFAULT;
			break;
		}

		dev_dbg(dev,
			"Network ioctl: Inference. ifm_fd=%u, ofm_fd=%u",
			uapi.ifm_fd[0], uapi.ofm_fd[0]);

		ret = ethosu_rpmsg_inference_create(dev, net->mailbox, net,
						    &uapi);
		break;
	}
	default: {
		dev_err(dev, "Invalid ioctl. cmd=%u, arg=%lu",
			cmd, arg);
		ret = -ENOIOCTLCMD;
		break;
	}
	}

	device_unlock(net->dev);

	return ret;
}

int ethosu_rpmsg_network_create(struct device *dev,
				struct ethosu_rpmsg_mailbox *mailbox,
				struct ethosu_uapi_network_create *uapi)
{
	struct ethosu_rpmsg_network *net;
	const void __user *data;
	int ret;

	net = devm_kzalloc(dev, sizeof(*net), GFP_KERNEL);
	if (!net) {
		dev_err(dev, "Network create. Failed to allocate struct");

		return -ENOMEM;
	}

	net->dev = dev;
	net->mailbox = mailbox;
	kref_init(&net->kref);

	switch (uapi->type) {
	case ETHOSU_UAPI_NETWORK_USER_BUFFER:
		if (!uapi->network.data_ptr) {
			dev_err(dev,
				"Network create. Invalid network data ptr");
			ret = -EINVAL;
			goto free_net;
		}

		if (!uapi->network.size) {
			dev_err(dev,
				"Network create. Invalid network data size");
			ret = -EINVAL;
			goto free_net;
		}

		net->dma_mem = ethosu_dma_mem_alloc(dev, uapi->network.size);
		if (IS_ERR(net->dma_mem)) {
			ret = PTR_ERR(net->dma_mem);
			dev_err(dev,
				"Network create. Failed to allocate DMA memory. ret=%d",
				ret);
			goto free_net;
		}

		data = u64_to_user_ptr(uapi->network.data_ptr);
		ret = copy_from_user(net->dma_mem->cpu_addr, data,
				     uapi->network.size);
		if (ret) {
			dev_err(dev,
				"Network create. Failed to copy network data from user buffer. ret=%d",
				ret);
			goto free_dma_mem;
		}

		break;
	case ETHOSU_UAPI_NETWORK_INDEX:
		net->index = uapi->index;
		break;
	default:
		dev_err(dev, "Network create. Invalid buffer type. type=%u",
			uapi->type);
		ret = -EINVAL;
		goto free_net;
	}

	ret = anon_inode_getfd("ethosu-network", &ethosu_rpmsg_network_fops,
			       net,
			       O_RDWR | O_CLOEXEC);
	if (ret < 0) {
		dev_err(dev,
			"Network create. Failed to get file descriptor. ret=%d",
			ret);
		goto free_dma_mem;
	}

	net->file = fget(ret);
	fput(net->file);

	dev_dbg(dev,
		"Network create. file=0x%pK, fd=%d, net=0x%pK, buf=0x%pK, index=%u",
		net->file, ret, net, net->dma_mem, net->index);

	return ret;

free_dma_mem:
	if (net->dma_mem != NULL)
		ethosu_dma_mem_free(&net->dma_mem);

free_net:
	memset(net, 0, sizeof(*net));
	devm_kfree(dev, net);

	return ret;
}

struct ethosu_rpmsg_network *ethosu_rpmsg_network_get_from_fd(int fd)
{
	struct ethosu_rpmsg_network *net;
	struct file *file;

	file = fget(fd);
	if (!file)
		return ERR_PTR(-EINVAL);

	if (!ethosu_rpmsg_network_verify(file)) {
		fput(file);

		return ERR_PTR(-EINVAL);
	}

	net = file->private_data;
	ethosu_rpmsg_network_get(net);
	fput(file);

	return net;
}

void ethosu_rpmsg_network_get(struct ethosu_rpmsg_network *net)
{
	kref_get(&net->kref);
}

int ethosu_rpmsg_network_put(struct ethosu_rpmsg_network *net)
{
	return kref_put(&net->kref, ethosu_rpmsg_network_destroy);
}
