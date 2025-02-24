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

#include <common/ethosu_device.h>
#include <uapi/ethosu.h>

#include <linux/bitmap.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/rpmsg.h>

/****************************************************************************
 * Defines
 ****************************************************************************/

#define ETHOSU_DRIVER_STR(s) #s
#define ETHOSU_DRIVER_VERSION_STR(major, minor, patch) \
	ETHOSU_DRIVER_STR(major) "."		       \
	ETHOSU_DRIVER_STR(minor) "."		       \
	ETHOSU_DRIVER_STR(patch)
#define ETHOSU_DRIVER_VERSION ETHOSU_DRIVER_VERSION_STR( \
		ETHOSU_KERNEL_DRIVER_VERSION_MAJOR,	 \
		ETHOSU_KERNEL_DRIVER_VERSION_MINOR,	 \
		ETHOSU_KERNEL_DRIVER_VERSION_PATCH)

#define ETHOSU_DRIVER_NAME    "ethosu"

#define MINOR_BASE      0 /* Minor version starts at 0 */
#define MINOR_COUNT    64 /* Allocate minor versions */

/****************************************************************************
 * Variables
 ****************************************************************************/

static struct class *ethosu_class;

static dev_t devt;

/****************************************************************************
 * Rpmsg driver
 ****************************************************************************/

static int ethosu_rpmsg_probe(struct rpmsg_device *rpdev)
{
	int ret;

	/* Initialize device */
	ret = ethosu_dev_init(rpdev, ethosu_class, devt);
	if (ret)
		return ret;

	return 0;
}

static void ethosu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	ethosu_dev_deinit(rpdev);
}

static int ethosu_rpmsg_cb(struct rpmsg_device *rpdev,
			   void *data,
			   int len,
			   void *priv,
			   u32 src)
{
	dev_err(&rpdev->dev, "%s", __FUNCTION__);

	return -EINVAL;
}

static struct rpmsg_device_id ethosu_rpmsg_driver_id_table[] = {
	{ .name = "ethos-u-0.0" },
	{},
};

MODULE_DEVICE_TABLE(rpmsg, ethosu_rpmsg_driver_id_table);

static struct rpmsg_driver ethosu_rpmsg_driver = {
	.drv                = {
		.name       = ETHOSU_DRIVER_NAME,
		.owner      = THIS_MODULE,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table           = ethosu_rpmsg_driver_id_table,
	.probe              = ethosu_rpmsg_probe,
	.callback           = ethosu_rpmsg_cb,
	.remove             = ethosu_rpmsg_remove,
};

/****************************************************************************
 * Module init and exit
 ****************************************************************************/

static void __exit ethosu_exit(void)
{
	unregister_rpmsg_driver(&ethosu_rpmsg_driver);
	unregister_chrdev_region(devt, MINOR_COUNT);
	class_destroy(ethosu_class);
}

static int __init ethosu_init(void)
{
	int ret;

	ethosu_class = class_create(ETHOSU_DRIVER_NAME);
	if (IS_ERR(ethosu_class)) {
		pr_err("Failed to create class '%s'.\n", ETHOSU_DRIVER_NAME);

		return PTR_ERR(ethosu_class);
	}

	ret = alloc_chrdev_region(&devt, MINOR_BASE, MINOR_COUNT,
				  ETHOSU_DRIVER_NAME);
	if (ret) {
		pr_err("Failed to allocate chrdev region.\n");
		goto destroy_class;
	}

	ret = register_rpmsg_driver(&ethosu_rpmsg_driver);
	if (ret) {
		pr_err("Failed to register Arm Ethos-U rpmsg driver.\n");
		goto region_unregister;
	}

	return 0;

region_unregister:
	unregister_chrdev_region(devt, MINOR_COUNT);

destroy_class:
	class_destroy(ethosu_class);

	return ret;
}

module_init(ethosu_init)
module_exit(ethosu_exit)

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Arm Ltd");
MODULE_DESCRIPTION("Arm Ethos-U NPU Driver");
MODULE_VERSION(ETHOSU_DRIVER_VERSION);
