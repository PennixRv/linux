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

#ifndef ETHOSU_DEVICE_H
#define ETHOSU_DEVICE_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <uapi/ethosu.h>
#include <rpmsg/ethosu_rpmsg_mailbox.h>

#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>

/****************************************************************************
 * Types
 ****************************************************************************/

/**
 * struct ethosu_device - Device structure
 */
struct ethosu_device {
	struct device                          dev;
	struct rpmsg_device                    *rpdev;
	struct rpmsg_endpoint                  *ept;
	struct cdev                            cdev;
	struct class                           *class;
	struct ethosu_rpmsg_mailbox            mailbox;
	struct ethosu_uapi_device_capabilities capabilities;
};

/****************************************************************************
 * Functions
 ****************************************************************************/

/**
 * ethosu_dev_init() - Initialize the device
 *
 * Return: 0 on success, else error code.
 */
int ethosu_dev_init(struct rpmsg_device *rpdev,
		    struct class *class,
		    dev_t devt);

/**
 * ethosu_dev_deinit() - Initialize the device
 */
void ethosu_dev_deinit(struct rpmsg_device *rpdev);

#endif /* ETHOSU_DEVICE_H */
