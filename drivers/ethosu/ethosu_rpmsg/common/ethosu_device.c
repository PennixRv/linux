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

#include <common/ethosu_device.h>

#include <common/ethosu_buffer.h>
#include <rpmsg/ethosu_rpmsg.h>
#include <rpmsg/ethosu_rpmsg_cancel_inference.h>
#include <rpmsg/ethosu_rpmsg_capabilities.h>
#include <rpmsg/ethosu_rpmsg_inference.h>
#include <rpmsg/ethosu_rpmsg_network.h>
#include <rpmsg/ethosu_rpmsg_network_info.h>
#include <rpmsg/ethosu_rpmsg_version.h>
#include <uapi/ethosu.h>

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/****************************************************************************
 * Defines
 ****************************************************************************/

#define MINOR_BASE      0 /* Minor version starts at 0 */
#define MINOR_COUNT    64 /* Allocate minor versions */

/****************************************************************************
 * Variables
 ****************************************************************************/

static DECLARE_BITMAP(minors, MINOR_COUNT);

/****************************************************************************
 * Functions
 ****************************************************************************/

/* Incoming messages */
static int ethosu_handle_rpmsg(struct rpmsg_device *rpdev,
			       void *data,
			       int len,
			       void *priv,
			       u32 src)
{
	struct ethosu_device *edev = dev_get_drvdata(&rpdev->dev);
	struct device *dev = &edev->dev;
	struct ethosu_rpmsg_mailbox *mbox = &edev->mailbox;
	struct ethosu_rpmsg *rpmsg = data;
	int length = len - sizeof(rpmsg->header);
	int ret = 0;

	if (unlikely(rpmsg->header.magic != ETHOSU_RPMSG_MAGIC)) {
		dev_warn(dev, "Msg: Error invalid message magic. magic=0x%08x",
			 rpmsg->header.magic);

		return -EBADMSG;
	}

	device_lock(dev);

	dev_dbg(dev,
		"Msg: magic=0x%08x, type=%u, msg_id=%llu",
		rpmsg->header.magic, rpmsg->header.type, rpmsg->header.msg_id);

	switch (rpmsg->header.type) {
	case ETHOSU_RPMSG_ERR:
		if (length != sizeof(rpmsg->error)) {
			dev_warn(dev,
				 "Msg: Error message of incorrect size. size=%u, expected=%zu", length,
				 sizeof(rpmsg->error));
			ret = -EBADMSG;
			break;
		}

		rpmsg->error.msg[sizeof(rpmsg->error.msg) - 1] = '\0';
		dev_warn(dev, "Msg: Error. type=%u, msg=\"%s\"",
			 rpmsg->error.type, rpmsg->error.msg);

		rproc_report_crash(rproc_get_by_child(dev), RPROC_FATAL_ERROR);
		break;
	case ETHOSU_RPMSG_PING:
		dev_dbg(dev, "Msg: Ping");
		ret = ethosu_rpmsg_mailbox_pong(mbox);
		break;
	case ETHOSU_RPMSG_PONG:
		dev_dbg(dev, "Msg: Pong");
		break;
	case ETHOSU_RPMSG_INFERENCE_RSP:
		if (length != sizeof(rpmsg->inf_rsp)) {
			dev_warn(dev,
				 "Msg: Inference response of incorrect size. size=%u, expected=%zu", length,
				 sizeof(rpmsg->inf_rsp));
			ret = -EBADMSG;
			break;
		}

		dev_dbg(dev,
			"Msg: Inference response. ofm_count=%u, status=%u",
			rpmsg->inf_rsp.ofm_count, rpmsg->inf_rsp.status);

		ethosu_rpmsg_inference_rsp(mbox, rpmsg->header.msg_id,
					   &rpmsg->inf_rsp);
		break;
	case ETHOSU_RPMSG_CANCEL_INFERENCE_RSP:
		if (length != sizeof(rpmsg->cancel_rsp)) {
			dev_warn(dev,
				 "Msg: Cancel Inference response of incorrect size. size=%u, expected=%zu", length,
				 sizeof(rpmsg->cancel_rsp));
			ret = -EBADMSG;
			break;
		}

		dev_dbg(dev,
			"Msg: Cancel Inference response. status=%u",
			rpmsg->cancel_rsp.status);
		ethosu_rpmsg_cancel_inference_rsp(mbox,
						  rpmsg->header.msg_id,
						  &rpmsg->cancel_rsp);
		break;
	case ETHOSU_RPMSG_VERSION_RSP:
		if (length != sizeof(rpmsg->version_rsp)) {
			dev_warn(dev,
				 "Msg: Protocol version response of incorrect size. size=%u, expected=%zu", length,
				 sizeof(rpmsg->version_rsp));
			ret = -EBADMSG;
			break;
		}

		dev_dbg(dev, "Msg: Protocol version response %u.%u.%u",
			rpmsg->version_rsp.major, rpmsg->version_rsp.minor,
			rpmsg->version_rsp.patch);

		ethosu_rpmsg_version_rsp(mbox, rpmsg->header.msg_id,
					 &rpmsg->version_rsp);
		break;
	case ETHOSU_RPMSG_CAPABILITIES_RSP:
		if (length != sizeof(rpmsg->cap_rsp)) {
			dev_warn(dev,
				 "Msg: Capabilities response of incorrect size. size=%u, expected=%zu", length,
				 sizeof(rpmsg->cap_rsp));
			ret = -EBADMSG;
			break;
		}

		dev_dbg(dev,
			"Msg: Capabilities response vs%hhu v%hhu.%hhu p%hhu av%hhu.%hhu.%hhu dv%hhu.%hhu.%hhu mcc%hhu csv%hhu cd%hhu",
			rpmsg->cap_rsp.version_status,
			rpmsg->cap_rsp.version_major,
			rpmsg->cap_rsp.version_minor,
			rpmsg->cap_rsp.product_major,
			rpmsg->cap_rsp.arch_major_rev,
			rpmsg->cap_rsp.arch_minor_rev,
			rpmsg->cap_rsp.arch_patch_rev,
			rpmsg->cap_rsp.driver_major_rev,
			rpmsg->cap_rsp.driver_minor_rev,
			rpmsg->cap_rsp.driver_patch_rev,
			rpmsg->cap_rsp.macs_per_cc,
			rpmsg->cap_rsp.cmd_stream_version,
			rpmsg->cap_rsp.custom_dma);

		ethosu_capability_rsp(mbox, rpmsg->header.msg_id,
				      &rpmsg->cap_rsp);
		break;
	case ETHOSU_RPMSG_NETWORK_INFO_RSP:
		if (length != sizeof(rpmsg->net_info_rsp)) {
			dev_warn(dev,
				 "Msg: Network info response of incorrect size. size=%u, expected=%zu", length,
				 sizeof(rpmsg->net_info_rsp));
			ret = -EBADMSG;
			break;
		}

		dev_dbg(dev,
			"Msg: Network info response. status=%u",
			rpmsg->net_info_rsp.status);

		ethosu_rpmsg_network_info_rsp(mbox,
					      rpmsg->header.msg_id,
					      &rpmsg->net_info_rsp);

		break;
	default:
		/* This should not happen due to version checks */
		dev_warn(dev, "Msg: Protocol error. type=%u",
			 rpmsg->header.type);
		ret = -EPROTO;
		break;
	}

	device_unlock(dev);

	wake_up(&mbox->send_queue);

	return ret;
}

