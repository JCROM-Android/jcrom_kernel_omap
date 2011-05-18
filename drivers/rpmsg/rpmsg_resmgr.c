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

struct rprm_elem {
	struct list_head next;
	u32 type;
	void *handle;
	char res[];
};

struct rprm {
	struct idr list;
	spinlock_t lock;
};

static int _get_rprm_size(u32 type)
{
	switch (type) {
	case RPRM_GPTIMER:
		return sizeof(struct rprm_gpt);
	}
	return 0;
}

static int rprm_gptimer_request(void **rh, struct rprm_gpt *obj)
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

static int rprm_rpres_request(void **rh, int type)
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

static int _resource_alloc(void **handle, int type, void *data)
{
	int ret;

	switch (type) {
	case RPRM_GPTIMER:
		ret = rprm_gptimer_request(handle, data);
		break;
	case RPRM_L3BUS:
		ret = rprm_l3_bus_request(handle);
		break;
	case RPRM_IVAHD:
	case RPRM_IVASEQ0:
	case RPRM_IVASEQ1:
	case RPRM_ISS:
	case RPRM_SL2IF:
	case RPRM_FDIF:
		ret = rprm_rpres_request(handle, type);
		break;
	default:
		ret = -ENOENT;
	}

	return ret;
}

static int rprm_resource_alloc(struct rprm *rprm, u32 addr, int *res_id,
				int type, void *data)
{
	void *handle = NULL;
	struct rprm_elem *e;
	int ret;
	struct idr *ridr;

	e = kmalloc(sizeof(*e) + _get_rprm_size(type), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	ret = _resource_alloc(&handle, type, data);
	if (ret)
		goto out;

	e->handle = handle;
	e->type = type;
	spin_lock(&rprm->lock);
	ridr = idr_find(&rprm->list, addr);
	if (!ridr) {
		spin_unlock(&rprm->lock);
		ret = -ENOTCONN;
		goto out;
	}
	if (!idr_pre_get(ridr, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}
	ret = idr_get_new(ridr, e, res_id);
	spin_unlock(&rprm->lock);
out:
	if (ret)
		kfree(e);
	return ret;
}

static int _resource_free(void *handle, int type)
{
	switch (type) {
	case RPRM_GPTIMER:
		rprm_gptimer_release(handle);
		break;
	case RPRM_L3BUS:
		rprm_l3_bus_release(handle);
		break;
	case RPRM_IVAHD:
	case RPRM_IVASEQ0:
	case RPRM_IVASEQ1:
	case RPRM_ISS:
	case RPRM_SL2IF:
	case RPRM_FDIF:
		rprm_rpres_release(handle);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int rprm_resource_free(struct rprm *rprm, u32 addr, int res_id)
{
	int ret = 0;
	struct rprm_elem *e;
	struct idr *ridr;

	spin_lock(&rprm->lock);
	ridr = idr_find(&rprm->list, addr);
	if (!ridr) {
		ret = -ENOTCONN;
		goto out;
	}
	e = idr_find(ridr, res_id);

	if (!e) {
		ret = -ENOENT;
		goto out;
	}

	ret = _resource_free(e->handle, e->type);

	if (!ret)
		idr_remove(ridr, res_id);
out:
	spin_unlock(&rprm->lock);
	return ret;
}

static int rprm_idr_free_res(int id, void *p, void *data)
{
	struct rprm_elem *e = p;

	return _resource_free(e->handle, e->type);
}

static int rprm_idr_disconnect_client(struct rprm *rprm, u32 addr)
{
	struct idr *ridr;
	int ret;

	spin_lock(&rprm->lock);
	ridr = idr_find(&rprm->list, addr);
	if (!ridr) {
		ret = -ENOTCONN;
		goto out;
	}
	ret = idr_for_each(ridr, rprm_idr_free_res, NULL);
	if (ret)
		goto out;

	idr_remove_all(ridr);
	idr_remove(&rprm->list, addr);
out:
	spin_unlock(&rprm->lock);
	if (!ret)
		kfree(ridr);

	return 0;
}

static int rpmsg_connect_client(struct rprm *rprm, u32 addr)
{
	struct idr *ridr;
	int ret;
	int tid;

	ridr = kmalloc(sizeof(*ridr), GFP_KERNEL);
	if (!ridr)
		return -ENOMEM;

	idr_init(ridr);
	spin_lock(&rprm->lock);
	if (idr_find(&rprm->list, addr)) {
		pr_err("Connection already opened\n");
		ret = -EISCONN;
		goto out;
	}
	if (!idr_pre_get(&rprm->list, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}
	ret = idr_get_new_above(&rprm->list, ridr, addr, &tid);
out:
	spin_unlock(&rprm->lock);
	if (ret)
		kfree(ridr);
	return ret;
}

static int rprm_resource(struct rprm *rprm, u32 src, int *res_id, int type,
			 int acq, int size, void *data)
{
	switch (acq) {
	case RPRM_CONNECT:
		return rpmsg_connect_client(rprm, src);
	case RPRM_REQ_ALLOC:
		if (size != _get_rprm_size(type))
			break;
		return rprm_resource_alloc(rprm, src, res_id, type, data);
	case RPRM_REQ_FREE:
		return rprm_resource_free(rprm, src, *res_id);
	case RPRM_DISCONNECT:
		return rprm_idr_disconnect_client(rprm, src);
	}

	return -EINVAL;
}

static void rprm_cb(struct rpmsg_channel *rpdev, void *data, int len,
			void *priv, u32 src)
{
	int ret;
	struct rprm *rprm = rpdev->priv;
	struct device *dev = &rpdev->dev;
	struct rprm_head *obj = data;
	char ack_msg[512];
	struct rprm_head_ack *ack = (void *)ack_msg;
	int r_sz = 0;

	if (len < sizeof(*obj)) {
		dev_err(dev, "Bad message\n");
		return;
	}

	dev_dbg(dev, "resource type %d\n"
		"acquire %d\n"
		"res_id %d",
		obj->res_type, obj->acquire, obj->res_id);

	if (obj->acquire == RPRM_REQ_ALLOC)
		r_sz = len - sizeof(*obj);

	ret = rprm_resource(rprm, src, &obj->res_id, obj->res_type,
			obj->acquire, r_sz, obj->data);
	if (ret)
		dev_err(dev, "rprm_resource failed ret = %d\n", ret);

	/* Dont ack free requests */
	if (obj->acquire == RPRM_REQ_FREE || obj->acquire == RPRM_DISCONNECT)
		return;

	/* remote processor is not interested in linux errors */
	ack->ret = ret ? -1 : 0;
	ack->res_type = obj->res_type;
	ack->res_id = obj->res_id;

	memcpy(ack->data, obj->data, r_sz);

	ret = rpmsg_sendto(rpdev, ack, sizeof(*ack) + r_sz, src);
	if (ret)
		dev_err(dev, "rprm ack failed: %d\n", ret);
}

static int rprm_cleanup_client(int id, void *p, void *data)
{
	struct idr *ridr = p;

	idr_for_each(ridr, rprm_idr_free_res, NULL);
	idr_remove_all(ridr);
	idr_destroy(ridr);
	kfree(ridr);

	return 0;
}

static int rprm_probe(struct rpmsg_channel *rpdev)
{
	struct rprm *rprm;

	rprm = kmalloc(sizeof(*rprm), GFP_KERNEL);
	if (!rprm)
		return -ENOMEM;

	spin_lock_init(&rprm->lock);
	idr_init(&rprm->list);
	rpdev->priv = rprm;

	return 0;
}

static void __devexit rprm_remove(struct rpmsg_channel *rpdev)
{
	struct rprm *rprm = rpdev->priv;

	spin_lock(&rprm->lock);

	/* clean up remaining resources */
	idr_for_each(&rprm->list, rprm_cleanup_client, NULL);
	idr_remove_all(&rprm->list);
	idr_destroy(&rprm->list);

	spin_unlock(&rprm->lock);

	kfree(rprm);
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
