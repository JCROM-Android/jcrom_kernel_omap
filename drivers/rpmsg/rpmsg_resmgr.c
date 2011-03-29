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
#include <plat/omap_device.h>
#include <plat/dmtimer.h>
#include <plat/rpres.h>

static LIST_HEAD(rpres_list);
static DEFINE_SPINLOCK(rpres_lock);

enum {
	RPRES_GPTIMER,
	RPRES_IVAHD,
	RPRES_IVASEQ0,
	RPRES_IVASEQ1,
	RPRES_L3BUS,
	RPRES_ISS,
};

struct rpres_elem {
	struct list_head next;
	u32 type;
	char res[];
};

struct rpres_head {
	u32 proc;
	u32 res_type;
	u32 acquire;
	u32 priv;
	u32 data_sz;
	char data[];

};

struct rpres_head_ack {
	u32 ret;
	u32 res_type;
	u32 priv;
	u32 data_sz;
	char data[];
};

struct rpres_gpt {
	u32 base;
	u32 id;
	u32 src_clk;
};

struct rpres_iva {
	u32 perf;
	u32 lat;
};

struct rpres_l3_bus {
	u32 bw;
	u32 lat;
};

struct rpres_iss {
	u32 perf;
	u32 lat;
};

bool match_gptimer(struct rpres_gpt *gpt1, struct rpres_gpt *gpt2)
{
	return gpt1->base == gpt2->base && gpt1->id == gpt2->id;
}

/*
 * If thre are more than one resource of the same type, there most be
 * a match funtion
 */
void *_get_rpres_match_fucn(int type)
{
	switch (type) {
	case RPRES_GPTIMER:
		return match_gptimer;
	}
	return NULL;
}

int _get_rpres_size(u32 type)
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

struct rpres_elem *__find_res(u32 type, void *resource)
{
	struct rpres_elem *e;
	bool (*match)(void *, void *) = _get_rpres_match_fucn(type);
	list_for_each_entry(e, &rpres_list, next)
		if (e->type == type && (!match || match(e->res, resource)))
			return e;

	return NULL;
}


struct rpres_elem *_find_res(u32 type, void *resource)
	__releases(&rpres_lock) __acquires(&rpres_lock)
{
	return  __find_res(type, resource);
}

int _add_res(u32 type, void *resource)
{
	struct rpres_elem *e;
	int s = _get_rpres_size(type);

	e = kmalloc(sizeof(*e) + s, GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	e->type = type;
	memcpy(e->res, resource, s);
	spin_lock(&rpres_lock);
	list_add_tail(&e->next, &rpres_list);
	spin_unlock(&rpres_lock);

	return 0;
}

void _del_res(u32 type, void *resource)
{
	struct rpres_elem *e;

	spin_lock(&rpres_lock);
	e = __find_res(type, resource);
	if (e) {
		list_del(&e->next);
		kfree(e);
	}
	spin_unlock(&rpres_lock);
}

static int rpres_gptimer(struct rpres_gpt *obj, bool req)
{
	struct omap_dm_timer *gpt;

	pr_err("gptimer resource\n"
			"base %x\n"
			"id %d\n"
			"source clock %d\n",
			obj->base, obj->id, obj->src_clk);
	if (req) {
		/* TODO: validate id */
		gpt = omap_dm_timer_request_specific(obj->id);
		if (!gpt)
			return -EBUSY;
		omap_dm_timer_set_source(gpt, obj->src_clk);
		obj->base = (u32)gpt;
	} else {
		gpt = (void *)obj->base;
		omap_dm_timer_free(gpt);
	}

	return 0;
}

static int rpres_iva(int type, struct rpres_iva *obj, bool req)
{
	struct rpres *res = NULL;

	pr_err("iva  resource\n"
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

	if (!res) {
		pr_err("fail to get the resource\n");
		return -EFAULT;
	}

	if (req)
		rpres_start(res);
	else
		rpres_stop(res);

	return 0;
}

static int rpres_l3_bus(struct rpres_l3_bus *obj, bool req)
{
	pr_err("l3 bus  resource\n"
			"bw %x\n"
			"lat %d\n",
			obj->bw, obj->lat);
	return 0;
}

static int rpres_iss(struct rpres_iss *obj, bool req)
{
	struct rpres *res;

	pr_err("iss  resource\n"
			"perf %x\n"
			"lat %d\n",
			obj->perf, obj->lat);

	res = rpres_get("rpres_iss");

	if (req)
		rpres_start(res);
	else
		rpres_stop(res);

	return 0;
}

static int rpres_resource(int type, bool acq, void *data)
{
	int err;

	/* if acquire, check if the resource is in used */
	if (acq && _find_res(type, data))
		return -EBUSY;

	/* if release, check if the resource exists */
	if (!acq && !_find_res(type, data))
		return -ENOENT;

	switch (type) {
	case RPRES_GPTIMER:
		err = rpres_gptimer(data, acq);
		break;
	case RPRES_IVAHD:
	case RPRES_IVASEQ0:
	case RPRES_IVASEQ1:
		err = rpres_iva(type, data, acq);
		break;
	case RPRES_L3BUS:
		err = rpres_l3_bus(data, acq);
		break;
	case RPRES_ISS:
		err = rpres_iss(data, acq);
		break;
	default:
		err = -ENOENT;
	}

	if (err)
		goto out;

	if (acq)
		err = _add_res(type, data);
	else
		_del_res(type, data);

out:
	return err;
}
static void
rpresources_cb(struct rpmsg_channel *rpdev, void *data, int len,
		void *priv, u32 src)
{
	int err;
	struct rpres_head *obj = data;
	struct rpres_head_ack *ack;

	pr_err("##remote resource request##\n");
	pr_err("proc %d\n"
		"resource type %d\n"
		"request %d\n"
		"private  %d\n"
		"data size %d\n",
		obj->proc, obj->res_type, obj->acquire, obj->priv,
		obj->data_sz);

	err = rpres_resource(obj->res_type, !!obj->acquire, obj->data);
	if (err)
		pr_err("%s: fail requesting/releasing resource %d\n",
			 __func__, err);

	/* Dont ack free requests */
	if (!obj->acquire)
		return;

	ack = kmalloc(sizeof(*ack) + obj->data_sz, GFP_KERNEL);
	if (!ack) {
		pr_err("%s: no mem\n", __func__);
		return;
	}
	/* remote processor is not interested in linux errors */
	ack->ret = err ? -1 : 0;
	ack->res_type = obj->res_type;
	ack->priv = obj->priv;
	ack->data_sz = obj->data_sz;

	memcpy(ack->data, obj->data, obj->data_sz);

	err = rpmsg_sendto(rpdev, ack, sizeof(*ack) + ack->data_sz, src);
	if (err)
		pr_err("rpres ack failed: %d\n", err);

	kfree(ack);
}

static int rpresources_probe(struct rpmsg_channel *rpdev)
{
	int err;
	char *msg = "rpmsg_resmgr ready";

	/*
	 * needed bacause of the sample in M3 side waits for
	 * message before doing the requests
	 * TODO: remove
	 */

	pr_err("resource manager ready\n");

	err = rpmsg_sendto(rpdev, msg, strlen(msg), 80);
	if (err)
		pr_err("rpmsg_send failed: %d\n", err);

	return err;
}

static void __devexit rpresources_remove(struct rpmsg_channel *rpdev)
{
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
