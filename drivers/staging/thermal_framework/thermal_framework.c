/*
 * drivers/thermal/thermal_framework.c
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Dan Murphy <DMurphy@ti.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
*/

#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/err.h>

#include <linux/thermal_framework.h>

static LIST_HEAD(thermal_sensor_list);
static LIST_HEAD(thermal_cooling_list);
static LIST_HEAD(thermal_governor_list);
static DEFINE_MUTEX(thermal_cooling_list_lock);
static DEFINE_MUTEX(thermal_gov_list_lock);
static DEFINE_MUTEX(thermal_sensor_list_lock);

static atomic_t device_count;

/**
 * DOC: Introduction
 * =================
 * The Thermal Framework is designed to be a central location to link
 * temperature sensor drivers, governors and cooling agents together.
 * The principle is to have one temperature sensor to one governor to many
 * cooling agents.  This model allows the governors to impart cooling policies
 * based on the available cooling agents for a specific domain.
 *
 * The temperature sensor device should register to the framework and report
 * the temperture of the current domain that it resides in.
 *
 * The governor is responsible for imparting the cooling policy for the specific
 * domain.  The governor will be given a list of cooling agents that it can call
 * to cool the domain.
 *
 * The cooling agents primary responsibilty is to perform an operation on the
 * device to cool the domain it is responsible for.
 *
 * The sensor, governor and the cooling agents are linked in the framework
 * via the domain_name in the thermal_dev structure.
*/

/**
 * thermal_cooling_set_level() - Calls a list of cooling devices to cool the
 *				thermal domain
 * @cooling_list:	A list of cooling agents to call to cool the domain
 * @level:	The level of cooling that the agent should perform
 *
 * Returns 0 always. (For now)
 */
int thermal_cooling_set_level(struct list_head *cooling_list,
		unsigned int level)
{
	struct thermal_dev *cooling_dev;

	list_for_each_entry(cooling_dev, cooling_list, node)
		if (cooling_dev->dev_ops &&
			cooling_dev->dev_ops->cool_device) {
			pr_info("%s:Found %s\n", __func__, cooling_dev->name);
			cooling_dev->dev_ops->cool_device(cooling_dev, level);
		} else {
			pr_info("%s:Cannot find cool_device for %s\n",
				__func__, cooling_dev->name);
		}
	;
	return 0;
}
EXPORT_SYMBOL_GPL(thermal_cooling_set_level);

/**
 * thermal_sensor_set_temp() - External API to allow a sensor driver to set
 *				the current temperature for a domain
 * @tdev:	The thermal device setting the temperature
 *
 * Returns 0 always. (For now)
 */
int thermal_sensor_set_temp(struct thermal_dev *tdev)
{
	struct thermal_dev *governor_dev;
	int ret = 0;

	mutex_lock(&thermal_gov_list_lock);
	if (list_empty(&thermal_cooling_list)) {
		pr_err("%s: No Cooling devices registered\n",
			__func__);
		ret = -ENODEV;
		goto out;
	}
	if (list_empty(&thermal_governor_list)) {
		pr_err("%s: No governors registered\n",
			__func__);
		ret = -ENODEV;
		goto out;
	}
	/* TODO: Need to find the cooling device first then the governor */
	list_for_each_entry(governor_dev, &thermal_governor_list, node)
		if (!strcmp(governor_dev->domain_name, tdev->domain_name)) {
			if (governor_dev->dev_ops &&
				governor_dev->dev_ops->process_temp) {
				/* TO DO: Probably want to pass in the sensor
				 * to modify the min/max values */
				governor_dev->dev_ops->process_temp(&thermal_cooling_list,
					tdev->current_temp);
				goto out;
			} else {
				pr_info("%s:Gov did not have right function\n",
					__func__);
				goto out;
			}

		}
out:
	mutex_unlock(&thermal_gov_list_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_sensor_set_temp);

/**
 * thermal_governor_dev_register() - Registration call for thermal domain governors
 *
 * @tdev:	The thermal governor device structure.
 *
 * Returns 0 always. (For now)
 */
int thermal_governor_dev_register(struct thermal_dev *tdev)
{
	int ret = 0;
	pr_info("%s:Registering %s governor\n", __func__, tdev->name);

	tdev->index = atomic_inc_return(&device_count);
	mutex_lock(&thermal_gov_list_lock);
	list_add(&tdev->node, &thermal_governor_list);
	mutex_unlock(&thermal_gov_list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(thermal_governor_dev_register);

/**
 * thermal_governor_dev_unregister() - Unregistration call for thermal domain governors
 *
 * @tdev:	The thermal governor device structure.
 *
 * Returns 0 always. (For now)
 */
void thermal_governor_dev_unregister(struct thermal_dev *tdev)
{
	dev_set_drvdata(tdev->dev, NULL);
}
EXPORT_SYMBOL_GPL(thermal_governor_dev_unregister);

/**
 * thermal_cooling_dev_register() - Registration call for cooling agents
 *
 * @tdev:	The cooling agent device structure.
 *
 * Returns 0 always. (For now)
 */
int thermal_cooling_dev_register(struct thermal_dev *tdev)
{
	int ret = 0;
	pr_info("%s:Registering %s cooling device\n", __func__, tdev->name);
	tdev->index = atomic_inc_return(&device_count);
	mutex_lock(&thermal_cooling_list_lock);
	list_add(&tdev->node, &thermal_cooling_list);
	mutex_unlock(&thermal_cooling_list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(thermal_cooling_dev_register);

/**
 * thermal_cooling_dev_unregister() - Unregistration call for cooling agents
 *
 * @tdev:	The cooling agent device structure.
 *
 * Returns 0 always. (For now)
 */
void thermal_cooling_dev_unregister(struct thermal_dev *tdev)
{
	dev_set_drvdata(tdev->dev, NULL);
}
EXPORT_SYMBOL_GPL(thermal_cooling_dev_unregister);

/**
 * thermal_sensor_dev_register() - Registration call for temperature sensors
 *
 * @tdev:	The temperature device structure.
 *
 * Returns 0 always. (For now)
 */
int thermal_sensor_dev_register(struct thermal_dev *tdev)
{
	int ret = 0;
	pr_info("%s:Registering %s sensor\n", __func__, tdev->name);
	tdev->index = atomic_inc_return(&device_count);
	mutex_lock(&thermal_sensor_list_lock);
	list_add(&tdev->node, &thermal_sensor_list);
	mutex_unlock(&thermal_sensor_list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(thermal_sensor_dev_register);

/**
 * thermal_sensor_dev_unregister() - Unregistration call for  temperature sensors
 *
 * @tdev:	The temperature device structure.
 *
 * Returns 0 always. (For now)
 */
void thermal_sensor_dev_unregister(struct thermal_dev *tdev)
{
	dev_set_drvdata(tdev->dev, NULL);
}
EXPORT_SYMBOL_GPL(thermal_sensor_dev_unregister);

static int __init thermal_framework_init(void)
{
	return 0;
}

static void __exit thermal_framework_exit(void)
{
	return;
}

module_init(thermal_framework_init);
module_exit(thermal_framework_exit);

MODULE_AUTHOR("Dan Murphy <DMurphy@ti.com>");
MODULE_DESCRIPTION("Thermal Framework driver");
MODULE_LICENSE("GPL");
