/*
 *  OMAP2PLUS cpufreq driver
 *
 *  CPU frequency scaling for OMAP using OPP information
 *
 *  Copyright (C) 2005 Nokia Corporation
 *  Written by Tony Lindgren <tony@atomide.com>
 *
 *  Based on cpu-sa1110.c, Copyright (C) 2001 Russell King
 *
 * Copyright (C) 2007-2011 Texas Instruments, Inc.
 * Updated to support OMAP3
 * Rajendra Nayak <rnayak@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/opp.h>
#include <linux/cpu.h>
#include <linux/thermal_framework.h>

#include <asm/system.h>
#include <asm/smp_plat.h>
#include <asm/cpu.h>

#include <plat/clock.h>
#include <plat/omap-pm.h>
#include <plat/common.h>

#include <mach/hardware.h>

#include "dvfs.h"

static struct cpufreq_frequency_table *freq_table;
static atomic_t freq_table_users = ATOMIC_INIT(0);
static struct clk *mpu_clk;
static char *mpu_clk_name;
static struct device *mpu_dev;


#ifdef CONFIG_OMAP_THERMAL
static unsigned int max_thermal;

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
	struct cpufreq_policy policy;
	unsigned int max;
	int i;

	if (cooling_level > 100) {
		pr_err("%s:Cooling level requested is out of range\n",
			__func__);
		return -ERANGE;
	}
	if (!freq_table) {
		pr_err("%s:Frequency table is NULL\n",
			__func__);
		return -EINVAL;
	}

	cpufreq_get_policy(&policy, 0);
	max = (policy.cpuinfo.max_freq - policy.cpuinfo.min_freq) *
			(100 - cooling_level) / 100;
	max += policy.cpuinfo.min_freq;

	/*
	 * This procedure is to find a upper limit based on the requested
	 * cooling level.
	 * First do a walk in the list to find the first freq that satisfies
	 * the requested cooling level.
	 */
	i = 0;
	while (freq_table[i].frequency != CPUFREQ_TABLE_END) {
		if (freq_table[i].frequency >= max)
			break;
		i++;
	}
	/* If none has been found do our best. Cross your fingers or pray. */
	if (freq_table[i].frequency == CPUFREQ_TABLE_END)
		i--;

	max_thermal = freq_table[i].frequency;

	dev_dbg(mpu_dev, "%s: thermal request to %d level."
			" Adjusting max frequency to %u "
			"(Computed %u ).\n", __func__,
			cooling_level, max_thermal, max);

	cpufreq_update_policy(0);

	return 0;
}

static void omap_cpufreq_cooling_verify_limit(struct cpufreq_policy *policy)
{
	if (policy->max > max_thermal) {
		policy->max = max_thermal;
		policy->user_policy.max = max_thermal;
	}
}

static struct thermal_dev_ops cpufreq_cooling_ops = {
	.cool_device = cpufreq_apply_cooling,
};

static struct thermal_dev thermal_dev = {
	.name		= "cpufreq_cooling",
	.domain_name	= "cpu",
	.dev_ops	= &cpufreq_cooling_ops,
};

static int __init omap_cpufreq_cooling_init(void)
{
	int i;
	int ret;

	ret = thermal_cooling_dev_register(&thermal_dev);
	if (ret)
		return ret;

	i = 0;
	while (freq_table[i].frequency != CPUFREQ_TABLE_END)
		i++;
	max_thermal = freq_table[i - 1].frequency;

	return ret;
}

static void __exit omap_cpufreq_cooling_exit(void)
{
	thermal_governor_dev_unregister(&thermal_dev);
}
#else
static void omap_cpufreq_cooling_verify_limit(struct cpufreq_policy *policy) { }
static int __init omap_cpufreq_cooling_init(void) { return 0; }
static void __exit omap_cpufreq_cooling_exit(void) { return 0; }
#endif

static int omap_verify_speed(struct cpufreq_policy *policy)
{
	if (!freq_table)
		return -EINVAL;
	omap_cpufreq_cooling_verify_limit(policy);
	return cpufreq_frequency_table_verify(policy, freq_table);
}

static unsigned int omap_getspeed(unsigned int cpu)
{
	unsigned long rate;

	if (cpu >= NR_CPUS)
		return 0;

	rate = clk_get_rate(mpu_clk) / 1000;
	return rate;
}

static int omap_target(struct cpufreq_policy *policy,
		       unsigned int target_freq,
		       unsigned int relation)
{
	unsigned int i;
	int ret = 0;
	struct cpufreq_freqs freqs;

	if (!freq_table) {
		dev_err(mpu_dev, "%s: cpu%d: no freq table!\n", __func__,
				policy->cpu);
		return -EINVAL;
	}

	ret = cpufreq_frequency_table_target(policy, freq_table, target_freq,
			relation, &i);
	if (ret) {
		dev_dbg(mpu_dev, "%s: cpu%d: no freq match for %d(ret=%d)\n",
			__func__, policy->cpu, target_freq, ret);
		return ret;
	}
	freqs.new = freq_table[i].frequency;
	if (!freqs.new) {
		dev_err(mpu_dev, "%s: cpu%d: no match for freq %d\n", __func__,
			policy->cpu, target_freq);
		return -EINVAL;
	}

	freqs.old = omap_getspeed(policy->cpu);
	freqs.cpu = policy->cpu;

	if (freqs.old == freqs.new && policy->cur == freqs.new)
		return ret;

	if (!is_smp()) {
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
		goto set_freq;
	}

	/* notifiers */
	for_each_cpu(i, policy->cpus) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	}

