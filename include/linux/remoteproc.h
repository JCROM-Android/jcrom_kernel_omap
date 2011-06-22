/*
 * Remote Processor Framework
 *
 * Copyright(c) 2011 Texas Instruments. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name Texas Instruments nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef REMOTEPROC_H
#define REMOTEPROC_H

#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>

/**
 * The following enums and structures define the binary format of the images
 * we load and run the remote processors with.
 *
 * The binary format is as follows:
 *
 * struct {
 *     char magic[4] = { 'R', 'P', 'R', 'C' };
 *     u32 version;
 *     u32 header_len;
 *     char header[...] = { header_len bytes of unformatted, textual header };
 *     struct section {
 *         u32 type;
 *         u64 da;
 *         u32 len;
 *         u8 content[...] = { len bytes of binary data };
 *     } [ no limit on number of sections ];
 * } __packed;
 */
struct fw_header {
	char magic[4];
	u32 version;
	u32 header_len;
	char header[0];
} __packed;

struct fw_section {
	u32 type;
	u64 da;
	u32 len;
	char content[0];
} __packed;

enum fw_section_type {
	FW_RESOURCE	= 0,
	FW_TEXT		= 1,
	FW_DATA		= 2,
};

struct fw_resource {
	u32 type;
	u64 da;
	u32 len;
	u32 reserved;
	u8 name[48];
} __packed;

enum fw_resource_type {
	RSC_MEMORY	= 0,
	RSC_DEVICE	= 1,
	RSC_IRQ		= 2,
	RSC_SERVICE	= 3,
	RSC_TRACE	= 4,
	RSC_BOOTADDR	= 5,
	RSC_END		= 6,
};

/**
 * struct rproc_mem_entry - descriptor of a remote memory region
 *
 * @da:		virtual address as seen by the device (aka device address)
 * @pa:		physical address
 * @size:	size of this memory region
 */
struct rproc_mem_entry {
	u64 da;
	phys_addr_t pa;
	u32 size;
};

struct rproc;

struct rproc_ops {
	int (*start)(struct rproc *rproc, u64 bootaddr);
	int (*stop)(struct rproc *rproc);
	int (*iommu_init)(struct rproc *, int (*)(struct rproc *, u64, u32));
	int (*iommu_exit)(struct rproc *);
};

/*
 * enum rproc_state - remote processor states
 *
 * @RPROC_OFFLINE: needs firmware load and init to exit this state.
 *
 * @RPROC_SUSPENDED: needs to be woken up to receive a message.
 *
 * @RPROC_RUNNING: up and running.
 *
 * @RPROC_LOADING: asynchronous firmware loading has started
 *
 * @RPROC_CRASHED: needs to be logged, connections torn down, resources
 * released, and returned to OFFLINE.
 */
enum rproc_state {
	RPROC_OFFLINE,
	RPROC_SUSPENDED,
	RPROC_RUNNING,
	RPROC_LOADING,
	RPROC_CRASHED,
};

/*
 * enum rproc_event - remote processor events
 *
 * @RPROC_ERROR: Fatal error has happened on the remote processor.
 */
enum rproc_event {
	RPROC_ERROR,
};

#define RPROC_MAX_NAME	100

/*
 * struct rproc - a physical remote processor device
 *
 * @next: next rproc entry in the list
 * @name: human readable name of the rproc, cannot exceed RPROC_MAN_NAME bytes
 * @memory_maps: table of da-to-pa memory maps (relevant if device is behind
 *               an iommu)
 * @firmware: name of firmware file to be loaded
 * @owner: reference to the platform-specific rproc module
 * @priv: private data which belongs to the platform-specific rproc module
 * @ops: platform-specific start/stop rproc handlers
 * @dev: reference to the platform-specific rproc dev
 * @count: usage refcount
 * @state: rproc_state enum value representing the state of the device
 * @lock: lock which protects concurrent manipulations of the rproc
 * @dbg_dir: debugfs directory of this rproc device
 * @trace_buf0: main trace buffer of the remote processor
 * @trace_buf1: second, optional, trace buffer of the remote processor
 * @trace_len0: length of main trace buffer of the remote processor
 * @trace_len1: length of the second (and optional) trace buffer
 * @firmware_loading_complete: flags e/o asynchronous firmware loading
 * @mmufault_work: work in charge of notifing mmufault
 * @nb_error: notify block for fatal errors
 */
struct rproc {
	struct list_head next;
	const char *name;
	const struct rproc_mem_entry *memory_maps;
	const char *firmware;
	struct module *owner;
	void *priv;
	const struct rproc_ops *ops;
	struct device *dev;
	int count;
	int state;
	struct mutex lock;
	struct dentry *dbg_dir;
	char *trace_buf0, *trace_buf1;
	int trace_len0, trace_len1;
	struct completion firmware_loading_complete;
	struct work_struct mmufault_work;
	struct blocking_notifier_head nb_error;
};

struct rproc *rproc_get(const char *);
void rproc_put(struct rproc *);
int rproc_event_register(struct rproc *, struct notifier_block *, int);
int rproc_event_unregister(struct rproc *, struct notifier_block *, int);
int rproc_register(struct device *, const char *, const struct rproc_ops *,
		const char *, const struct rproc_mem_entry *, struct module *);
int rproc_unregister(const char *);

#endif /* REMOTEPROC_H */