static int ethosu_open(struct inode *inode,
		       struct file *file)
{
	struct cdev *cdev = inode->i_cdev;
	struct ethosu_device *edev = container_of(cdev, struct ethosu_device,
						  cdev);
	struct rpmsg_device *rpdev = edev->rpdev;
	struct device *dev = &edev->dev;

	dev_dbg(dev, "Device open. file=0x%pK", file);

	file->private_data = rpdev;

	return nonseekable_open(inode, file);
}

static long ethosu_ioctl(struct file *file,
			 unsigned int cmd,
			 unsigned long arg)
{
	struct rpmsg_device *rpdev = file->private_data;
	struct ethosu_device *edev = dev_get_drvdata(&rpdev->dev);
	struct device *dev = &edev->dev;
	void __user *udata = (void __user *)arg;
	int ret;

	switch (cmd) {
	case ETHOSU_IOCTL_DRIVER_VERSION_GET: {
		const struct ethosu_uapi_kernel_driver_version version = {
			.major = ETHOSU_KERNEL_DRIVER_VERSION_MAJOR,
			.minor = ETHOSU_KERNEL_DRIVER_VERSION_MINOR,
			.patch = ETHOSU_KERNEL_DRIVER_VERSION_PATCH,
		};

		ret = copy_to_user(udata, &version,
				   sizeof(version)) ? -EFAULT : 0;
		break;
	}
	case ETHOSU_IOCTL_CAPABILITIES_REQ: {
		dev_dbg(dev, "Device ioctl: Capabilities request");

		ret = copy_to_user(udata, &edev->capabilities,
				   sizeof(edev->capabilities)) ? -EFAULT : 0;
		break;
	}
	case ETHOSU_IOCTL_PING: {
		ret = device_lock_interruptible(dev);
		if (ret)
			return ret;

		dev_dbg(dev, "Device ioctl: Send ping");

		ret = ethosu_rpmsg_mailbox_ping(&edev->mailbox);

		device_unlock(dev);

		break;
	}
	case ETHOSU_IOCTL_BUFFER_CREATE: {
		struct ethosu_uapi_buffer_create uapi;

		if (copy_from_user(&uapi, udata, sizeof(uapi))) {
			ret = -EFAULT;
			break;
		}

		ret = device_lock_interruptible(dev);
		if (ret)
			return ret;

		dev_dbg(dev,
			"Device ioctl: Buffer create. size=%u",
			uapi.size);

		ret = ethosu_buffer_create(dev, uapi.size);

		device_unlock(dev);

		break;
	}
	case ETHOSU_IOCTL_NETWORK_CREATE: {
		struct ethosu_uapi_network_create uapi;

		if (copy_from_user(&uapi, udata, sizeof(uapi))) {
			ret = -EFAULT;
			break;
		}

		ret = device_lock_interruptible(dev);
		if (ret)
			return ret;

		dev_dbg(dev,
			"Device ioctl: Network create. type=%u\n", uapi.type);

		ret = ethosu_rpmsg_network_create(dev, &edev->mailbox, &uapi);

		device_unlock(dev);

		break;
	}
	default: {
		dev_err(dev, "Invalid ioctl. cmd=%u, arg=%lu",
			cmd, arg);
		ret = -ENOIOCTLCMD;
		break;
	}
	}

	return ret;
}

