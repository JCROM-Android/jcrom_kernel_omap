/*
 * Remote processor resource manager
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/slab.h>
#include <linux/rpmsg.h>
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/err.h>
#include <linux/rpmsg_resmgr.h>
#include <plat/omap_device.h>
#include <plat/dmtimer.h>
#include <plat/rpres.h>

static DEFINE_IDR(rprm_list);
static DEFINE_SPINLOCK(rprm_lock);

struct rprm_elem {
	struct list_head next;
	u32 type;
	void *handle;
	char res[];
};

static int _get_rprm_size(u32 type)
{
	switch (type) {
	case RPRM_GPTIMER:
		return sizeof(struct rprm_gpt);
	}
	return 0;
}

static int rprm_gptimer_request(struct rprm_gpt *obj,
				 void **rh)
{
	int ret;
	struct omap_dm_timer *gpt;

	/* TODO: validate id */
	gpt = omap_dm_timer_request_specific(obj->id);
	if (!gpt)
		return -EBUSY;

	ret = omap_dm_timer_set_source(gpt, obj->src_clk);
	if (!ret)
		*rh = gpt;

	return ret;
}

static void rprm_gptimer_release(struct omap_dm_timer *obj)
{
	omap_dm_timer_free(obj);
}

static const char *_get_rpres_name(int type)
{
	switch (type) {
	case RPRM_IVAHD:
		return "rpres_iva";
	case RPRM_IVASEQ0:
		return "rpres_iva_seq0";
	case RPRM_IVASEQ1:
		return "rpres_iva_seq1";
	case RPRM_ISS:
		return "rpres_iss";
	case RPRM_FDIF:
		return "rpres_fdif";
	case RPRM_SL2IF:
		return "rpres_sl2if";
	}
	return NULL;
}

static int rprm_rpres_request(int type, void **rh)
{
	const char *res_name;
	struct rpres *res = NULL;

	res_name = _get_rpres_name(type);

	pr_debug("rpres resource\n"
			"type %x\n"
			"name %s\n",
			type, res_name);

	if (!res_name)
		return -EINVAL;

	res = rpres_get(res_name);

	if (IS_ERR(res)) {
		pr_err("fail to get the resource\n");
		return PTR_ERR(res);
	}
	*rh = res;

	return 0;
}

static void rprm_rpres_release(struct rpres *res)
{
	rpres_put(res);
}

static int rprm_l3_bus_request(void **handle)
{
	pr_debug("l3 bus  resource\n");

	return 0;
}

static void rprm_l3_bus_release(void *handle)
{
	/* Nothing */
}

static int rprm_resource_alloc(int *res_id, int type, void *data)
{
	void *handle = NULL;
	struct rprm_elem *e;
	int ret;

	e = kmalloc(sizeof(*e) + _get_rprm_size(type), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	switch (type) {
	case RPRM_GPTIMER:
		ret = rprm_gptimer_request(data, &handle);
		break;
	case RPRM_L3BUS:
		ret = rprm_l3_bus_request(&handle);
		break;
	case RPRM_IVAHD:
	case RPRM_IVASEQ0:
	case RPRM_IVASEQ1:
	case RPRM_ISS:
	case RPRM_SL2IF:
	case RPRM_FDIF:
		ret = rprm_rpres_request(type, &handle);
		break;
	default:
		ret = -ENOENT;
	}

	if (ret)
		goto out;

	e->handle = handle;
	e->type = type;
	if (!idr_pre_get(&rprm_list, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}
	spin_lock(&rprm_lock);
	ret = idr_get_new(&rprm_list, e, res_id);
	spin_unlock(&rprm_lock);
out:
	if (ret)
		kfree(e);
	return ret;
}

static int rprm_resource_free(int res_id)
{
	int ret = 0;
	struct rprm_elem *e;

	spin_lock(&rprm_lock);
	e = idr_find(&rprm_list, res_id);

	if (!e) {
		ret = -ENOENT;
		goto out;
	}

	switch (e->type) {
	case RPRM_GPTIMER:
		rprm_gptimer_release(e->handle);
		break;
	case RPRM_L3BUS:
		rprm_l3_bus_release(e->handle);
		break;
	case RPRM_IVAHD:
	case RPRM_IVASEQ0:
	case RPRM_IVASEQ1:
	case RPRM_ISS:
	case RPRM_SL2IF:
	case RPRM_FDIF:
		rprm_rpres_release(e->handle);
		break;
	default:
		ret = -EINVAL;
	}
	if (!ret)
		idr_remove(&rprm_list, res_id);
	spin_unlock(&rprm_lock);
out:
	return ret;
}

static int rprm_resource(int *res_id, int type, bool acq, int size, void *data)
{
	switch (acq) {
	case RPRM_REQ_ALLOC:
		if (size != _get_rprm_size(type))
			break;
		return rprm_resource_alloc(res_id, type, data);
	case RPRM_REQ_FREE:
		return rprm_resource_free(*res_id);
	}

	return -EINVAL;
}
static void rprm_cb(struct rpmsg_channel *rpdev, void *data, int len,
			void *priv, u32 src)
{
	int ret;
	struct device *dev = &rpdev->dev;
	struct rprm_head *obj = data;
	char ack_msg[512];
	struct rprm_head_ack *ack = (void *)ack_msg;
	int r_sz = len - sizeof(*obj);

	if (r_sz < 0) {
		dev_err(dev, "Bad message\n");
		return;
	}

	dev_dbg(dev, "resource type %d\n"
		"acquire %d\n"
		"res_id %d",
		obj->res_type, obj->acquire, obj->res_id);

	ret = rprm_resource(&obj->res_id, obj->res_type, obj->acquire,
				 r_sz, obj->data);
	if (ret)
		dev_err(dev, "fail requesting/releasing resource %d\n", ret);

	/* Dont ack free requests */
	if (obj->acquire == RPRM_REQ_FREE)
		return;

	/* remote processor is not interested in linux errors */
	ack->ret = ret ? -1 : 0;
	ack->res_type = obj->res_type;
	ack->res_id = obj->res_id;

	memcpy(ack->data, obj->data, r_sz);

	ret = rpmsg_sendto(rpdev, ack, sizeof(*ack) + r_sz, src);
	if (ret)
		dev_err(dev, "rpres ack failed: %d\n", ret);
}

static int rprm_probe(struct rpmsg_channel *rpdev)
{
}

static void __devexit rprm_remove(struct rpmsg_channel *rpdev)
{
	/* TODO: remove all resources */
}

static struct rpmsg_device_id rprm_id_table[] = {
	{
		.name	= "rpmsg-resmgr",
	},
	{ },
};
MODULE_DEVICE_TABLE(platform, rprm_id_table);

static struct rpmsg_driver rprm_driver = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rprm_id_table,
	.probe		= rprm_probe,
	.callback	= rprm_cb,
	.remove		= __devexit_p(rprm_remove),
};

static int __init init(void)
{
	return register_rpmsg_driver(&rprm_driver);
}

static void __exit fini(void)
{
	unregister_rpmsg_driver(&rprm_driver);
}
module_init(init);
module_exit(fini);

MODULE_DESCRIPTION("Remote Processor Resource Manager");
MODULE_LICENSE("GPL v2");
