/*
 * Remote Processor Framework
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Mark Grosen <mgrosen@ti.com>
 * Brian Swetland <swetland@google.com>
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 * Robert Tivy <rtivy@ti.com>
 * Armando Uribe De Leon <x0095078@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/remoteproc.h>
#include <linux/pm_runtime.h>

/* list of available remote processors on this board */
static LIST_HEAD(rprocs);
static DEFINE_SPINLOCK(rprocs_lock);

/* debugfs parent dir */
static struct dentry *rproc_dbg;

static ssize_t rproc_format_trace_buf(char __user *userbuf, size_t count,
				    loff_t *ppos, const void *src, int size)
{
	const char *buf = (const char *) src;
	int i;

	/* find the end of trace buffer - does not account for wrapping */
	for (i = 0; i < size && buf[i]; i++);

	return simple_read_from_buffer(userbuf, count, ppos, src, i);
}

static ssize_t rproc_name_read(struct file *filp, char __user *userbuf,
						size_t count, loff_t *ppos)
{
	struct rproc *rproc = filp->private_data;
	/* need room for the name, a newline and a terminating null */
	char buf[RPROC_MAX_NAME + 2];
	int i;

	i = snprintf(buf, RPROC_MAX_NAME + 2, "%s\n", rproc->name);

	return simple_read_from_buffer(userbuf, count, ppos, buf, i);
}

static int rproc_open_generic(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

#define DEBUGFS_READONLY_FILE(name, value, len)				\
static ssize_t name## _rproc_read(struct file *filp,			\
		char __user *userbuf, size_t count, loff_t *ppos)	\
{									\
	struct rproc *rproc = filp->private_data;			\
	return rproc_format_trace_buf(userbuf, count, ppos, value, len);\
}									\
									\
static const struct file_operations name ##_rproc_ops = {		\
	.read = name ##_rproc_read,					\
	.open = rproc_open_generic,					\
	.llseek	= generic_file_llseek,					\
};

static const struct file_operations rproc_name_ops = {
	.read = rproc_name_read,
	.open = rproc_open_generic,
	.llseek	= generic_file_llseek,
};

DEBUGFS_READONLY_FILE(trace0, rproc->trace_buf0, rproc->trace_len0);
DEBUGFS_READONLY_FILE(trace1, rproc->trace_buf1, rproc->trace_len1);

#define DEBUGFS_ADD(name)						\
	debugfs_create_file(#name, 0400, rproc->dbg_dir,		\
			rproc, &name## _rproc_ops)

/**
 * __find_rproc_by_name - find a registered remote processor by name
 * @name: name of the remote processor
 *
 * Internal function that returns the rproc @name, or NULL if @name does
 * not exists.
 */
static struct rproc *__find_rproc_by_name(const char *name)
{
	struct rproc *rproc;
	struct list_head *tmp;

	spin_lock(&rprocs_lock);

	list_for_each(tmp, &rprocs) {
		rproc = list_entry(tmp, struct rproc, next);
		if (!strcmp(rproc->name, name))
			break;
		rproc = NULL;
	}

	spin_unlock(&rprocs_lock);

	return rproc;
}

/**
 * __rproc_da_to_pa - convert a device (virtual) address to its physical address
 * @maps: the remote processor's memory mappings array
 * @da: a device address (as seen by the remote processor)
 * @pa: pointer to the physical address result
 *
 * This function converts @da to its physical address (pa) by going through
 * @maps, looking for a mapping that contains @da, and then calculating the
 * appropriate pa.
 *
 * On success 0 is returned, and the @pa is updated with the result.
 * Otherwise, -EINVAL is returned.
 */
static int
rproc_da_to_pa(const struct rproc_mem_entry *maps, u64 da, phys_addr_t *pa)
{
	int i;
	u64 offset;

	for (i = 0; maps[i].size; i++) {
		const struct rproc_mem_entry *me = &maps[i];

		if (da >= me->da && da < (me->da + me->size)) {
			offset = da - me->da;
			pr_debug("%s: matched mem entry no. %d\n", __func__, i);
			*pa = me->pa + offset;
			return 0;
		}
	}

	return -EINVAL;
}

static int rproc_mmu_fault_isr(struct rproc *rproc, u64 da, u32 flags)
{
	dev_err(rproc->dev, "Enter %s\n", __func__);
	rproc->state = RPROC_CRASHED;
	schedule_work(&rproc->mmufault_work);
#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	pm_runtime_dont_use_autosuspend(rproc->dev);
#endif

	return -EFAULT;
}

static int _event_notify(struct rproc *rproc, int type, void *data)
{
	struct blocking_notifier_head *nh;

	switch (type) {
	case RPROC_ERROR:
		nh = &rproc->nb_error;
		rproc->state = RPROC_CRASHED;
		break;
#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	case RPROC_PRE_SUSPEND:
		nh = &rproc->nb_presus;
		break;
	case RPROC_POS_SUSPEND:
		nh = &rproc->nb_possus;
		break;
	case RPROC_RESUME:
		nh = &rproc->nb_resume;
		break;
#endif
	default:
		return -EINVAL;
	}

	return blocking_notifier_call_chain(nh, type, data);
}

/**
 * rproc_start - power on the remote processor and let it start running
 * @rproc: the remote processor
 * @bootaddr: address of first instruction to execute (optional)
 *
 * Start a remote processor (i.e. power it on, take it out of reset, etc..)
 */
static void rproc_start(struct rproc *rproc, u64 bootaddr)
{
	struct device *dev = rproc->dev;
	int err;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %d\n", err);
		return;
	}

	if (rproc->ops->iommu_init) {
		err = rproc->ops->iommu_init(rproc, rproc_mmu_fault_isr);
		if (err) {
			dev_err(dev, "can't configure iommu %d\n", err);
			goto unlock_mutext;
		}
	}

	err = rproc->ops->start(rproc, bootaddr);
	if (err) {
		dev_err(dev, "can't start rproc %s: %d\n", rproc->name, err);
		goto unlock_mutext;
	}

#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	pm_runtime_set_active(rproc->dev);
	pm_runtime_enable(rproc->dev);
	pm_runtime_get_noresume(rproc->dev);
	pm_runtime_mark_last_busy(rproc->dev);
	pm_runtime_put_autosuspend(rproc->dev);
#endif

	rproc->state = RPROC_RUNNING;

	dev_info(dev, "remote processor %s is now up\n", rproc->name);

unlock_mutext:
	mutex_unlock(&rproc->lock);
}

