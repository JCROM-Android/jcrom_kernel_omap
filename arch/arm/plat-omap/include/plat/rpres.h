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

#ifndef RPRES_H
#define RPRES_H

struct rpres_ops {
	int (*start)(struct platform_device *pdev);
	int (*stop)(struct platform_device *pdev);
	/* no PM for the momment */
	//int (*set_constraint)(struct device *dev, void *arg);
	//int (*remove_constraint)(struct device *dev);
};

struct rpres_platform_data {
	struct omap_device *od;
	char *name;
	char *oh_name;
	struct omap_hwmod *oh;
	struct rpres_ops *ops;
};

struct rpres {
	struct list_head next;
	const char *name;
	struct platform_device *pdev;
};

struct rpres *rpres_get(char *);
void rpres_put(struct rpres *);
int rpres_start(struct rpres *obj);
int rpres_stop(struct rpres *obj);
#endif /* RPRES_H */
