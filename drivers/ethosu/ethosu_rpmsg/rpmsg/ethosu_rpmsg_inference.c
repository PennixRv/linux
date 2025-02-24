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

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <rpmsg/ethosu_rpmsg_inference.h>

#include <common/ethosu_buffer.h>
#include <common/ethosu_device.h>
#include <rpmsg/ethosu_rpmsg.h>
#include <rpmsg/ethosu_rpmsg_cancel_inference.h>
#include <rpmsg/ethosu_rpmsg_network.h>

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/poll.h>

/****************************************************************************
 * Variables
 ****************************************************************************/

static int ethosu_rpmsg_inference_release(struct inode *inode,
					  struct file *file);

static __poll_t ethosu_rpmsg_inference_poll(struct file *file,
					    poll_table *wait);

static long ethosu_rpmsg_inference_ioctl(struct file *file,
					 unsigned int cmd,
					 unsigned long arg);

static const struct file_operations ethosu_rpmsg_inference_fops = {
	.release        = &ethosu_rpmsg_inference_release,
	.poll           = &ethosu_rpmsg_inference_poll,
	.unlocked_ioctl = &ethosu_rpmsg_inference_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = &ethosu_rpmsg_inference_ioctl,
#endif
};

/****************************************************************************
 * Functions
 ****************************************************************************/

static const char *status_to_string(const enum ethosu_uapi_status status)
{
	switch (status) {
	case ETHOSU_UAPI_STATUS_OK: {
		return "Ok";
	}
	case ETHOSU_UAPI_STATUS_ERROR: {
		return "Error";
	}
	case ETHOSU_UAPI_STATUS_RUNNING: {
		return "Running";
	}
	case ETHOSU_UAPI_STATUS_REJECTED: {
		return "Rejected";
	}
	case ETHOSU_UAPI_STATUS_ABORTED: {
		return "Aborted";
	}
	case ETHOSU_UAPI_STATUS_ABORTING: {
		return "Aborting";
	}
	default: {
		return "Unknown";
	}
	}
}

static int ethosu_rpmsg_inference_send(struct ethosu_rpmsg_inference *inf)
{
	struct device *dev = inf->dev;
	int ret;

	inf->status = ETHOSU_UAPI_STATUS_ERROR;

	ret = ethosu_rpmsg_mailbox_inference(inf->mailbox, &inf->msg,
					     inf->ifm_count, inf->ifm,
					     inf->ofm_count, inf->ofm,
					     inf->net, inf->pmu_event_config,
					     ETHOSU_PMU_EVENT_MAX,
					     inf->pmu_cycle_counter_enable);
	if (ret) {
		dev_warn(dev,
			 "Failed to send inference request. inf=0x%pK, ret=%d",
			 inf, ret);

		return ret;
	}

	inf->status = ETHOSU_UAPI_STATUS_RUNNING;

	ethosu_rpmsg_inference_get(inf);

	return 0;
}

static void ethosu_rpmsg_inference_fail(struct ethosu_rpmsg_mailbox_msg *msg)
{
	struct ethosu_rpmsg_inference *inf =
		container_of(msg, typeof(*inf), msg);
	int ret;

	if (inf->done)
		return;

	/* Decrement reference count if inference was pending reponse */
	ret = ethosu_rpmsg_inference_put(inf);
	if (ret)
		return;

	/* Set status accordingly to the inference state */
	inf->status = inf->status == ETHOSU_UAPI_STATUS_ABORTING ?
		      ETHOSU_UAPI_STATUS_ABORTED :
		      ETHOSU_UAPI_STATUS_ERROR;
	/* Mark it done and wake up the waiting process */
	inf->done = true;
	wake_up_interruptible(&inf->waitq);
}

static bool ethosu_rpmsg_inference_verify(struct file *file)
{
	return file->f_op == &ethosu_rpmsg_inference_fops;
}

