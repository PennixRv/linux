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

#include <rpmsg/ethosu_rpmsg_mailbox.h>

#include <common/ethosu_buffer.h>
#include <common/ethosu_device.h>
#include <common/ethosu_dma_mem.h>
#include <rpmsg/ethosu_rpmsg.h>

#include <rpmsg/ethosu_rpmsg_network.h>

#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/resource.h>
#include <linux/uio.h>
#include <linux/bug.h>

/****************************************************************************
 * Includes
 ****************************************************************************/

#ifndef fallthrough
#if __has_attribute(__fallthrough__)
#define fallthrough __attribute__((__fallthrough__))
#else
#define fallthrough do {} while (0)  /* fallthrough */
#endif
#endif

/****************************************************************************
 * Defines
 ****************************************************************************/

#define MAILBOX_SEND_TIMEOUT_MS 15000

/****************************************************************************
 * Functions
 ****************************************************************************/

/**
 * ethosu_send_locked() - Blocking mailbox message sender
 *
 * Context: Can sleep and must be called with the device mutex locked.
 *
 * Return: 0 on success, else error code.
 */
static int ethosu_send_locked(struct ethosu_rpmsg_mailbox *mbox,
			      void *data,
			      size_t length)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct device *dev = mbox->dev;
	long timeout = msecs_to_jiffies(MAILBOX_SEND_TIMEOUT_MS);
	bool try_send = !wq_has_sleeper(&mbox->send_queue);
	int ret;

	might_sleep();

	/* Exclusive wait to only wake up one task at a time */
	add_wait_queue_exclusive(&mbox->send_queue, &wait);
	for (;;) {
		/* Stop if the mailbox is closing down */
		if (atomic_read(&mbox->done)) {
			ret = -ENODEV;
			break;
		}

		/* Attempt to send if queue is empty or task was woken up */
		if (try_send) {
			ret = rpmsg_trysend(mbox->ept, data, length);
			if (ret != -ENOMEM)
				break;
		} else {
			try_send = true;
		}

		/* Unlock device mutex while waiting to not block other tasks */
		device_unlock(dev);
		timeout = wait_woken(&wait, TASK_INTERRUPTIBLE, timeout);
		device_lock(dev);

		/* Stop if the wait was interrupted */
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		if (!timeout) {
			ret = -ETIME;
			break;
		}
	}

	remove_wait_queue(&mbox->send_queue, &wait);

	/*
	 * If the message was sent successfully, there may be more TX buffers
	 * available so wake up the next waiting task.
	 */
	if (!ret && wq_has_sleeper(&mbox->send_queue))
		wake_up(&mbox->send_queue);

	return ret;
}

static void ethosu_rpmsg_buffer_dma_mem_set(struct ethosu_dma_mem *dma_mem,
					    struct ethosu_rpmsg_buffer *cbuf)
{
	cbuf->ptr = (uint32_t)dma_mem->dma_addr;
	cbuf->size = (uint32_t)dma_mem->size;
}

int ethosu_rpmsg_mailbox_register(struct ethosu_rpmsg_mailbox *mbox,
				  struct ethosu_rpmsg_mailbox_msg *msg)
{
	WARN_ON_ONCE(!mutex_is_locked(&mbox->dev->mutex));
	msg->id = idr_alloc_cyclic(&mbox->msg_idr, msg, 0, INT_MAX, GFP_KERNEL);
	if (msg->id < 0)
		return msg->id;

	return 0;
}

void ethosu_rpmsg_mailbox_deregister(struct ethosu_rpmsg_mailbox *mbox,
				     struct ethosu_rpmsg_mailbox_msg *msg)
{
	WARN_ON_ONCE(!mutex_is_locked(&mbox->dev->mutex));
	idr_remove(&mbox->msg_idr, msg->id);
}

