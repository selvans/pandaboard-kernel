/*
 *  OMAP2PLUS cpufreq driver
 *
 *  CPU frequency scaling for OMAP
 *
 *  Copyright (C) 2005 Nokia Corporation
 *  Written by Tony Lindgren <tony@atomide.com>
 *
 *  Based on cpu-sa1110.c, Copyright (C) 2001 Russell King
 *
 * Copyright (C) 2007-2008 Texas Instruments, Inc.
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

#include <asm/system.h>
#include <asm/smp_plat.h>
#include <asm/cpu.h>

#include <plat/clock.h>
#include <plat/omap-pm.h>
#include <plat/common.h>

#include <mach/hardware.h>
#include <plat/dvfs.h>

#define VERY_HI_RATE	900000000

static struct cpufreq_frequency_table *freq_table;
static struct clk *mpu_clk;

static int omap_verify_speed(struct cpufreq_policy *policy)
{
	if (freq_table)
		return cpufreq_frequency_table_verify(policy, freq_table);

	if (policy->cpu)
		return -EINVAL;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
				     policy->cpuinfo.max_freq);

	policy->min = clk_round_rate(mpu_clk, policy->min * 1000) / 1000;
	policy->max = clk_round_rate(mpu_clk, policy->max * 1000) / 1000;
	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
				     policy->cpuinfo.max_freq);
	return 0;
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
	int i, ret = 0;
	struct cpufreq_freqs freqs;
	struct device *mpu_dev = omap2_get_mpuss_device();

	/* Wait untill all CPU's are initialized */
	if (is_smp() && (num_online_cpus() < NR_CPUS))
		return ret;

	/* Ensure desired rate is within allowed range.  Some govenors
	 * (ondemand) will just pass target_freq=0 to get the minimum. */
	if (target_freq < policy->min)
		target_freq = policy->min;
	if (target_freq > policy->max)
		target_freq = policy->max;

	freqs.old = omap_getspeed(policy->cpu);
	freqs.new = clk_round_rate(mpu_clk, target_freq * 1000) / 1000;
	freqs.cpu = policy->cpu;

	if (freqs.old == freqs.new)
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
	if (ret)
		return ret;
	freqs.new = omap_getspeed(policy->cpu);

	/*
	 * Generic CPUFREQ driver jiffy update is under !SMP. So jiffies
	 * won't get updated when UP machine cpufreq build with
	 * CONFIG_SMP enabled. Below code is added only to manage that
	 * scenario
	 */
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
	 * on frequency transition. We need to update all dependent cpus
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

static int __init omap_cpu_init(struct cpufreq_policy *policy)
{
	int result = 0;
	struct device *mpu_dev;
	static cpumask_var_t cpumask;

	mpu_clk = clk_get(NULL, "cpu_ck");
	if (IS_ERR(mpu_clk))
		return PTR_ERR(mpu_clk);

	if (policy->cpu >= NR_CPUS)
		return -EINVAL;

	policy->cur = policy->min = policy->max = omap_getspeed(policy->cpu);

	mpu_dev = omap2_get_mpuss_device();
	if (!mpu_dev) {
		pr_warning("%s: unable to get the mpu device\n", __func__);
		return -EINVAL;
	}
	opp_init_cpufreq_table(mpu_dev, &freq_table);

	if (freq_table) {
		result = cpufreq_frequency_table_cpuinfo(policy, freq_table);
		if (!result)
			cpufreq_frequency_table_get_attr(freq_table,
							policy->cpu);
	} else {
		policy->cpuinfo.min_freq = clk_round_rate(mpu_clk, 0) / 1000;
		policy->cpuinfo.max_freq = clk_round_rate(mpu_clk,
							VERY_HI_RATE) / 1000;
	}

	policy->min = policy->cpuinfo.min_freq;
	policy->max = policy->cpuinfo.max_freq;
	policy->cur = omap_getspeed(policy->cpu);

	/*
	 * On OMAP SMP configuartion, both processors share the voltage
	 * and clock. So both CPUs needs to be scaled together and hence
	 * needs software co-ordination. Use cpufreq affected_cpus
	 * interface to handle this scenario. Additional is_smp() check
	 * is to keep SMP_ON_UP builf working.
	 */
	if (is_smp()) {
		policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
		cpumask_or(cpumask, cpumask_of(policy->cpu), cpumask);
		cpumask_copy(policy->cpus, cpumask);
	}

	/* FIXME: what's the actual transition time? */
	policy->cpuinfo.transition_latency = 300 * 1000;

	return 0;
}

static int omap_cpu_exit(struct cpufreq_policy *policy)
{
	clk_exit_cpufreq_table(&freq_table);
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
	return cpufreq_register_driver(&omap_driver);
}

static void __exit omap_cpufreq_exit(void)
{
	cpufreq_unregister_driver(&omap_driver);
}

MODULE_DESCRIPTION("cpufreq driver for OMAP2PLUS SOCs");
MODULE_LICENSE("GPL");
module_init(omap_cpufreq_init);
module_exit(omap_cpufreq_exit);