static void ethosu_rpmsg_inference_kref_destroy(struct kref *kref)
{
	struct ethosu_rpmsg_inference *inf =
		container_of(kref, struct ethosu_rpmsg_inference, kref);
	struct device *dev = inf->dev;

	dev_dbg(dev,
		"Inference destroy. inf=0x%pK, status=%d, ifm_count=%u, ofm_count=%u",
		inf, inf->status, inf->ifm_count, inf->ofm_count);

	ethosu_rpmsg_mailbox_deregister(inf->mailbox, &inf->msg);

	while (inf->ifm_count-- > 0)
		ethosu_buffer_put(inf->ifm[inf->ifm_count]);

	while (inf->ofm_count-- > 0)
		ethosu_buffer_put(inf->ofm[inf->ofm_count]);

	ethosu_rpmsg_network_put(inf->net);
	memset(inf, 0, sizeof(*inf));
	devm_kfree(dev, inf);
}

static int ethosu_rpmsg_inference_release(struct inode *inode,
					  struct file *file)
{
	struct ethosu_rpmsg_inference *inf = file->private_data;
	struct device *dev = inf->dev;

	dev_dbg(dev,
		"Inference release. file=0x%pK, inf=0x%pK",
		file, inf);

	device_lock(dev);
	ethosu_rpmsg_inference_put(inf);
	device_unlock(dev);

	return 0;
}

static __poll_t ethosu_rpmsg_inference_poll(struct file *file,
					    poll_table *wait)
{
	struct ethosu_rpmsg_inference *inf = file->private_data;
	__poll_t ret = 0;

	poll_wait(file, &inf->waitq, wait);

	if (inf->done)
		ret |= EPOLLIN;

	return ret;
}

static long ethosu_rpmsg_inference_ioctl(struct file *file,
					 unsigned int cmd,
					 unsigned long arg)
{
	struct ethosu_rpmsg_inference *inf = file->private_data;
	struct device *dev = inf->dev;
	void __user *udata = (void __user *)arg;
	int ret;

	ret = device_lock_interruptible(dev);
	if (ret)
		return ret;

	switch (cmd) {
	case ETHOSU_IOCTL_INFERENCE_STATUS: {
		struct ethosu_uapi_result_status uapi = { 0 };
		int i;

		uapi.status = inf->status;

		for (i = 0; i < ETHOSU_PMU_EVENT_MAX; i++) {
			uapi.pmu_config.events[i] =
				inf->pmu_event_config[i];
			uapi.pmu_count.events[i] =
				inf->pmu_event_count[i];
		}

		uapi.pmu_config.cycle_count = inf->pmu_cycle_counter_enable;
		uapi.pmu_count.cycle_count = inf->pmu_cycle_counter_count;

		dev_dbg(dev,
			"Inference ioctl: Inference status. status=%s (%d)\n",
			status_to_string(uapi.status), uapi.status);

		ret = copy_to_user(udata, &uapi, sizeof(uapi)) ? -EFAULT : 0;

		break;
	}
	case ETHOSU_IOCTL_INFERENCE_CANCEL: {
		struct ethosu_uapi_cancel_inference_status uapi = { 0 };

		dev_dbg(dev,
			"Inference ioctl: Cancel Inference. Handle=%p\n",
			inf);

		ret = ethosu_rpmsg_cancel_inference_request(dev, inf->mailbox,
							    inf,
							    &uapi);
		if (ret)
			break;

		ret = copy_to_user(udata, &uapi, sizeof(uapi)) ? -EFAULT : 0;

		break;
	}
	default: {
		dev_err(dev, "Invalid ioctl. cmd=%u, arg=%lu\n",
			cmd, arg);
		ret = -ENOIOCTLCMD;
		break;
	}
	}

	device_unlock(dev);

	return ret;
}

int ethosu_rpmsg_inference_create(struct device *dev,
				  struct ethosu_rpmsg_mailbox *mailbox,
				  struct ethosu_rpmsg_network *net,
				  struct ethosu_uapi_inference_create *uapi)
{
	struct ethosu_rpmsg_inference *inf;
	uint32_t i;
	int fd;
	int ret = -ENOMEM;