static int rproc_handle_resources(struct rproc *rproc, struct fw_resource *rsc,
							int len, u64 *bootaddr)
{
	struct device *dev = rproc->dev;
	phys_addr_t pa;
	u64 da;
	void *ptr;
	int ret;

	while (len >= sizeof(*rsc)) {
		da = rsc->da;

		ret = rproc_da_to_pa(rproc->memory_maps, da, &pa);
		if (ret) {
			dev_err(dev, "invalid device address\n");
			return -EINVAL;
		}

		dev_dbg(dev, "resource: type %d, da 0x%llx, pa 0x%x, len 0x%x"
			", reserved %d, name %s\n", rsc->type, rsc->da, pa,
			rsc->len, rsc->reserved, rsc->name);

		if (rsc->reserved)
			dev_warn(dev, "nonzero reserved\n");

		switch (rsc->type) {
		case RSC_TRACE:
			if (rproc->trace_buf0 && rproc->trace_buf1) {
				dev_warn(dev, "skipping extra trace rsc %s\n",
						rsc->name);
				break;
			}

			/*
			 * trace buffer memory _is_ normal memory, so we cast
			 * away the __iomem to make sparse happy
			 */
			ptr = (__force void *) ioremap_nocache(pa, rsc->len);
			if (!ptr) {
				dev_err(dev, "can't ioremap trace buffer %s\n",
								rsc->name);
				break;
			}

			if (!rproc->trace_buf0) {
				rproc->trace_len0 = rsc->len;
				rproc->trace_buf0 = ptr;
				DEBUGFS_ADD(trace0);
			} else {
				rproc->trace_len1 = rsc->len;
				rproc->trace_buf1 = ptr;
				DEBUGFS_ADD(trace1);
			}
			break;
		case RSC_BOOTADDR:
			*bootaddr = da;
			break;
		default:
			/* we don't support much right now. so use dbg lvl */
			dev_dbg(dev, "unsupported resource type %d\n",
								rsc->type);
			break;
		}

		rsc++;
		len -= sizeof(*rsc);
	}

	return 0;
}

