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

#ifndef ETHOSU_RPMSG_MAILBOX_H
#define ETHOSU_RPMSG_MAILBOX_H

/****************************************************************************
 * Includes
 ****************************************************************************/
#include <rpmsg/ethosu_rpmsg.h>

#include <linux/types.h>
#include <linux/mailbox_client.h>
#include <linux/wait.h>
#include <linux/idr.h>

/****************************************************************************
 * Types
 ****************************************************************************/

struct device;
struct ethosu_buffer;
struct ethosu_rpmsg;
struct ethosu_rpmsg_core_queue;
struct ethosu_device;
struct ethosu_rpmsg_network;
struct resource;

typedef void (*ethosu_rpmsg_mailbox_cb)(void *user_arg);

struct ethosu_rpmsg_mailbox {
	struct device         *dev;
	struct rpmsg_endpoint *ept;
	struct idr            msg_idr;
	atomic_t              done;
	wait_queue_head_t     send_queue;
};

/**
 * struct ethosu_rpmsg_mailbox_msg - Mailbox message
 * @id: Message id
 * @type: Message request type
 * @fail: Message failure callback
 *
 * The fail callback will be called with the device mutex locked
 */
struct ethosu_rpmsg_mailbox_msg {
	int      id;
	uint32_t type;
	void     (*fail)(struct ethosu_rpmsg_mailbox_msg *msg);
};

/****************************************************************************
 * Functions
 ****************************************************************************/

/**
 * ethosu_rpmsg_mailbox_init() - Initialize mailbox
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_init(struct ethosu_rpmsg_mailbox *mbox,
			      struct device *dev,
			      struct rpmsg_endpoint *ept);

/**
 * ethosu_rpmsg_mailbox_deinit() - Deinitialize mailbox
 */
void ethosu_rpmsg_mailbox_deinit(struct ethosu_rpmsg_mailbox *mbox);

/**
 * ethosu_rpmsg_mailbox_register() - Register the message in mailbox
 *
 * Context: Must be called with the device mutex locked
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_register(struct ethosu_rpmsg_mailbox *mbox,
				  struct ethosu_rpmsg_mailbox_msg *msg);

/**
 * ethosu_rpmsg_mailbox_free_id() - Free the id of the message
 *
 * Context: Must be called with the device mutex locked
 */
void ethosu_rpmsg_mailbox_deregister(struct ethosu_rpmsg_mailbox *mbox,
				     struct ethosu_rpmsg_mailbox_msg *msg);

/**
 * ethosu_rpmsg_mailbox_find() - Find mailbox message
 *
 * Context: Must be called with the device mutex locked
 *
 * Return: a valid pointer on success, otherwise an error ptr.
 */
struct ethosu_rpmsg_mailbox_msg *ethosu_rpmsg_mailbox_find(
	struct ethosu_rpmsg_mailbox *mbox,
	int msg_id,
	uint32_t msg_type);

/**
 * ethosu_rpmsg_mailbox_fail() - Fail mailbox messages
 *
 * Call fail() callback on all messages in pending list.
 *
 * Context: Must be called with the device mutex locked
 */
void ethosu_rpmsg_mailbox_fail(struct ethosu_rpmsg_mailbox *mbox);

/**
 * ethosu_rpmsg_mailbox_reset() - Reset to end of queue
 */
void ethosu_rpmsg_mailbox_reset(struct ethosu_rpmsg_mailbox *mbox);

/**
 * ethosu_rpmsg_mailbox_ping() - Send ping message
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_ping(struct ethosu_rpmsg_mailbox *mbox);

/**
 * ethosu_rpmsg_mailbox_pong() - Send pong response
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_pong(struct ethosu_rpmsg_mailbox *mbox);

/**
 * ethosu_rpmsg_mailbox_version_request() - Send protocol version request
 *
 * Return: 0 on succes, else error code
 */
int ethosu_rpmsg_mailbox_version_request(struct ethosu_rpmsg_mailbox *mbox,
					 struct ethosu_rpmsg_mailbox_msg *msg);

/**
 * ethosu_rpmsg_mailbox_capabilities_request() - Send capabilities request
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_capabilities_request(struct ethosu_rpmsg_mailbox *mbox,
					      struct ethosu_rpmsg_mailbox_msg *msg);

/**
 * ethosu_rpmsg_mailbox_inference() - Send inference
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_inference(struct ethosu_rpmsg_mailbox *mbox,
				   struct ethosu_rpmsg_mailbox_msg *msg,
				   uint32_t ifm_count,
				   struct ethosu_buffer **ifm,
				   uint32_t ofm_count,
				   struct ethosu_buffer **ofm,
				   struct ethosu_rpmsg_network *network,
				   uint8_t *pmu_event_config,
				   uint8_t pmu_event_config_count,
				   uint8_t pmu_cycle_counter_enable);

/**
 * ethosu_rpmsg_mailbox_network_info_request() - Send network info request
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_network_info_request(struct ethosu_rpmsg_mailbox *mbox,
					      struct ethosu_rpmsg_mailbox_msg *msg,
					      struct ethosu_rpmsg_network *network);

/**
 * ethosu_rpmsg_mailbox_cancel_inference() - Send inference cancellation
 *
 * Return: 0 on success, else error code.
 */
int ethosu_rpmsg_mailbox_cancel_inference(struct ethosu_rpmsg_mailbox *mbox,
					  struct ethosu_rpmsg_mailbox_msg *msg,
					  int inference_handle);

#endif /* ETHOSU_RPMSG_MAILBOX_H */
