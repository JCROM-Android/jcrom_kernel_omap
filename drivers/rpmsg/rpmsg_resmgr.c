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

static DEFINE_IDR(rpres_list);
static DEFINE_SPINLOCK(rpres_lock);

struct rpres_elem {
	struct list_head next;
	u32 type;
	void *handle;
	char res[];
};

static int _get_rpres_size(u32 type)
{
	switch (type) {
	case RPRES_GPTIMER:
		return sizeof(struct rpres_gpt);
	case RPRES_IVAHD:
	case RPRES_IVASEQ0:
	case RPRES_IVASEQ1:
		return sizeof(struct rpres_iva);
	case RPRES_L3BUS:
		return sizeof(struct rpres_l3_bus);
	case RPRES_ISS:
		return sizeof(struct rpres_iss);
	};
	return -ENOENT;
}

static int rpres_gptimer_request(struct rpres_gpt *obj,
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

static void rpres_gptimer_release(struct omap_dm_timer *obj)
{
	omap_dm_timer_free(obj);
}

static int rpres_iva_request(int type, struct rpres_iva *obj,
				void **rh)
{
	struct rpres *res = NULL;

	pr_debug("iva  resource\n"
			"sub type %x\n"
			"perf %x\n"
			"lat %d\n",
			type, obj->perf, obj->lat);

	switch (type) {
	case RPRES_IVAHD:
		res = rpres_get("rpres_iva");
		break;
	case RPRES_IVASEQ0:
		res = rpres_get("rpres_iva_seq0");
		break;
	case RPRES_IVASEQ1:
		res = rpres_get("rpres_iva_seq1");
	}

	if (IS_ERR(res)) {
		pr_err("fail to get the resource\n");
		return PTR_ERR(res);
	}
	*rh = res;

	return 0;
}

static void rpres_iva_release(struct rpres *res)
{
	rpres_put(res);
}

static int rpres_l3_bus_request(struct rpres_l3_bus *obj, void **handle)
{
	pr_debug("l3 bus  resource\n"
			"bw %x\n"
			"lat %d\n",
			obj->bw, obj->lat);
	return 0;
}

static void rpres_l3_bus_release(void *handle)
{
	/* Nothing */
}

static int rpres_iss_request(struct rpres_iss *obj, void **rh)
{
	struct rpres *res;

	pr_debug("iss  resource\n"
			"perf %x\n"
			"lat %d\n",
			obj->perf, obj->lat);

	res = rpres_get("rpres_iss");
	if (IS_ERR(res))
		return PTR_ERR(res);

	*rh = res;

	return 0;
}

static void rpres_iss_release(struct rpres *res)
{
	rpres_put(res);
}

static int rpres_resource_alloc(int *res_id, int type, void *data)
{
	void *handle = NULL;
	struct rpres_elem *e;
	int ret;

	e = kmalloc(sizeof(*e) + _get_rpres_size(type), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	switch (type) {
	case RPRES_GPTIMER:
		ret = rpres_gptimer_request(data, &handle);
		break;
	case RPRES_IVAHD:
	case RPRES_IVASEQ0:
	case RPRES_IVASEQ1:
		ret = rpres_iva_request(type, data, &handle);
		break;
	case RPRES_L3BUS:
		ret = rpres_l3_bus_request(data, &handle);
		break;
	case RPRES_ISS:
		ret = rpres_iss_request(data, &handle);
		break;
	default:
		ret = -ENOENT;
	}

	if (ret)
		goto out;

	e->handle = handle;
	e->type = type;
	if (!idr_pre_get(&rpres_list, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}
	spin_lock(&rpres_lock);
	ret = idr_get_new(&rpres_list, e, res_id);
	spin_unlock(&rpres_lock);
out:
	if (ret)
		kfree(e);
	return ret;
}

static int rpres_resource_free(int res_id)
{
	int ret = 0;
	struct rpres_elem *e;

	spin_lock(&rpres_lock);
	e = idr_find(&rpres_list, res_id);

	if (!e) {
		ret = -ENOENT;
		goto out;
	}

	switch (e->type) {
	case RPRES_GPTIMER:
		rpres_gptimer_release(e->handle);
		break;
	case RPRES_IVAHD:
	case RPRES_IVASEQ0:
	case RPRES_IVASEQ1:
		rpres_iva_release(e->handle);
		break;
	case RPRES_L3BUS:
		rpres_l3_bus_release(e->handle);
		break;
	case RPRES_ISS:
		rpres_iss_release(e->handle);
		break;
	default:
		ret = -EINVAL;
	}
	if (!ret)
		idr_remove(&rpres_list, res_id);
	spin_unlock(&rpres_lock);
out:
	return ret;
}

static int rpres_resource(int *res_id, int type, bool acq, int size, void *data)
{
	switch (acq) {
	case RPRES_REQ_ALLOC:
		if (size != _get_rpres_size(type))
			break;
		return rpres_resource_alloc(res_id, type, data);
	case RPRES_REQ_FREE:
		return rpres_resource_free(*res_id);
	}

	return -EINVAL;
}
static void rpresources_cb(struct rpmsg_channel *rpdev, void *data, int len,
			void *priv, u32 src)
{
	int ret;
	struct device *dev = &rpdev->dev;
	struct rpres_head *obj = data;
	char ack_msg[512];
	struct rpres_head_ack *ack = (void *)ack_msg;
	int r_sz = len - sizeof(*obj);

	if (r_sz < 0) {
		dev_err(dev, "Bad message\n");
		return;
	}

	dev_debug("proc %d\n"
		"resource type %d\n"
		"request %d\n"
		"private  %d\n"
		"data size %d\n",
		dev, obj->proc, obj->res_type, obj->acquire, obj->priv,
		obj->data_sz);

	ret = rpres_resource(&obj->res_id, obj->res_type, obj->acquire,
				 r_sz, obj->data);
	if (ret)
		dev_err(dev, "fail requesting/releasing resource %d\n", ret);

	/* Dont ack free requests */
	if (obj->acquire == RPRES_REQ_FREE)
		return;

	/* remote processor is not interested in linux errors */
	ack->ret = ret ? -1 : 0;
	ack->res_type = obj->res_type;
	ack->res_id = obj->res_id;
	ack->priv = obj->priv;
	ack->data_sz = obj->data_sz;

	memcpy(ack->data, obj->data, obj->data_sz);

	ret = rpmsg_sendto(rpdev, ack, sizeof(*ack) + ack->data_sz, src);
	if (ret)
		dev_err(dev, "rpres ack failed: %d\n", ret);
}

static int rpresources_probe(struct rpmsg_channel *rpdev)
{
	return 0;
}

static void __devexit rpresources_remove(struct rpmsg_channel *rpdev)
{
	/* TODO: remove all resources */
}

static struct rpmsg_device_id rpresources_id_table[] = {
	{
		.name	= "rpmsg-resmgr",
	},
	{ },
};
MODULE_DEVICE_TABLE(platform, rpresources_id_table);

static struct rpmsg_driver rpresources_driver = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rpresources_id_table,
	.probe		= rpresources_probe,
	.callback	= rpresources_cb,
	.remove		= __devexit_p(rpresources_remove),
};

static int __init init(void)
{
	return register_rpmsg_driver(&rpresources_driver);
}

static void __exit fini(void)
{
	unregister_rpmsg_driver(&rpresources_driver);
}
module_init(init);
module_exit(fini);

MODULE_DESCRIPTION("Remote processor resource manager");
MODULE_LICENSE("GPL v2");