static int rproc_process_fw(struct rproc *rproc, struct fw_section *section,
						int left, u64 *bootaddr)
{
	struct device *dev = rproc->dev;
	phys_addr_t pa;
	u32 len, type;
	u64 da;
	int ret = 0;
	void *ptr;

	while (left > sizeof(struct fw_section)) {
		da = section->da;
		len = section->len;
		type = section->type;

		dev_dbg(dev, "section: type %d da 0x%llx len 0x%x\n",
								type, da, len);

		left -= sizeof(struct fw_section);
		if (left < section->len) {
			dev_err(dev, "BIOS image is truncated\n");
			ret = -EINVAL;
			break;
		}

		ret = rproc_da_to_pa(rproc->memory_maps, da, &pa);
		if (ret) {
			dev_err(dev, "rproc_da_to_pa failed: %d\n", ret);
			break;
		}

		dev_dbg(dev, "da 0x%llx pa 0x%x len 0x%x\n", da, pa, len);

		/* ioremaping normal memory, so make sparse happy */
		ptr = (__force void *) ioremap_nocache(pa, len);
		if (!ptr) {
			dev_err(dev, "can't ioremap 0x%x\n", pa);
			ret = -ENOMEM;
			break;
		}

		memcpy(ptr, section->content, len);

		/* a resource table needs special handling */
		if (section->type == FW_RESOURCE) {
			ret = rproc_handle_resources(rproc,
						(struct fw_resource *) ptr,
						len, bootaddr);
			if (ret) {
				/* iounmap normal mem, so make sparse happy */
				iounmap((__force void __iomem *) ptr);
				break;
			}
		}

		/* iounmap normal memory, so make sparse happy */
		iounmap((__force void __iomem *) ptr);

		section = (struct fw_section *)(section->content + len);
		left -= len;
	}

	return ret;
}

static void rproc_loader_cont(const struct firmware *fw, void *context)
{
	struct rproc *rproc = context;
	struct device *dev = rproc->dev;
	const char *fwfile = rproc->firmware;
	u64 bootaddr = 0;
	struct fw_header *image;
	struct fw_section *section;
	int left, ret;

	if (!fw) {
		dev_err(dev, "%s: failed to load %s\n", __func__, fwfile);
		goto complete_fw;
	}

	dev_info(dev, "Loaded BIOS image %s, size %d\n", fwfile, fw->size);

	/* make sure this image is sane */
	if (fw->size < sizeof(struct fw_header)) {
		dev_err(dev, "Image is too small\n");
		goto out;
	}

	image = (struct fw_header *) fw->data;

	if (memcmp(image->magic, "RPRC", 4)) {
		dev_err(dev, "Image is corrupted (bad magic)\n");
		goto out;
	}

	dev_info(dev, "BIOS image version is %d\n", image->version);

	/* now process the image, section by section */
	section = (struct fw_section *)(image->header + image->header_len);

	left = fw->size - sizeof(struct fw_header) - image->header_len;

	ret = rproc_process_fw(rproc, section, left, &bootaddr);
	if (ret) {
		dev_err(dev, "Failed to process the image: %d\n", ret);
		goto out;
	}

	rproc_start(rproc, bootaddr);

out:
	release_firmware(fw);
complete_fw:
	/* allow all contexts calling rproc_put() to proceed */
	complete_all(&rproc->firmware_loading_complete);
}

static int rproc_loader(struct rproc *rproc)
{
	const char *fwfile = rproc->firmware;
	struct device *dev = rproc->dev;
	int ret;

	if (!fwfile) {
		dev_err(dev, "%s: no firmware to load\n", __func__);
		return -EINVAL;
	}

	/*
	 * allow building remoteproc as built-in kernel code, without
	 * hanging the boot process
	 */
	ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, fwfile,
			dev, GFP_KERNEL, rproc, rproc_loader_cont);
	if (ret < 0) {
		dev_err(dev, "request_firmware_nowait failed: %d\n", ret);
		return ret;
	}

	return 0;
}

struct rproc *rproc_get(const char *name)
{
	struct rproc *rproc, *ret = NULL;
	struct device *dev;
	int err;

	rproc = __find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return NULL;
	}

	dev = rproc->dev;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %s\n", name);
		return NULL;
	}

	/* prevent underlying implementation from being removed */
	if (!try_module_get(rproc->owner)) {
		dev_err(dev, "%s: can't get owner\n", __func__);
		goto unlock_mutex;
	}

	/* bail if rproc is already powered up */
	if (rproc->count++) {
		ret = rproc;
		goto unlock_mutex;
	}

	/* rproc_put() calls should wait until async loader completes */
	init_completion(&rproc->firmware_loading_complete);

	dev_info(dev, "powering up %s\n", name);

	err = rproc_loader(rproc);
	if (err) {
		dev_err(dev, "failed to load rproc %s\n", rproc->name);
		complete_all(&rproc->firmware_loading_complete);
		module_put(rproc->owner);
		--rproc->count;
		goto unlock_mutex;
	}

	rproc->state = RPROC_LOADING;
	ret = rproc;

unlock_mutex:
	mutex_unlock(&rproc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_get);

