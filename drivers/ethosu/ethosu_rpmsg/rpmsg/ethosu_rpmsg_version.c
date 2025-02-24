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

#include <rpmsg/ethosu_rpmsg_version.h>
#include <rpmsg/ethosu_rpmsg.h>

#include <linux/errno.h>

/****************************************************************************
 * Defines
 ****************************************************************************/

#define VERSION_RESP_TIMEOUT_MS 2000

/****************************************************************************
 * Functions
 ****************************************************************************/

static void ethosu_rpmsg_version_fail(struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg_version *version =
		container_of(msg, typeof(*version), msg);

	if (completion_done(&version->done))
		return;

	version->errno = -EFAULT;
	complete(&version->done);
}

void ethosu_rpmsg_version_rsp(struct ethosu_rpmsg_mailbox *mailbox,
			      int msg_id,
			      struct ethosu_rpmsg_version_rsp *rsp)
{
	struct device *dev = mailbox->dev;
	struct ethosu_rpmsg_mailbox_msg *msg;
	struct ethosu_rpmsg_version *version;

	msg = ethosu_rpmsg_mailbox_find(mailbox, msg_id,
					ETHOSU_RPMSG_VERSION_REQ);
	if (IS_ERR(msg)) {
		dev_warn(dev,
			 "Id for version msg not found. Id=0x%0x: %ld\n",
			 msg_id, PTR_ERR(msg));

		return;
	}

	version = container_of(msg, typeof(*version), msg);

	if (completion_done(&version->done))
		return;

	if (rsp->major != ETHOSU_RPMSG_VERSION_MAJOR ||
	    rsp->minor != ETHOSU_RPMSG_VERSION_MINOR) {
		dev_warn(dev,
			 "Msg: Protocol version mismatch. Expected %u.%u.X but got %u.%u.%u",
			 ETHOSU_RPMSG_VERSION_MAJOR,
			 ETHOSU_RPMSG_VERSION_MINOR,
			 rsp->major, rsp->minor, rsp->patch);
		version->errno = -EPROTO;
	} else {
		version->errno = 0;
	}

	complete(&version->done);
}

int ethosu_rpmsg_version_check_request(struct device *dev,
				       struct ethosu_rpmsg_mailbox *mailbox)
{
	struct ethosu_rpmsg_version *version;
	int ret;
	int timeout;

	version = devm_kzalloc(dev, sizeof(*version), GFP_KERNEL);
	if (!version)
		return -ENOMEM;

	version->dev = dev;
	init_completion(&version->done);
	version->msg.fail = ethosu_rpmsg_version_fail;

	ret = ethosu_rpmsg_mailbox_register(mailbox, &version->msg);
	if (ret < 0)
		goto free_version;

	dev_dbg(dev, "Protocol version request created. Id=0x%x, handle=%pK\n",
		version->msg.id, version);

	ret = ethosu_rpmsg_mailbox_version_request(mailbox, &version->msg);
	if (ret)
		goto deregister;

	/* Unlock the mutex to not block other messages while waiting */
	device_unlock(dev);

	/* Wait for version response */
	timeout = wait_for_completion_timeout(&version->done,
					      msecs_to_jiffies(
						      VERSION_RESP_TIMEOUT_MS));

	/* Take back the mutex before resuming to do anything */
	device_lock(dev);

	if (0 == timeout) {
		dev_warn(dev, "Protocol version response timeout");
		ret = -ETIME;
		goto deregister;
	}

	if (version->errno) {
		ret = version->errno;
		goto deregister;
	}

deregister:
	ethosu_rpmsg_mailbox_deregister(mailbox, &version->msg);

free_version:
	dev_dbg(dev, "Protocol version destroy. Id=0x%x, handle=%pK\n",
		version->msg.id,
		version);
	devm_kfree(dev, version);

	return ret;
}