set_freq:
#ifdef CONFIG_CPU_FREQ_DEBUG
	pr_info("cpufreq-omap: transition: %u --> %u\n", freqs.old, freqs.new);
#endif

	ret = omap_device_scale(mpu_dev, mpu_dev, freqs.new * 1000);

	/*
	 * Generic CPUFREQ driver jiffy update is under !SMP. So jiffies
	 * won't get updated when UP machine cpufreq build with
	 * CONFIG_SMP enabled. Below code is added only to manage that
	 * scenario
	 */
	freqs.new = omap_getspeed(policy->cpu);
	if (!is_smp()) {
		loops_per_jiffy =
			 cpufreq_scale(loops_per_jiffy, freqs.old, freqs.new);
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
		goto skip_lpj;
	}

#ifdef CONFIG_SMP
	/*
	 * Note that loops_per_jiffy is not updated on SMP systems in
	 * cpufreq driver. So, update the per-CPU loops_per_jiffy value
	 * on frequency transition. We need to update all dependent CPUs.
	 */
	for_each_cpu(i, policy->cpus)
		per_cpu(cpu_data, i).loops_per_jiffy =
			cpufreq_scale(per_cpu(cpu_data, i).loops_per_jiffy,
					freqs.old, freqs.new);
#endif

	/* notifiers */
	for_each_cpu(i, policy->cpus) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}

skip_lpj:
	return ret;
}

static inline void freq_table_free(void)
{
	if (atomic_dec_and_test(&freq_table_users))
		opp_free_cpufreq_table(mpu_dev, &freq_table);
}

static int __cpuinit omap_cpu_init(struct cpufreq_policy *policy)
{
	int result = 0;

	mpu_clk = clk_get(NULL, mpu_clk_name);
	if (IS_ERR(mpu_clk))
		return PTR_ERR(mpu_clk);

	if (policy->cpu >= NR_CPUS) {
		result = -EINVAL;
		goto fail_ck;
	}

	policy->cur = policy->min = policy->max = omap_getspeed(policy->cpu);

	if (atomic_inc_return(&freq_table_users) == 1)
		result = opp_init_cpufreq_table(mpu_dev, &freq_table);

	if (result) {
		dev_err(mpu_dev, "%s: cpu%d: failed creating freq table[%d]\n",
				__func__, policy->cpu, result);
		goto fail_ck;
	}

	result = cpufreq_frequency_table_cpuinfo(policy, freq_table);
	if (result)
		goto fail_table;

	cpufreq_frequency_table_get_attr(freq_table, policy->cpu);

	policy->min = policy->cpuinfo.min_freq;
	policy->max = policy->cpuinfo.max_freq;
	policy->cur = omap_getspeed(policy->cpu);

	result = omap_cpufreq_cooling_init();
	if (result)
		goto fail_table;

	/*
	 * On OMAP SMP configuartion, both processors share the voltage
	 * and clock. So both CPUs needs to be scaled together and hence
	 * needs software co-ordination. Use cpufreq affected_cpus
	 * interface to handle this scenario. Additional is_smp() check
	 * is to keep SMP_ON_UP build working.
	 */
	if (is_smp()) {
		policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
		cpumask_setall(policy->cpus);
	}

	/* FIXME: what's the actual transition time? */
	policy->cpuinfo.transition_latency = 300 * 1000;

	return 0;

fail_table:
	freq_table_free();
fail_ck:
	clk_put(mpu_clk);
	return result;
}

static int omap_cpu_exit(struct cpufreq_policy *policy)
{
	freq_table_free();
	clk_put(mpu_clk);
	return 0;
}

static struct freq_attr *omap_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver omap_driver = {
	.flags		= CPUFREQ_STICKY,
	.verify		= omap_verify_speed,
	.target		= omap_target,
	.get		= omap_getspeed,
	.init		= omap_cpu_init,
	.exit		= omap_cpu_exit,
	.name		= "omap2plus",
	.attr		= omap_cpufreq_attr,
};

static int __init omap_cpufreq_init(void)
{
	if (cpu_is_omap24xx())
		mpu_clk_name = "virt_prcm_set";
	else if (cpu_is_omap34xx())
		mpu_clk_name = "dpll1_ck";
	else if (cpu_is_omap443x())
		mpu_clk_name = "dpll_mpu_ck";
	else if (cpu_is_omap446x())
		mpu_clk_name = "virt_dpll_mpu_ck";

	if (!mpu_clk_name) {
		pr_err("%s: unsupported Silicon?\n", __func__);
		return -EINVAL;
	}

	mpu_dev = omap2_get_mpuss_device();
	if (!mpu_dev) {
		pr_warning("%s: unable to get the mpu device\n", __func__);
		return -EINVAL;
	}

	return cpufreq_register_driver(&omap_driver);
}

static void __exit omap_cpufreq_exit(void)
{
	cpufreq_unregister_driver(&omap_driver);
	omap_cpufreq_cooling_exit();
}

MODULE_DESCRIPTION("cpufreq driver for OMAP2PLUS SOCs");
MODULE_LICENSE("GPL");
late_initcall(omap_cpufreq_init);
module_exit(omap_cpufreq_exit);
