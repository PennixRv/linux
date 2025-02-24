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

#ifndef ETHOSU_RPMSG_H
#define ETHOSU_RPMSG_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
namespace EthosU {
#endif

/** Maximum number of IFM/OFM buffers per inference */
#define ETHOSU_RPMSG_BUFFER_MAX 16

/** Maximum number of PMU counters to be returned for inference */
#define ETHOSU_RPMSG_PMU_MAX 8

#define ETHOSU_RPMSG_MAGIC 0x41457631
#define ETHOSU_RPMSG_VERSION_MAJOR 0
#define ETHOSU_RPMSG_VERSION_MINOR 2
#define ETHOSU_RPMSG_VERSION_PATCH 0

/**
 * enum ethosu_rpmsg_type - Message types
 *
 * Types for the messages sent between the host and the core subsystem.
 */
enum ethosu_rpmsg_type {
	ETHOSU_RPMSG_ERR = 1,
	ETHOSU_RPMSG_PING,
	ETHOSU_RPMSG_PONG,
	ETHOSU_RPMSG_INFERENCE_REQ,
	ETHOSU_RPMSG_INFERENCE_RSP,
	ETHOSU_RPMSG_VERSION_REQ,
	ETHOSU_RPMSG_VERSION_RSP,
	ETHOSU_RPMSG_CAPABILITIES_REQ,
	ETHOSU_RPMSG_CAPABILITIES_RSP,
	ETHOSU_RPMSG_NETWORK_INFO_REQ,
	ETHOSU_RPMSG_NETWORK_INFO_RSP,
	ETHOSU_RPMSG_CANCEL_INFERENCE_REQ,
	ETHOSU_RPMSG_CANCEL_INFERENCE_RSP,
	ETHOSU_RPMSG_MAX
};

/**
 * struct ethosu_rpmsg_header - Message header
 */
struct ethosu_rpmsg_header {
	uint32_t magic;
	uint32_t type;
	uint64_t msg_id;
};

/**
 * enum ethosu_rpmsg_status - Status
 */
enum ethosu_rpmsg_status {
	ETHOSU_RPMSG_STATUS_OK,
	ETHOSU_RPMSG_STATUS_ERROR,
	ETHOSU_RPMSG_STATUS_RUNNING,
	ETHOSU_RPMSG_STATUS_REJECTED,
	ETHOSU_RPMSG_STATUS_ABORTED,
	ETHOSU_RPMSG_STATUS_ABORTING,
};

/**
 * struct ethosu_rpmsg_buffer - Buffer descriptor
 *
 * Pointer and size to a buffer within the Ethos-U address space.
 */
struct ethosu_rpmsg_buffer {
	uint32_t ptr;
	uint32_t size;
};

/**
 * enum ethosu_rpmsg_network_type - Network buffer type
 */
enum ethosu_rpmsg_network_type {
	ETHOSU_RPMSG_NETWORK_BUFFER = 1,
	ETHOSU_RPMSG_NETWORK_INDEX
};

/**
 * struct ethosu_rpmsg_network_buffer - Network buffer
 */
struct ethosu_rpmsg_network_buffer {
	uint32_t type;
	union {
		struct ethosu_rpmsg_buffer buffer;
		uint32_t                   index;
	};
};

/**
 * struct ethosu_rpmsg_inference_req - Inference request
 */
struct ethosu_rpmsg_inference_req {
	uint32_t                           ifm_count;
	struct ethosu_rpmsg_buffer         ifm[ETHOSU_RPMSG_BUFFER_MAX];
	uint32_t                           ofm_count;
	struct ethosu_rpmsg_buffer         ofm[ETHOSU_RPMSG_BUFFER_MAX];
	struct ethosu_rpmsg_network_buffer network;
	uint8_t                            pmu_event_config[ETHOSU_RPMSG_PMU_MAX
	];
	uint32_t                           pmu_cycle_counter_enable;
};

/**
 * struct ethosu_rpmsg_inference_rsp - Inference response
 */
struct ethosu_rpmsg_inference_rsp {
	uint32_t ofm_count;
	uint32_t ofm_size[ETHOSU_RPMSG_BUFFER_MAX];
	uint32_t status;
	uint8_t  pmu_event_config[ETHOSU_RPMSG_PMU_MAX];
	uint64_t pmu_event_count[ETHOSU_RPMSG_PMU_MAX];
	uint32_t pmu_cycle_counter_enable;
	uint64_t pmu_cycle_counter_count;
};

/**
 * struct ethosu_rpmsg_network_info_req - Network information request
 */
struct ethosu_rpmsg_network_info_req {
	struct ethosu_rpmsg_network_buffer network;
};

/**
 * struct ethosu_rpmsg_network_info_rsp - Network information response
 */
struct ethosu_rpmsg_network_info_rsp {
	char     desc[32];
	uint32_t ifm_count;
	uint32_t ifm_size[ETHOSU_RPMSG_BUFFER_MAX];
	uint32_t ofm_count;
	uint32_t ofm_size[ETHOSU_RPMSG_BUFFER_MAX];
	uint32_t status;
};

/**
 * struct ethosu_rpmsg_version_rsp - Message protocol version
 */
struct ethosu_rpmsg_version_rsp {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
	uint8_t _reserved;
};

/**
 * struct ethosu_rpmsg_capabilities_rsp - Message capabilities response
 */
struct ethosu_rpmsg_capabilities_rsp {
	uint32_t version_status;
	uint32_t version_minor;
	uint32_t version_major;
	uint32_t product_major;
	uint32_t arch_patch_rev;
	uint32_t arch_minor_rev;
	uint32_t arch_major_rev;
	uint32_t driver_patch_rev;
	uint32_t driver_minor_rev;
	uint32_t driver_major_rev;
	uint32_t macs_per_cc;
	uint32_t cmd_stream_version;
	uint32_t custom_dma;
};

/**
 * struct ethosu_rpmsg_cancel_inference_req - Message cancel inference
 * request
 */
struct ethosu_rpmsg_cancel_inference_req {
	uint64_t inference_handle;
};

/**
 * struct ethosu_rpmsg_cancel_inference_rsp - Message cancel inference
 * response
 */
struct ethosu_rpmsg_cancel_inference_rsp {
	uint32_t status;
};

/**
 * enum ethosu_rpmsg_err_type - Error types
 */
enum ethosu_rpmsg_err_type {
	ETHOSU_RPMSG_ERR_GENERIC = 0,
	ETHOSU_RPMSG_ERR_UNSUPPORTED_TYPE,
	ETHOSU_RPMSG_ERR_INVALID_PAYLOAD,
	ETHOSU_RPMSG_ERR_INVALID_SIZE,
	ETHOSU_RPMSG_ERR_INVALID_MAGIC,
	ETHOSU_RPMSG_ERR_MAX
};

/**
 * struct ethosu_rpmsg_err - Error message struct
 */
struct ethosu_rpmsg_err {
	uint32_t type;     /* optional use of extra error code */
	char     msg[128];
};

/**
 * struct ethosu_rpmsg - Rpmsg message
 */
struct ethosu_rpmsg {
	struct ethosu_rpmsg_header header;
	union {
		struct ethosu_rpmsg_inference_req        inf_req;
		struct ethosu_rpmsg_inference_rsp        inf_rsp;
		struct ethosu_rpmsg_network_info_req     net_info_req;
		struct ethosu_rpmsg_network_info_rsp     net_info_rsp;
		struct ethosu_rpmsg_capabilities_rsp     cap_rsp;
		struct ethosu_rpmsg_cancel_inference_req cancel_req;
		struct ethosu_rpmsg_cancel_inference_rsp cancel_rsp;
		struct ethosu_rpmsg_version_rsp          version_rsp;
		struct ethosu_rpmsg_err                  error;
	};
};

#ifdef __cplusplus
} /*namespace EthosU */
#endif

#endif /* ETHOSU_RPMSG_H */
