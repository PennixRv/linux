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

#ifndef ETHOSU_BUFFER_H
#define ETHOSU_BUFFER_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <linux/kref.h>
#include <linux/types.h>

/****************************************************************************
 * Types
 ****************************************************************************/

struct ethosu_dma_mem;
struct ethosu_device;
struct device;

/**
 * struct ethosu_buffer - User data buffer
 * @dev:	Device
 * @file:	File
 * @kref:	Reference counting
 * @dma_mem:	DMA memory allocated for the buffer
 */
struct ethosu_buffer {
	struct device         *dev;
	struct file           *file;
	struct kref           kref;
	struct ethosu_dma_mem *dma_mem;
};

/****************************************************************************
 * Functions
 ****************************************************************************/

/**
 * ethosu_buffer_create() - Create buffer
 *
 * This function must be called in the context of a user space process.
 *
 * Return: fd on success, else error code.
 */
int ethosu_buffer_create(struct device *dev,
			 size_t size);

/**
 * ethosu_buffer_get_from_fd() - Get buffer handle from fd
 *
 * This function must be called from a user space context.
 *
 * Return: Pointer on success, else ERR_PTR.
 */
struct ethosu_buffer *ethosu_buffer_get_from_fd(int fd);

/**
 * ethosu_buffer_get() - Put buffer
 */
void ethosu_buffer_get(struct ethosu_buffer *buf);

/**
 * ethosu_buffer_put() - Put buffer
 */
void ethosu_buffer_put(struct ethosu_buffer *buf);

#endif /* ETHOSU_BUFFER_H */