void rproc_put(struct rproc *rproc)
{
	struct device *dev = rproc->dev;
	int ret;

	/* make sure rproc is not loading now */
	wait_for_completion(&rproc->firmware_loading_complete);

	ret = mutex_lock_interruptible(&rproc->lock);
	if (ret) {
		dev_err(dev, "can't lock rproc %s: %d\n", rproc->name, ret);
		return;
	}

	if (!rproc->count) {
		dev_warn(dev, "asymmetric rproc_put\n");
		ret = -EINVAL;
		goto out;
	}

	/* if the remote proc is still needed, bail out */
	if (--rproc->count)
		goto out;

	if (rproc->trace_buf0)
		/* iounmap normal memory, so make sparse happy */
		iounmap((__force void __iomem *) rproc->trace_buf0);
	if (rproc->trace_buf1)
		/* iounmap normal memory, so make sparse happy */
		iounmap((__force void __iomem *) rproc->trace_buf1);

	rproc->trace_buf0 = rproc->trace_buf1 = NULL;

	/*
	 * make sure rproc is really running before powering it off.
	 * this is important, because the fw loading might have failed.
	 */
	if (rproc->state == RPROC_RUNNING || rproc->state == RPROC_CRASHED) {
#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
		/*
		 * Call resume, it will cancel any pending autosuspend,
		 * so that no callback is executed after the device is stopped.
		 * Device stop function takes care of shuting down the device.
		 */
		pm_runtime_get_sync(rproc->dev);
		pm_runtime_disable(rproc->dev);
		pm_runtime_set_suspended(rproc->dev);
#endif
		ret = rproc->ops->stop(rproc);
		if (ret) {
			dev_err(dev, "can't stop rproc %s: %d\n", rproc->name,
									ret);
			goto out;
		}
		if (rproc->ops->iommu_exit) {
			ret = rproc->ops->iommu_exit(rproc);
			if (ret) {
				dev_err(rproc->dev, "error iommu_exit %d\n",
					ret);
				goto out;
			}
		}

	}

	rproc->state = RPROC_OFFLINE;

	dev_info(dev, "stopped remote processor %s\n", rproc->name);

out:
	mutex_unlock(&rproc->lock);
	if (!ret)
		module_put(rproc->owner);
}
EXPORT_SYMBOL_GPL(rproc_put);

static void rproc_mmufault_work(struct work_struct *work)
{
	struct rproc *rproc = container_of(work, struct rproc, mmufault_work);

	dev_dbg(rproc->dev, "Enter %s\n", __func__);
	_event_notify(rproc, RPROC_ERROR, NULL);
}

static int _register(struct rproc *rproc,
			struct notifier_block *nb, int type, bool reg)
{
	struct blocking_notifier_head *nh;

	switch (type) {
	case RPROC_ERROR:
		nh = &rproc->nb_error;
		break;
#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	case RPROC_PRE_SUSPEND:
		nh = &rproc->nb_presus;
		break;
	case RPROC_POS_SUSPEND:
		nh = &rproc->nb_possus;
		break;
	case RPROC_RESUME:
		nh = &rproc->nb_resume;
		break;
#endif
	default:
		return -EINVAL;
	}

	return (reg) ? blocking_notifier_chain_register(nh, nb) :
		blocking_notifier_chain_unregister(nh, nb);
}

int rproc_event_register(struct rproc *rproc,
				struct notifier_block *nb, int type)
{
	return _register(rproc, nb, type, true);
}
EXPORT_SYMBOL_GPL(rproc_event_register);

int rproc_event_unregister(struct rproc *rproc,
				struct notifier_block *nb, int type)
{
	return _register(rproc, nb, type, false);
}
EXPORT_SYMBOL_GPL(rproc_event_unregister);

void rproc_last_busy(struct rproc *rproc)
{
#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	struct device *dev = rproc->dev;

	if (!pm_runtime_autosuspend_expiration(dev) || rproc->suspended) {
		rproc->suspended = false;
		pm_runtime_get_sync(dev);
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
	}
	pm_runtime_mark_last_busy(dev);
#endif
}
EXPORT_SYMBOL(rproc_last_busy);

#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
static int rproc_resume(struct rproc *rproc)
{
	dev_err(rproc->dev, "Enter %s\n", __func__);
	_event_notify(rproc, RPROC_RESUME, NULL);
	return 0;
}

