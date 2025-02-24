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
 */

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <rpmsg/ethosu_rpmsg_network_info.h>

#include <common/ethosu_device.h>
#include <rpmsg/ethosu_rpmsg_network.h>
#include <uapi/ethosu.h>

#include <linux/bug.h>

#define NETWORK_INFO_RESP_TIMEOUT_MS 3000

static inline int ethosu_rpmsg_network_info_send(
	struct ethosu_rpmsg_network_info *info,
	struct ethosu_rpmsg_mailbox *mailbox)
{
	/* Send network info request to firmware */
	return ethosu_rpmsg_mailbox_network_info_request(mailbox,
							 &info->msg,
							 info->net);
}

static void ethosu_rpmsg_network_info_fail(struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg_network_info *info =
		container_of(msg, typeof(*info), msg);

	if (completion_done(&info->done))
		return;

	info->errno = -EFAULT;
	complete(&info->done);
}

int ethosu_rpmsg_network_info_request(struct device *dev,
				      struct ethosu_rpmsg_mailbox *mailbox,
				      struct ethosu_rpmsg_network *net,
				      struct ethosu_uapi_network_info *uapi)
{
	struct ethosu_rpmsg_network_info *info;
	int ret;
	int timeout;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->net = net;
	info->uapi = uapi;
	init_completion(&info->done);
	info->msg.fail = ethosu_rpmsg_network_info_fail;

	ret = ethosu_rpmsg_mailbox_register(mailbox, &info->msg);
	if (ret < 0) {
		dev_err(dev,
			"Network info create. Failed to register message in mailbox. ret=%d",
			ret);
		goto kfree;
	}

	/* Get reference to network */
	ethosu_rpmsg_network_get(info->net);

	ret = ethosu_rpmsg_network_info_send(info, mailbox);
	if (ret)
		goto deregister;

	dev_dbg(dev,
		"Network info create. info=0x%pK, net=0x%pK, msg.id=0x%x\n",
		info, info->net, info->msg.id);

	/* Unlock the device mutex and wait for completion */
	device_unlock(dev);
	timeout = wait_for_completion_timeout(&info->done,
					      msecs_to_jiffies(
						      NETWORK_INFO_RESP_TIMEOUT_MS));
	device_lock(dev);

	if (0 == timeout) {
		dev_warn(dev, "Network info timed out. info=0x%pK",
			 info);

		ret = -ETIME;
		goto deregister;
	}

	ret = info->errno;

deregister:
	ethosu_rpmsg_mailbox_deregister(mailbox, &info->msg);
	ethosu_rpmsg_network_put(info->net);

kfree:
	dev_dbg(dev,
		"Network info destroy. info=0x%pK, msg.id=0x%x\n",
		info, info->msg.id);
	devm_kfree(dev, info);

	return ret;
}

void ethosu_rpmsg_network_info_rsp(struct ethosu_rpmsg_mailbox *mailbox,
				   int msg_id,
				   struct ethosu_rpmsg_network_info_rsp *rsp)
{
	int ret;
	struct device *dev = mailbox->dev;
	struct ethosu_rpmsg_mailbox_msg *msg;
	struct ethosu_rpmsg_network_info *info;
	uint32_t i;
	const size_t rsp_desc_size = sizeof(rsp->desc);

	BUILD_BUG_ON(rsp_desc_size != sizeof(info->uapi->desc));

	msg = ethosu_rpmsg_mailbox_find(mailbox, msg_id,
					ETHOSU_RPMSG_NETWORK_INFO_REQ);
	if (IS_ERR(msg)) {
		dev_warn(dev,
			 "Id for network info msg not found. Id=0x%x: %ld\n",
			 msg_id, PTR_ERR(msg));

		return;
	}

	info = container_of(msg, typeof(*info), msg);

	if (completion_done(&info->done))
		return;

	info->errno = 0;

	if (rsp->status != ETHOSU_RPMSG_STATUS_OK) {
		dev_err(dev, "Failed to get information about the network\n");
		info->errno = -EBADF;
		goto signal_complete;
	}

	if (rsp->ifm_count > ETHOSU_FD_MAX || rsp->ofm_count > ETHOSU_FD_MAX) {
		dev_err(dev,
			"Invalid number of IFMs/OFMs in network info: IFMs=%u OFMs=%u\n",
			rsp->ifm_count, rsp->ofm_count);
		info->errno = -ENFILE;
		goto signal_complete;
	}

	if (strnlen(rsp->desc, rsp_desc_size) == rsp_desc_size) {
		dev_err(dev,
			"Description in network info is not null-terminated\n");
		info->errno = -EMSGSIZE;
		goto signal_complete;
	}

	ret = strscpy(info->uapi->desc, rsp->desc, sizeof(info->uapi->desc));
	if (ret < 0) {
		dev_err(dev, "Failed to copy network info description\n");
		info->errno = ret;
		goto signal_complete;
	}

	info->uapi->ifm_count = rsp->ifm_count;
	for (i = 0; i < rsp->ifm_count; i++)
		info->uapi->ifm_size[i] = rsp->ifm_size[i];

	info->uapi->ofm_count = rsp->ofm_count;
	for (i = 0; i < rsp->ofm_count; i++)
		info->uapi->ofm_size[i] = rsp->ofm_size[i];

signal_complete:
	complete(&info->done);
}