struct ethosu_rpmsg_mailbox_msg *ethosu_rpmsg_mailbox_find(
	struct ethosu_rpmsg_mailbox *mbox,
	int msg_id,
	uint32_t msg_type)
{
	struct ethosu_rpmsg_mailbox_msg *ptr;

	WARN_ON_ONCE(!mutex_is_locked(&mbox->dev->mutex));
	ptr =
		(struct ethosu_rpmsg_mailbox_msg *)idr_find(&mbox->msg_idr,
							    msg_id);

	if (ptr == NULL)
		return ERR_PTR(-ENOENT);

	if (ptr->type != msg_type)
		return ERR_PTR(-EINVAL);

	return ptr;
}

void ethosu_rpmsg_mailbox_fail(struct ethosu_rpmsg_mailbox *mbox)
{
	struct ethosu_rpmsg_mailbox_msg *cur;
	int id;

	WARN_ON_ONCE(!mutex_is_locked(&mbox->dev->mutex));
	idr_for_each_entry(&mbox->msg_idr, cur, id) {
		cur->fail(cur);
	}
}

int ethosu_rpmsg_mailbox_ping(struct ethosu_rpmsg_mailbox *mbox)
{
	struct ethosu_rpmsg rpmsg = {
		.header        = {
			.magic = ETHOSU_RPMSG_MAGIC,
			.type  = ETHOSU_RPMSG_PING,
		}
	};

	return ethosu_send_locked(mbox, &rpmsg, sizeof(rpmsg.header));
}

int ethosu_rpmsg_mailbox_pong(struct ethosu_rpmsg_mailbox *mbox)
{
	struct ethosu_rpmsg rpmsg = {
		.header        = {
			.magic = ETHOSU_RPMSG_MAGIC,
			.type  = ETHOSU_RPMSG_PONG,
		}
	};

	return ethosu_send_locked(mbox, &rpmsg, sizeof(rpmsg.header));
}

int ethosu_rpmsg_mailbox_version_request(struct ethosu_rpmsg_mailbox *mbox,
					 struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg rpmsg = {
		.header         = {
			.magic  = ETHOSU_RPMSG_MAGIC,
			.type   = ETHOSU_RPMSG_VERSION_REQ,
			.msg_id = msg->id
		}
	};

	msg->type = rpmsg.header.type;

	return ethosu_send_locked(mbox, &rpmsg, sizeof(rpmsg.header));
}

int ethosu_rpmsg_mailbox_capabilities_request(struct ethosu_rpmsg_mailbox *mbox,
					      struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg rpmsg = {
		.header         = {
			.magic  = ETHOSU_RPMSG_MAGIC,
			.type   = ETHOSU_RPMSG_CAPABILITIES_REQ,
			.msg_id = msg->id
		}
	};

	msg->type = rpmsg.header.type;

	return ethosu_send_locked(mbox, &rpmsg, sizeof(rpmsg.header));
}