	if (uapi->ifm_count > ETHOSU_FD_MAX ||
	    uapi->ofm_count > ETHOSU_FD_MAX) {
		dev_err(dev,
			"Inference create. Too many IFM and/or OFM buffers for inference. ifm_count=%u, ofm_count=%u",
			uapi->ifm_count, uapi->ofm_count);

		return -EFAULT;
	}

	inf = devm_kzalloc(dev, sizeof(*inf), GFP_KERNEL);
	if (!inf) {
		dev_err(dev,
			"Inference create. Failed to allocate struct");

		return -ENOMEM;
	}

	inf->dev = dev;
	inf->mailbox = mailbox;
	inf->net = net;
	inf->done = false;
	inf->status = ETHOSU_UAPI_STATUS_ERROR;
	kref_init(&inf->kref);
	init_waitqueue_head(&inf->waitq);
	inf->msg.fail = ethosu_rpmsg_inference_fail;

	/* Add inference to pending list */
	ret = ethosu_rpmsg_mailbox_register(mailbox, &inf->msg);
	if (ret < 0)
		goto kfree;

	/* Get pointer to IFM buffers */
	for (i = 0; i < uapi->ifm_count; i++) {
		inf->ifm[i] = ethosu_buffer_get_from_fd(uapi->ifm_fd[i]);
		if (IS_ERR(inf->ifm[i])) {
			ret = PTR_ERR(inf->ifm[i]);
			dev_err(dev,
				"Inference create. Failed to get IFM buffer%u ret=%d",
				i, ret);
			goto put_ifm;
		}

		inf->ifm_count++;
	}

	/* Get pointer to OFM buffer */
	for (i = 0; i < uapi->ofm_count; i++) {
		inf->ofm[i] = ethosu_buffer_get_from_fd(uapi->ofm_fd[i]);
		if (IS_ERR(inf->ofm[i])) {
			ret = PTR_ERR(inf->ofm[i]);
			dev_err(dev,
				"Inference create. Failed to get OFM buffer%u ret=%d",
				i, ret);
			goto put_ofm;
		}

		inf->ofm_count++;
	}

	/* Configure PMU and cycle counter */
	dev_dbg(dev,
		"Configuring events for PMU. events=[%u, %u, %u, %u]\n",
		uapi->pmu_config.events[0], uapi->pmu_config.events[1],
		uapi->pmu_config.events[2], uapi->pmu_config.events[3]);

	/* Configure events and reset count for all events */
	for (i = 0; i < ETHOSU_PMU_EVENT_MAX; i++) {
		inf->pmu_event_config[i] = uapi->pmu_config.events[i];
		inf->pmu_event_count[i] = 0;
	}

	/* Configure cycle counter and reset any previous count */
	inf->pmu_cycle_counter_enable = uapi->pmu_config.cycle_count;
	inf->pmu_cycle_counter_count = 0;

	/* Increment network reference count */
	ethosu_rpmsg_network_get(net);

	/* Send inference request to Arm Ethos-U subsystem */
	ret = ethosu_rpmsg_inference_send(inf);
	if (ret)
		goto put_net;

	/* Create file descriptor */
	ret = fd = anon_inode_getfd("ethosu-inference",
				    &ethosu_rpmsg_inference_fops,
				    inf, O_RDWR | O_CLOEXEC);
	if (ret < 0) {
		dev_err(dev,
			"Inference create. Failed to get file descriptor. ret=%d",
			ret);

		goto put_net;
	}

	/* Store pointer to file structure */
	inf->file = fget(ret);
	fput(inf->file);

	dev_dbg(dev,
		"Inference create. file=0x%pK, fd=%d, inf=0x%p, net=0x%pK, msg.id=0x%x",
		inf->file, fd, inf, inf->net, inf->msg.id);

	return fd;

put_net:
	ethosu_rpmsg_network_put(inf->net);

put_ofm:
	while (inf->ofm_count-- > 0)
		ethosu_buffer_put(inf->ofm[inf->ofm_count]);

put_ifm:
	while (inf->ifm_count-- > 0)
		ethosu_buffer_put(inf->ifm[inf->ifm_count]);