static struct rpmsg_endpoint *ethosu_create_ept(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpmsg_channel_info info = { 0 };
	struct rpmsg_endpoint *ept;

	/* Create rpmsg endpoint */
	strncpy(info.name, rpdev->id.name, sizeof(info.name) - 1);
	info.src = 0;
	info.dst = rpdev->dst;

	dev_dbg(dev, "Creating rpmsg endpoint. name=%s, src=%u, dst=%u",
		info.name, info.src, info.dst);

	ept = rpmsg_create_ept(rpdev, ethosu_handle_rpmsg, NULL, info);
	if (!ept) {
		dev_err(&rpdev->dev, "Failed to create endpoint");

		return ERR_PTR(-EINVAL);
	}

	return ept;
}

static const struct file_operations fops = {
	.owner          = THIS_MODULE,
	.open           = &ethosu_open,
	.unlocked_ioctl = &ethosu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = &ethosu_ioctl,
#endif
};

static void ethosu_dev_release(struct device *dev)
{
	struct ethosu_device *edev = dev_get_drvdata(dev);

	clear_bit(MINOR(edev->cdev.dev), minors);

	ethosu_rpmsg_mailbox_deinit(&edev->mailbox);
	device_destroy(edev->class, edev->cdev.dev);
	kfree(edev);
}

static int ethosu_device_register(struct device *dev,
				  struct device *parent,
				  void *drvdata,
				  dev_t devt)
{
	struct rproc *rproc = rproc_get_by_child(parent);
	int ret;

	dev->parent = parent;
	dev->release = ethosu_dev_release;
	dev_set_drvdata(dev, drvdata);

	ret = dev_set_name(dev, "ethosu%d", MINOR(devt));
	if (ret) {
		dev_err(parent, "Failed to set device name. ret=%d", ret);

		return ret;
	}

	/* Inherit DMA mask from rproc device */
	ret = dma_coerce_mask_and_coherent(dev,
					   dma_get_mask(rproc->dev.parent));
	if (ret) {
		dev_err(parent, "Failed to set DMA mask. ret=%d", ret);

		return ret;
	}

	/* Inherit DMA configuration from rproc device */
	ret = of_dma_configure(dev, rproc->dev.parent->of_node, false);
	if (ret) {
		dev_err(parent, "Failed to configure DMA. ret=%d",
			ret);

		return ret;
	}

	/* Inherit reserved memory from rproc device */
	ret = of_reserved_mem_device_init_by_idx(dev,
						 rproc->dev.parent->of_node, 0);
	if (ret) {
		dev_err(parent, "Failed to initialize reserved memory. ret=%d",
			ret);

		return ret;
	}

	ret = device_register(dev);
	if (ret) {
		dev_err(parent, "Failed to register device. ret=%d", ret);

		return ret;
	}

	return 0;
}

