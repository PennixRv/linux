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

#include <rpmsg/ethosu_rpmsg_cancel_inference.h>

#include <common/ethosu_device.h>
#include <rpmsg/ethosu_rpmsg.h>
#include <rpmsg/ethosu_rpmsg_inference.h>

#include <linux/remoteproc.h>
#include <linux/wait.h>

/****************************************************************************
 * Defines
 ****************************************************************************/

#define CANCEL_INFERENCE_RESP_TIMEOUT_MS 2000

/****************************************************************************
 * Functions
 ****************************************************************************/

static int ethosu_rpmsg_cancel_inference_send(
	struct ethosu_rpmsg_cancel_inference *cancellation,
	struct ethosu_rpmsg_mailbox *mailbox)
{
	return ethosu_rpmsg_mailbox_cancel_inference(mailbox,
						     &cancellation->msg,
						     cancellation->inf->msg.id);
}

static void ethosu_rpmsg_cancel_inference_fail(
	struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg_cancel_inference *cancellation =
		container_of(msg, typeof(*cancellation), msg);

	if (completion_done(&cancellation->done))
		return;

	cancellation->errno = -EFAULT;
	cancellation->uapi->status = ETHOSU_UAPI_STATUS_ERROR;
	complete(&cancellation->done);
}

int ethosu_rpmsg_cancel_inference_request(struct device *dev,
					  struct ethosu_rpmsg_mailbox *mailbox,
					  struct ethosu_rpmsg_inference *inf,
					  struct ethosu_uapi_cancel_inference_status *uapi)
{
	struct ethosu_rpmsg_cancel_inference *cancellation;
	int ret;
	int timeout;

	if (inf->done) {
		uapi->status = ETHOSU_UAPI_STATUS_ERROR;

		return 0;
	}

	cancellation =
		devm_kzalloc(dev,
			     sizeof(struct ethosu_rpmsg_cancel_inference),
			     GFP_KERNEL);
	if (!cancellation) {
		dev_err(dev, "Cancel inference. Failed to allocate struct");

		return -ENOMEM;
	}

	/* increase ref count on the inference we are refering to */
	ethosu_rpmsg_inference_get(inf);
	/* mark inference ABORTING to avoid resending the inference message */
	inf->status = ETHOSU_UAPI_STATUS_ABORTING;

	cancellation->dev = dev;
	cancellation->inf = inf;
	cancellation->uapi = uapi;
	init_completion(&cancellation->done);
	cancellation->msg.fail = ethosu_rpmsg_cancel_inference_fail;

	ret = ethosu_rpmsg_mailbox_register(mailbox,
					    &cancellation->msg);
	if (ret < 0)
		goto kfree;

	dev_dbg(dev,
		"Inference cancellation create. cancel=0x%pK, msg.id=%ddev",
		cancellation, cancellation->msg.id);

	ret = ethosu_rpmsg_cancel_inference_send(cancellation, mailbox);
	if (0 != ret)
		goto deregister;

	/* Unlock the mutex before going to block on the condition */
	device_unlock(dev);

	/* wait for response to arrive back */
	timeout = wait_for_completion_timeout(&cancellation->done,
					      msecs_to_jiffies(
						      CANCEL_INFERENCE_RESP_TIMEOUT_MS));
	/* take back the mutex before resuming to do anything */
	ret = device_lock_interruptible(dev);
	if (0 != ret)
		goto deregister;

	if (0 == timeout /* timed out*/) {
		dev_warn(dev,
			 "Msg: Cancel Inference response lost - timeoutdev");
		ret = -EIO;

		rproc_report_crash(rproc_get_by_child(dev), RPROC_FATAL_ERROR);
		goto deregister;
	}

	if (cancellation->errno) {
		ret = cancellation->errno;
		rproc_report_crash(rproc_get_by_child(dev), RPROC_FATAL_ERROR);
		goto deregister;
	}

	if (inf->status != ETHOSU_UAPI_STATUS_ABORTED)
		inf->status = ETHOSU_UAPI_STATUS_ABORTED;

deregister:
	ethosu_rpmsg_mailbox_deregister(mailbox,
					&cancellation->msg);

kfree:
	dev_dbg(dev,
		"Cancel inference destroy. cancel=0x%pK", cancellation);

	/* decrease the reference on the inference we are refering to */
	ethosu_rpmsg_inference_put(cancellation->inf);
	devm_kfree(dev, cancellation);

	return ret;
}

void ethosu_rpmsg_cancel_inference_rsp(struct ethosu_rpmsg_mailbox *mailbox,
				       int msg_id,
				       struct ethosu_rpmsg_cancel_inference_rsp *rsp)
{
	struct device *dev = mailbox->dev;
	struct ethosu_rpmsg_mailbox_msg *msg;
	struct ethosu_rpmsg_cancel_inference *cancellation;

	msg = ethosu_rpmsg_mailbox_find(mailbox, msg_id,
					ETHOSU_RPMSG_CANCEL_INFERENCE_REQ);
	if (IS_ERR(msg)) {
		dev_warn(dev,
			 "Id for cancel inference msg not found. Id=0x%x: %ld",
			 msg_id, PTR_ERR(msg));

		return;
	}

	cancellation = container_of(msg, typeof(*cancellation), msg);

	if (completion_done(&cancellation->done))
		return;

	cancellation->errno = 0;
	switch (rsp->status) {
	case ETHOSU_RPMSG_STATUS_OK:
		cancellation->uapi->status = ETHOSU_UAPI_STATUS_OK;
		break;
	case ETHOSU_RPMSG_STATUS_ERROR:
		cancellation->uapi->status = ETHOSU_UAPI_STATUS_ERROR;
		break;
	}

	complete(&cancellation->done);
}
