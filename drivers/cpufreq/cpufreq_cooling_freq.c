/*
 * drivers/cpufreq/cpufreq_cooling_freq.c
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Contact: Eduardo Valentin <eduardo.valentin@ti.com>
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
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>

#include <linux/thermal_framework.h>

static struct thermal_dev *thermal_dev;

/*
 * cpufreq_apply_cooling: based on requested cooling level, throttle the cpu
 * @param cooling_level: percentage of required cooling at the moment
 *
 * The maximum cpu frequency will be readjusted based on the required
 * cooling_level.
 * TODO: Make it cpu independent
 */
static int cpufreq_apply_cooling(struct thermal_dev *dev,
					int cooling_level)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);
	struct cpufreq_frequency_table *table =	cpufreq_frequency_get_table(0);
	int i;

	if (cooling_level > 100)
		return -ERANGE;
	pr_info("%s\n", __func__);

	/*
	 * This procedure is to find a upper limit based on the requested
	 * cooling level.
	 * First do a walk in the list to find the first freq that satisfies
	 * the requested cooling level.
	 */
	i = 0;
	while(table[i].frequency != CPUFREQ_TABLE_END) {
		if (table[i].cooling_level >= cooling_level)
			break;
		i++;
	}
	/* If none has been found do our best. Cross your fingers or pray. */
	if (table[i].frequency == CPUFREQ_TABLE_END)
		i--;
/*	lock_policy_rwsem_write(0); */
	policy->max = table[i].frequency;
	policy->user_policy.max = policy->max;
/*	unlock_policy_rwsem_write(0); */
	cpufreq_update_policy(0);
	cpufreq_cpu_put(policy);

	return 0;
}

static struct thermal_dev_ops cpufreq_cooling_ops = {
	.cool_device = cpufreq_apply_cooling,
};

static int __init cpufreq_cooling_init(void)
{
	struct thermal_dev *tdev;

	tdev = kzalloc(sizeof(struct thermal_dev), GFP_KERNEL);
	if (tdev) {
		tdev->name = "cpufreq_cooling";
		tdev->domain_name = "cpu";
		tdev->dev_ops = &cpufreq_cooling_ops;
		thermal_cooling_dev_register(tdev);
		thermal_dev = tdev;
	}

	return 0;
}

static void __exit cpufreq_cooling_exit(void)
{
	thermal_governor_dev_unregister(thermal_dev);
}

module_init(cpufreq_cooling_init);
module_exit(cpufreq_cooling_exit);

MODULE_AUTHOR("Texas Instruments");
MODULE_DESCRIPTION("CPUfreq cooling device driver");
MODULE_LICENSE("GPL");