int ethosu_dev_init(struct rpmsg_device *rpdev,
		    struct class *class,
		    dev_t devt)
{
	struct device *dev = &rpdev->dev;
	struct ethosu_device *edev;
	struct device *sysdev;
	int minor;
	int ret;

	/* Reserve minor number for device node */
	minor = find_first_zero_bit(minors, MINOR_COUNT);
	if (minor >= MINOR_COUNT) {
		dev_err(dev, "No more minor numbers.");

		return -ENOMEM;
	}

	devt = MKDEV(MAJOR(devt), minor);

	/* Allocate and create Ethos-U device */
	edev = kzalloc(sizeof(*edev), GFP_KERNEL);
	if (!edev)
		return -ENOMEM;

	dev_set_drvdata(&rpdev->dev, edev);

	edev->rpdev = rpdev;
	edev->class = class;

	/* Create device object */
	ret = ethosu_device_register(&edev->dev, &rpdev->dev, edev,
				     devt);
	if (ret) {
		kfree(edev);

		return ret;
	}

	/* Continue with new device */
	dev = &edev->dev;

	/* Create RPMsg endpoint */
	edev->ept = ethosu_create_ept(rpdev);
	if (IS_ERR(edev->ept)) {
		ret = PTR_ERR(edev->ept);
		goto device_unregister;
	}

	ret = ethosu_rpmsg_mailbox_init(&edev->mailbox, dev, edev->ept);
	if (ret)
		goto free_rpmsg_ept;

	device_lock(dev);
	ret = ethosu_rpmsg_version_check_request(dev, &edev->mailbox);
	device_unlock(dev);
	if (ret) {
		dev_err(dev, "Protocol version check failed: %d", ret);
		goto deinit_mailbox;
	}

	device_lock(dev);
	ret = ethosu_rpmsg_capabilities_request(dev, &edev->mailbox,
						&edev->capabilities);
	device_unlock(dev);
	if (ret) {
		dev_err(dev, "Failed to get device capabilities: %d", ret);
		goto deinit_mailbox;
	}

	/* Create device node */
	cdev_init(&edev->cdev, &fops);
	edev->cdev.owner = THIS_MODULE;

	cdev_set_parent(&edev->cdev, &dev->kobj);

	ret = cdev_add(&edev->cdev, devt, 1);
	if (ret) {
		dev_err(dev, "Failed to add character device.");
		goto deinit_mailbox;
	}

	sysdev = device_create(edev->class, NULL, devt, rpdev,
			       "ethosu%d", MINOR(devt));
	if (IS_ERR(sysdev)) {
		dev_err(dev, "Failed to create device.");
		ret = PTR_ERR(sysdev);
		goto del_cdev;
	}

	set_bit(minor, minors);

	dev_info(dev,
		 "Created Arm Ethos-U device. name=%s, major=%d, minor=%d",
		 dev_name(sysdev), MAJOR(devt), MINOR(devt));

	return 0;

del_cdev:
	cdev_del(&edev->cdev);

deinit_mailbox:
	ethosu_rpmsg_mailbox_deinit(&edev->mailbox);

free_rpmsg_ept:
	rpmsg_destroy_ept(edev->ept);

device_unregister:
	device_unregister(dev);

	return ret;
}

void ethosu_dev_deinit(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct ethosu_device *edev = dev_get_drvdata(dev);

	device_lock(&edev->dev);
	ethosu_rpmsg_mailbox_fail(&edev->mailbox);
	device_unlock(&edev->dev);

	rpmsg_destroy_ept(edev->ept);
	cdev_del(&edev->cdev);
	device_unregister(&edev->dev);
}
