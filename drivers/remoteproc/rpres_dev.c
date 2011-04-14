/*
 * Remote processor resources
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
#include <plat/omap_device.h>
#include <plat/rpres.h>

static struct rpres_ops gen_ops = {
	.start = omap_device_enable,
	.stop = omap_device_shutdown,
};

static struct omap_device_pm_latency rpres_latency[] = {
	{
		.deactivate_func = omap_device_idle_hwmods,
		.activate_func   = omap_device_enable_hwmods,
		.flags           = OMAP_DEVICE_LATENCY_AUTO_ADJUST,
	},
};

static struct rpres_platform_data rpres_data[] = {
	{
		.name = "rpres_iva",
		.oh_name = "iva",
		.ops = &gen_ops,
	},
	{
		.name = "rpres_iva_seq0",
		.oh_name = "iva_seq0",
		.ops = &gen_ops,
	},
	{
		.name = "rpres_iva_seq1",
		.oh_name = "iva_seq1",
		.ops = &gen_ops,
	},
	{
		.name = "rpres_iss",
		.oh_name = "iss",
		.ops = &gen_ops,
	},
};

static int __init init(void)
{
	int i;
	struct omap_hwmod *oh;
	const char *oh_name;
	struct omap_device_pm_latency *ohl = rpres_latency;
	int ohl_cnt = ARRAY_SIZE(rpres_latency);
	struct omap_device *od;

	for (i = 0; i < ARRAY_SIZE(rpres_data); i++) {
		oh_name = rpres_data[i].oh_name;
		oh = omap_hwmod_lookup(oh_name);
		rpres_data[i].oh = oh;
		od = omap_device_build("rpres", i, oh,
				&rpres_data[i],
				sizeof(struct rpres_platform_data),
				ohl, ohl_cnt, false);
		rpres_data[i].od = od;
	}
	return 0;
}
device_initcall(init);