int ethosu_rpmsg_mailbox_inference(struct ethosu_rpmsg_mailbox *mbox,
				   struct ethosu_rpmsg_mailbox_msg *msg,
				   uint32_t ifm_count,
				   struct ethosu_buffer **ifm,
				   uint32_t ofm_count,
				   struct ethosu_buffer **ofm,
				   struct ethosu_rpmsg_network *network,
				   uint8_t *pmu_event_config,
				   uint8_t pmu_event_config_count,
				   uint8_t pmu_cycle_counter_enable)
{
	struct ethosu_rpmsg rpmsg = {
		.header         = {
			.magic  = ETHOSU_RPMSG_MAGIC,
			.type   = ETHOSU_RPMSG_INFERENCE_REQ,
			.msg_id = msg->id
		}
	};
	struct ethosu_rpmsg_inference_req *inf_req = &rpmsg.inf_req;
	uint32_t i;

	msg->type = rpmsg.header.type;

	/* Verify that the uapi and core has the same number of pmus */
	if (pmu_event_config_count != ETHOSU_RPMSG_PMU_MAX) {
		dev_err(mbox->dev, "PMU count misconfigured.");

		return -EINVAL;
	}

	inf_req->ifm_count = ifm_count;
	inf_req->ofm_count = ofm_count;
	inf_req->pmu_cycle_counter_enable = pmu_cycle_counter_enable;

	for (i = 0; i < ifm_count; i++)
		ethosu_rpmsg_buffer_dma_mem_set(ifm[i]->dma_mem,
						&inf_req->ifm[i]);

	for (i = 0; i < ofm_count; i++)
		ethosu_rpmsg_buffer_dma_mem_set(ofm[i]->dma_mem,
						&inf_req->ofm[i]);

	for (i = 0; i < ETHOSU_RPMSG_PMU_MAX; i++)
		inf_req->pmu_event_config[i] = pmu_event_config[i];

	if (network->dma_mem != NULL) {
		inf_req->network.type = ETHOSU_RPMSG_NETWORK_BUFFER;
		ethosu_rpmsg_buffer_dma_mem_set(network->dma_mem,
						&inf_req->network.buffer);
	} else {
		inf_req->network.type = ETHOSU_RPMSG_NETWORK_INDEX;
		inf_req->network.index = network->index;
	}

	return ethosu_send_locked(mbox, &rpmsg,
				  sizeof(rpmsg.header) + sizeof(rpmsg.inf_req));
}

int ethosu_rpmsg_mailbox_network_info_request(struct ethosu_rpmsg_mailbox *mbox,
					      struct ethosu_rpmsg_mailbox_msg *msg,
					      struct ethosu_rpmsg_network *network)
{
	struct ethosu_rpmsg rpmsg = {
		.header         = {
			.magic  = ETHOSU_RPMSG_MAGIC,
			.type   = ETHOSU_RPMSG_NETWORK_INFO_REQ,
			.msg_id = msg->id
		}
	};
	struct ethosu_rpmsg_network_info_req *info_req = &rpmsg.net_info_req;

	msg->type = rpmsg.header.type;

	if (network->dma_mem != NULL) {
		info_req->network.type = ETHOSU_RPMSG_NETWORK_BUFFER;
		ethosu_rpmsg_buffer_dma_mem_set(network->dma_mem,
						&info_req->network.buffer);
	} else {
		info_req->network.type = ETHOSU_RPMSG_NETWORK_INDEX;
		info_req->network.index = network->index;
	}

	return ethosu_send_locked(mbox, &rpmsg,
				  sizeof(rpmsg.header) +
				  sizeof(rpmsg.net_info_req));
}

int ethosu_rpmsg_mailbox_cancel_inference(struct ethosu_rpmsg_mailbox *mbox,
					  struct ethosu_rpmsg_mailbox_msg *msg,
					  int inference_handle)
{
	struct ethosu_rpmsg rpmsg = {
		.header                   = {
			.magic            = ETHOSU_RPMSG_MAGIC,
			.type             =
				ETHOSU_RPMSG_CANCEL_INFERENCE_REQ,
			.msg_id           = msg->id
		},
		.cancel_req               = {
			.inference_handle = inference_handle
		}
	};

	msg->type = rpmsg.header.type;

	return ethosu_send_locked(mbox, &rpmsg,
				  sizeof(rpmsg.header) +
				  sizeof(rpmsg.cancel_req));
}

int ethosu_rpmsg_mailbox_init(struct ethosu_rpmsg_mailbox *mbox,
			      struct device *dev,
			      struct rpmsg_endpoint *ept)
{
	mbox->dev = dev;
	mbox->ept = ept;
	idr_init(&mbox->msg_idr);
	init_waitqueue_head(&mbox->send_queue);

	return 0;
}

void ethosu_rpmsg_mailbox_deinit(struct ethosu_rpmsg_mailbox *mbox)
{
	atomic_set(&mbox->done, 1);
	wake_up_all(&mbox->send_queue);
}
