/*
 * SPDX-FileCopyrightText: Copyright 2022-2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
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
 *
 */

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <rpmsg/ethosu_rpmsg_capabilities.h>

#include <common/ethosu_device.h>
#include <rpmsg/ethosu_rpmsg.h>

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>

/****************************************************************************
 * Defines
 ****************************************************************************/

#define CAPABILITIES_RESP_TIMEOUT_MS 2000

/****************************************************************************
 * Functions
 ****************************************************************************/

static inline int ethosu_rpmsg_capabilities_send(
	struct ethosu_rpmsg_capabilities *cap,
	struct ethosu_rpmsg_mailbox *mailbox)
{
	return ethosu_rpmsg_mailbox_capabilities_request(mailbox,
							 &cap->msg);
}

static void ethosu_rpmsg_capabilities_fail(struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg_capabilities *cap =
		container_of(msg, typeof(*cap), msg);

	if (completion_done(&cap->done))
		return;

	cap->errno = -EFAULT;
	complete(&cap->done);
}

void ethosu_capability_rsp(struct ethosu_rpmsg_mailbox *mailbox,
			   int msg_id,
			   struct ethosu_rpmsg_capabilities_rsp *rsp)
{
	struct device *dev = mailbox->dev;
	struct ethosu_rpmsg_mailbox_msg *msg;
	struct ethosu_rpmsg_capabilities *cap;

	msg = ethosu_rpmsg_mailbox_find(mailbox, msg_id,
					ETHOSU_RPMSG_CAPABILITIES_REQ);
	if (IS_ERR(msg)) {
		dev_warn(dev,
			 "Id for capabilities msg not found. Id=0x%0x: %ld\n",
			 msg_id, PTR_ERR(msg));

		return;
	}

	cap = container_of(msg, typeof(*cap), msg);

	if (completion_done(&cap->done))
		return;

	cap->uapi->hw_id.version_status = rsp->version_status;
	cap->uapi->hw_id.version_minor = rsp->version_minor;
	cap->uapi->hw_id.version_major = rsp->version_major;
	cap->uapi->hw_id.product_major = rsp->product_major;
	cap->uapi->hw_id.arch_patch_rev = rsp->arch_patch_rev;
	cap->uapi->hw_id.arch_minor_rev = rsp->arch_minor_rev;
	cap->uapi->hw_id.arch_major_rev = rsp->arch_major_rev;
	cap->uapi->driver_patch_rev = rsp->driver_patch_rev;
	cap->uapi->driver_minor_rev = rsp->driver_minor_rev;
	cap->uapi->driver_major_rev = rsp->driver_major_rev;
	cap->uapi->hw_cfg.macs_per_cc = rsp->macs_per_cc;
	cap->uapi->hw_cfg.cmd_stream_version = rsp->cmd_stream_version;
	cap->uapi->hw_cfg.custom_dma = rsp->custom_dma;
	cap->uapi->hw_cfg.type = ETHOSU_UAPI_DEVICE_SUBSYSTEM;

	cap->errno = 0;
	complete(&cap->done);
}

int ethosu_rpmsg_capabilities_request(struct device *dev,
				      struct ethosu_rpmsg_mailbox *mailbox,
				      struct ethosu_uapi_device_capabilities *uapi)
{
	struct ethosu_rpmsg_capabilities *cap;
	int ret;
	int timeout;

	cap = devm_kzalloc(dev, sizeof(struct ethosu_rpmsg_capabilities),
			   GFP_KERNEL);
	if (!cap)
		return -ENOMEM;

	cap->dev = dev;
	cap->uapi = uapi;
	init_completion(&cap->done);
	cap->msg.fail = ethosu_rpmsg_capabilities_fail;

	ret = ethosu_rpmsg_mailbox_register(mailbox, &cap->msg);
	if (ret < 0)
		goto kfree;

	dev_dbg(dev, "Capabilities create. Id=%d, handle=0x%p\n",
		cap->msg.id, cap);

	ret = ethosu_rpmsg_capabilities_send(cap, mailbox);
	if (0 != ret)
		goto deregister;

	/* Unlock the mutex before going to block on the condition */
	device_unlock(dev);

	/* wait for response to arrive back */
	timeout = wait_for_completion_timeout(&cap->done,
					      msecs_to_jiffies(
						      CAPABILITIES_RESP_TIMEOUT_MS));

	/* take back the mutex before resuming to do anything */
	device_lock(dev);

	if (0 == timeout) {
		dev_warn(dev, "Capabilities response timeout");
		ret = -ETIME;
		goto deregister;
	}

	if (cap->errno) {
		ret = cap->errno;
		goto deregister;
	}

deregister:
	ethosu_rpmsg_mailbox_deregister(mailbox, &cap->msg);

kfree:
	dev_dbg(dev, "Capabilities destroy. Id=%d, handle=0x%p\n",
		cap->msg.id, cap);
	devm_kfree(dev, cap);

	return ret;
}
