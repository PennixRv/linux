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

#ifndef ETHOSU_RPMSG_VERSION_H
#define ETHOSU_RPMSG_VERSION_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <rpmsg/ethosu_rpmsg_mailbox.h>

#include <linux/types.h>
#include <linux/completion.h>

/****************************************************************************
 * Types
 ****************************************************************************/

struct ethosu_rpmsg_version_rsp;

/**
 * struct ethosu_rpmsg_version - Protocol version internal struct
 */
struct ethosu_rpmsg_version {
	struct device                   *dev;
	struct completion               done;
	struct ethosu_rpmsg_mailbox_msg msg;
	int                             errno;
};

/****************************************************************************
 * Functions
 ****************************************************************************/

void ethosu_rpmsg_version_rsp(struct ethosu_rpmsg_mailbox *mailbox,
			      int msg_id,
			      struct ethosu_rpmsg_version_rsp *rsp);

int ethosu_rpmsg_version_check_request(struct device *dev,
				       struct ethosu_rpmsg_mailbox *mailbox);

#endif /* ETHOSU_RPMSG_VERSION_H */