static int rproc_suspend(struct rproc *rproc)
{
	int ret;
	unsigned to;
	struct device *dev = rproc->dev;

	dev_err(dev, "Enter %s\n", __func__);

	rproc->suspended = true;

	ret = _event_notify(rproc, RPROC_PRE_SUSPEND, NULL);
	/*
	 * if rproc user avoid suspend that means it is still using rproc
	 * it is ok lets go to abort suspend.
	 */
	if (ret) {
		dev_err(dev, "rproc user avoid suspend %d\n", ret);
		ret = -EBUSY;
		goto abort;
	}

	/*
	 * Lets try to suspend remoteproc, if it is still doing something
	 * (no idle) if should returned failed.
	 */
	if (rproc->ops->suspend)
		ret = rproc->ops->suspend(rproc, false);
	/*
	 * if fails remote processor still in used, however rproc users were
	 * not awared of that, it is not an issue either, lets abort.
	 */
	if (ret) {
		dev_err(dev, "remote processor busy avoid suspend %d\n", ret);
		goto abort;
	}

	/* we are note interested in the user return value */
	_event_notify(rproc, RPROC_POS_SUSPEND, NULL);

	return 0;
abort:
	rproc->suspended = false;
	pm_runtime_mark_last_busy(dev);
	to = jiffies_to_msecs(pm_runtime_autosuspend_expiration(dev) - jiffies);
	pm_schedule_suspend(dev, to);
	return ret;
}

static int rproc_idle(struct rproc *rproc)
{
	dev_dbg(rproc->dev, "Enter %s\n", __func__);
	return 0;
}
#endif

int rproc_register(struct device *dev, const char *name,
				const struct rproc_ops *ops,
				const char *firmware,
				const struct rproc_mem_entry *memory_maps,
				struct module *owner,
				unsigned sus_timeout)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rproc *rproc;

	if (!dev || !name || !ops)
		return -EINVAL;

	rproc = kzalloc(sizeof(struct rproc), GFP_KERNEL);
	if (!rproc) {
		dev_err(dev, "%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	rproc->dev = dev;
	rproc->name = name;
	rproc->ops = ops;
	rproc->firmware = firmware;
	rproc->owner = owner;
	rproc->memory_maps = memory_maps;
#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	rproc->sus_timeout = sus_timeout;
	rproc->resume = rproc_resume;
	rproc->suspend = rproc_suspend;
	rproc->idle = rproc_idle;
#endif

	mutex_init(&rproc->lock);
	INIT_WORK(&rproc->mmufault_work, rproc_mmufault_work);
	BLOCKING_INIT_NOTIFIER_HEAD(&rproc->nb_error);

	rproc->state = RPROC_OFFLINE;

	spin_lock(&rprocs_lock);
	list_add_tail(&rproc->next, &rprocs);
	spin_unlock(&rprocs_lock);

	platform_set_drvdata(pdev, rproc);

	dev_info(dev, "%s is available\n", name);

	if (!rproc_dbg)
		goto out;

	rproc->dbg_dir = debugfs_create_dir(dev_name(dev), rproc_dbg);
	if (!rproc->dbg_dir) {
		dev_err(dev, "can't create debugfs dir\n");
		goto out;
	}

	debugfs_create_file("name", 0400, rproc->dbg_dir, rproc,
							&rproc_name_ops);

#ifdef CONFIG_OMAP_REMOTE_PROC_AUTOSUSPEND
	BLOCKING_INIT_NOTIFIER_HEAD(&rproc->nb_presus);
	BLOCKING_INIT_NOTIFIER_HEAD(&rproc->nb_possus);
	BLOCKING_INIT_NOTIFIER_HEAD(&rproc->nb_resume);
	pm_runtime_allow(dev);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, rproc->sus_timeout);
#endif

out:
	return 0;
}
EXPORT_SYMBOL_GPL(rproc_register);

int rproc_unregister(const char *name)
{
	struct rproc *rproc;

	rproc = __find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return -EINVAL;
	}

	dev_info(rproc->dev, "removing %s\n", name);

	if (rproc->dbg_dir)
		debugfs_remove_recursive(rproc->dbg_dir);

	spin_lock(&rprocs_lock);
	list_del(&rproc->next);
	spin_unlock(&rprocs_lock);

	kfree(rproc);

	return 0;
}
EXPORT_SYMBOL_GPL(rproc_unregister);

static int __init remoteproc_init(void)
{
	if (debugfs_initialized()) {
		rproc_dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
		if (!rproc_dbg)
			pr_err("can't create debugfs dir\n");
	}

	return 0;
}
/* must be ready in time for device_initcall users */
subsys_initcall(remoteproc_init);

static void __exit remoteproc_exit(void)
{
	if (rproc_dbg)
		debugfs_remove(rproc_dbg);
}
module_exit(remoteproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Generic Remote Processor Framework");