	ethosu_rpmsg_mailbox_deregister(mailbox, &inf->msg);

kfree:
	memset(inf, 0, sizeof(*inf));
	devm_kfree(dev, inf);

	return ret;
}

struct ethosu_rpmsg_inference *ethosu_rpmsg_inference_get_from_fd(int fd)
{
	struct ethosu_rpmsg_inference *inf;
	struct file *file;

	file = fget(fd);
	if (!file)
		return ERR_PTR(-EINVAL);

	if (!ethosu_rpmsg_inference_verify(file)) {
		fput(file);

		return ERR_PTR(-EINVAL);
	}

	inf = file->private_data;
	ethosu_rpmsg_inference_get(inf);
	fput(file);

	return inf;
}

void ethosu_rpmsg_inference_get(struct ethosu_rpmsg_inference *inf)
{
	kref_get(&inf->kref);
}

int ethosu_rpmsg_inference_put(struct ethosu_rpmsg_inference *inf)
{
	return kref_put(&inf->kref, &ethosu_rpmsg_inference_kref_destroy);
}

void ethosu_rpmsg_inference_rsp(struct ethosu_rpmsg_mailbox *mailbox,
				int msg_id,
				struct ethosu_rpmsg_inference_rsp *rsp)
{
	struct device *dev = mailbox->dev;
	struct ethosu_rpmsg_mailbox_msg *msg;
	struct ethosu_rpmsg_inference *inf;
	int i;

	msg = ethosu_rpmsg_mailbox_find(mailbox, msg_id,
					ETHOSU_RPMSG_INFERENCE_REQ);
	if (IS_ERR(msg)) {
		dev_warn(dev,
			 "Id for inference msg not found. Id=0x%x: %ld\n",
			 msg_id, PTR_ERR(msg));

		return;
	}

	inf = container_of(msg, typeof(*inf), msg);

	/*
	 * Don't handle the response if the inference is aborted or
	 * in the process of being aborted
	 */
	if (inf->status == ETHOSU_UAPI_STATUS_ABORTED ||
	    inf->status == ETHOSU_UAPI_STATUS_ABORTING) {
		inf->status = ETHOSU_UAPI_STATUS_ABORTED;
		goto done;
	}

	if (rsp->status == ETHOSU_RPMSG_STATUS_OK &&
	    inf->ofm_count <= ETHOSU_RPMSG_BUFFER_MAX)
		inf->status = ETHOSU_UAPI_STATUS_OK;
	else if (rsp->status == ETHOSU_RPMSG_STATUS_REJECTED)
		inf->status = ETHOSU_UAPI_STATUS_REJECTED;
	else if (rsp->status == ETHOSU_RPMSG_STATUS_ABORTED)
		inf->status = ETHOSU_UAPI_STATUS_ABORTED;
	else
		inf->status = ETHOSU_UAPI_STATUS_ERROR;

	if (inf->status == ETHOSU_UAPI_STATUS_OK) {
		for (i = 0; i < ETHOSU_RPMSG_PMU_MAX; i++) {
			inf->pmu_event_config[i] = rsp->pmu_event_config[i];
			inf->pmu_event_count[i] = rsp->pmu_event_count[i];
		}

		inf->pmu_cycle_counter_enable = rsp->pmu_cycle_counter_enable;
		inf->pmu_cycle_counter_count = rsp->pmu_cycle_counter_count;

		dev_dbg(dev,
			"PMU events. config=[%u, %u, %u, %u], count=[%llu, %llu, %llu, %llu]\n",
			inf->pmu_event_config[0], inf->pmu_event_config[1],
			inf->pmu_event_config[2], inf->pmu_event_config[3],
			inf->pmu_event_count[0], inf->pmu_event_count[1],
			inf->pmu_event_count[2], inf->pmu_event_count[3]);

		if (inf->pmu_cycle_counter_enable)
			dev_dbg(dev,
				"PMU cycle counter: count=%llu\n",
				inf->pmu_cycle_counter_count);
	}

done:
	inf->done = true;
	wake_up_interruptible(&inf->waitq);
	ethosu_rpmsg_inference_put(inf);
}
