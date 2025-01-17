/*
 * amd-pstate.c - AMD Processor P-state Frequency Driver
 *
 * Copyright (C) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Author: Huang Rui <ray.huang@amd.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/compiler.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/static_call.h>
#include <trace/events/power.h>

#include <acpi/processor.h>
#include <acpi/cppc_acpi.h>

#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/cpu_device_id.h>

#define AMD_PSTATE_TRANSITION_LATENCY	0x20000
#define AMD_PSTATE_TRANSITION_DELAY	500

static struct cpufreq_driver amd_pstate_driver;

struct amd_cpudata {
	int	cpu;

	struct freq_qos_request req[2];

	u64	cppc_req_cached;

	u32	highest_perf;
	u32	nominal_perf;
	u32	lowest_nonlinear_perf;
	u32	lowest_perf;

	u32	max_freq;
	u32	min_freq;
	u32	nominal_freq;
	u32	lowest_nonlinear_freq;

	bool	boost_supported;
};

static inline int pstate_enable(bool enable)
{
	return wrmsrl_safe(MSR_AMD_CPPC_ENABLE, enable ? 1 : 0);
}

static int cppc_enable(bool enable)
{
	int cpu, ret = 0;

	for_each_online_cpu(cpu) {
		ret = cppc_set_enable(cpu, enable ? 1 : 0);
		if (ret)
			return ret;
	}

	return ret;
}

DEFINE_STATIC_CALL(amd_pstate_enable, pstate_enable);

static inline int amd_pstate_enable(bool enable)
{
	return static_call(amd_pstate_enable)(enable);
}

static int pstate_init_perf(struct amd_cpudata *cpudata)
{
	u64 cap1;

	int ret = rdmsrl_safe_on_cpu(cpudata->cpu, MSR_AMD_CPPC_CAP1,
				     &cap1);
	if (ret)
		return ret;

	/*
	 * TODO: Introduce AMD specific power feature.
	 *
	 * CPPC entry doesn't indicate the highest performance in some ASICs.
	 */
	WRITE_ONCE(cpudata->highest_perf, amd_get_highest_perf());

	WRITE_ONCE(cpudata->nominal_perf, CAP1_NOMINAL_PERF(cap1));
	WRITE_ONCE(cpudata->lowest_nonlinear_perf, CAP1_LOWNONLIN_PERF(cap1));
	WRITE_ONCE(cpudata->lowest_perf, CAP1_LOWEST_PERF(cap1));

	return 0;
}

static int cppc_init_perf(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	WRITE_ONCE(cpudata->highest_perf, amd_get_highest_perf());

	WRITE_ONCE(cpudata->nominal_perf, cppc_perf.nominal_perf);
	WRITE_ONCE(cpudata->lowest_nonlinear_perf,
		   cppc_perf.lowest_nonlinear_perf);
	WRITE_ONCE(cpudata->lowest_perf, cppc_perf.lowest_perf);

	return 0;
}

DEFINE_STATIC_CALL(amd_pstate_init_perf, pstate_init_perf);

static inline int amd_pstate_init_perf(struct amd_cpudata *cpudata)
{
	return static_call(amd_pstate_init_perf)(cpudata);
}

static void pstate_update_perf(struct amd_cpudata *cpudata, u32 min_perf,
			       u32 des_perf, u32 max_perf, bool fast_switch)
{
	if (fast_switch)
		wrmsrl(MSR_AMD_CPPC_REQ, READ_ONCE(cpudata->cppc_req_cached));
	else
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ,
			      READ_ONCE(cpudata->cppc_req_cached));
}

static void cppc_update_perf(struct amd_cpudata *cpudata,
			     u32 min_perf, u32 des_perf,
			     u32 max_perf, bool fast_switch)
{
	struct cppc_perf_ctrls perf_ctrls;

	perf_ctrls.max_perf = max_perf;
	perf_ctrls.min_perf = min_perf;
	perf_ctrls.desired_perf = des_perf;

	cppc_set_perf(cpudata->cpu, &perf_ctrls);
}

DEFINE_STATIC_CALL(amd_pstate_update_perf, pstate_update_perf);

static inline void amd_pstate_update_perf(struct amd_cpudata *cpudata,
					  u32 min_perf, u32 des_perf,
					  u32 max_perf, bool fast_switch)
{
	static_call(amd_pstate_update_perf)(cpudata, min_perf, des_perf,
					    max_perf, fast_switch);
}

static void amd_pstate_update(struct amd_cpudata *cpudata, u32 min_perf,
			      u32 des_perf, u32 max_perf, bool fast_switch)
{
	u64 prev = READ_ONCE(cpudata->cppc_req_cached);
	u64 value = prev;

	value &= ~REQ_MIN_PERF(~0L);
	value |= REQ_MIN_PERF(min_perf);

	value &= ~REQ_DES_PERF(~0L);
	value |= REQ_DES_PERF(des_perf);

	value &= ~REQ_MAX_PERF(~0L);
	value |= REQ_MAX_PERF(max_perf);

	trace_amd_pstate_perf(min_perf, des_perf, max_perf, cpudata->cpu,
			      (value != prev), fast_switch);

	if (value == prev)
		return;

	WRITE_ONCE(cpudata->cppc_req_cached, value);

	amd_pstate_update_perf(cpudata, min_perf, des_perf,
			       max_perf, fast_switch);
}

static int amd_pstate_verify(struct cpufreq_policy_data *policy)
{
	cpufreq_verify_within_cpu_limits(policy);

	return 0;
}

static int amd_pstate_target(struct cpufreq_policy *policy,
			     unsigned int target_freq,
			     unsigned int relation)
{
	struct cpufreq_freqs freqs;
	struct amd_cpudata *cpudata = policy->driver_data;
	unsigned long amd_max_perf, amd_min_perf, amd_des_perf,
		      amd_cap_perf;

	if (!cpudata->max_freq)
		return -ENODEV;

	amd_cap_perf = READ_ONCE(cpudata->highest_perf);
	amd_min_perf = READ_ONCE(cpudata->lowest_nonlinear_perf);
	amd_max_perf = amd_cap_perf;

	freqs.old = policy->cur;
	freqs.new = target_freq;

	amd_des_perf = DIV_ROUND_CLOSEST(target_freq * amd_cap_perf,
					 cpudata->max_freq);

	cpufreq_freq_transition_begin(policy, &freqs);
	amd_pstate_update(cpudata, amd_min_perf, amd_des_perf,
			  amd_max_perf, false);
	cpufreq_freq_transition_end(policy, &freqs, false);

	return 0;
}

static void amd_pstate_adjust_perf(unsigned int cpu,
				   unsigned long min_perf,
				   unsigned long target_perf,
				   unsigned long capacity)
{
	unsigned long amd_max_perf, amd_min_perf, amd_des_perf,
		      amd_cap_perf, lowest_nonlinear_perf;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct amd_cpudata *cpudata = policy->driver_data;

	amd_cap_perf = READ_ONCE(cpudata->highest_perf);
	lowest_nonlinear_perf = READ_ONCE(cpudata->lowest_nonlinear_perf);

	if (target_perf < capacity)
		amd_des_perf = DIV_ROUND_UP(amd_cap_perf * target_perf,
					    capacity);

	amd_min_perf = READ_ONCE(cpudata->highest_perf);
	if (min_perf < capacity)
		amd_min_perf = DIV_ROUND_UP(amd_cap_perf * min_perf, capacity);

	if (amd_min_perf < lowest_nonlinear_perf)
		amd_min_perf = lowest_nonlinear_perf;

	amd_max_perf = amd_cap_perf;
	if (amd_max_perf < amd_min_perf)
		amd_max_perf = amd_min_perf;

	amd_des_perf = clamp_t(unsigned long, amd_des_perf,
			       amd_min_perf, amd_max_perf);

	amd_pstate_update(cpudata, amd_min_perf, amd_des_perf,
			  amd_max_perf, true);
}

static int amd_get_min_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	/* Switch to khz */
	return cppc_perf.lowest_freq * 1000;
}

static int amd_get_max_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 max_perf, max_freq, nominal_freq, nominal_perf;
	u64 boost_ratio;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;
	nominal_perf = READ_ONCE(cpudata->nominal_perf);
	max_perf = READ_ONCE(cpudata->highest_perf);

	boost_ratio = div_u64(max_perf << SCHED_CAPACITY_SHIFT,
			      nominal_perf);

	max_freq = nominal_freq * boost_ratio >> SCHED_CAPACITY_SHIFT;

	/* Switch to khz */
	return max_freq * 1000;
}

static int amd_get_nominal_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 nominal_freq;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;

	/* Switch to khz */
	return nominal_freq * 1000;
}

static int amd_get_lowest_nonlinear_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 lowest_nonlinear_freq, lowest_nonlinear_perf,
	    nominal_freq, nominal_perf;
	u64 lowest_nonlinear_ratio;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;
	nominal_perf = READ_ONCE(cpudata->nominal_perf);

	lowest_nonlinear_perf = cppc_perf.lowest_nonlinear_perf;

	lowest_nonlinear_ratio = div_u64(lowest_nonlinear_perf <<
					 SCHED_CAPACITY_SHIFT, nominal_perf);

	lowest_nonlinear_freq = nominal_freq * lowest_nonlinear_ratio >> SCHED_CAPACITY_SHIFT;

	/* Switch to khz */
	return lowest_nonlinear_freq * 1000;
}

static int amd_pstate_set_boost(struct cpufreq_policy *policy, int state)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	int ret;

	if (!cpudata->boost_supported) {
		pr_err("Boost mode is not supported by this processor or SBIOS\n");
		return -EINVAL;
	}

	if (state)
		policy->cpuinfo.max_freq = cpudata->max_freq;
	else
		policy->cpuinfo.max_freq = cpudata->nominal_freq;

	policy->max = policy->cpuinfo.max_freq;

	ret = freq_qos_update_request(&cpudata->req[1],
				      policy->cpuinfo.max_freq);
	if (ret < 0)
		return ret;

	return 0;
}

static void amd_pstate_boost_init(struct amd_cpudata *cpudata)
{
	u32 highest_perf, nominal_perf;

	highest_perf = READ_ONCE(cpudata->highest_perf);
	nominal_perf = READ_ONCE(cpudata->nominal_perf);

	if (highest_perf <= nominal_perf)
		return;

	cpudata->boost_supported = true;
	amd_pstate_driver.boost_enabled = true;
}

static int amd_pstate_cpu_init(struct cpufreq_policy *policy)
{
	int min_freq, max_freq, nominal_freq, lowest_nonlinear_freq, ret;
	unsigned int cpu = policy->cpu;
	struct device *dev;
	struct amd_cpudata *cpudata;

	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
	if (!cpudata)
		return -ENOMEM;

	cpudata->cpu = cpu;

	ret = amd_pstate_init_perf(cpudata);
	if (ret)
		goto free_cpudata1;

	min_freq = amd_get_min_freq(cpudata);
	max_freq = amd_get_max_freq(cpudata);
	nominal_freq = amd_get_nominal_freq(cpudata);
	lowest_nonlinear_freq = amd_get_lowest_nonlinear_freq(cpudata);

	if (min_freq < 0 || max_freq < 0 || min_freq > max_freq) {
		dev_err(dev, "min_freq(%d) or max_freq(%d) value is incorrect\n",
			min_freq, max_freq);
		ret = -EINVAL;
		goto free_cpudata1;
	}

	policy->cpuinfo.transition_latency = AMD_PSTATE_TRANSITION_LATENCY;
	policy->transition_delay_us = AMD_PSTATE_TRANSITION_DELAY;

	policy->min = min_freq;
	policy->max = max_freq;

	policy->cpuinfo.min_freq = min_freq;
	policy->cpuinfo.max_freq = max_freq;

	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.min_freq;

	if (boot_cpu_has(X86_FEATURE_AMD_CPPC))
		policy->fast_switch_possible = true;

	ret = freq_qos_add_request(&policy->constraints, &cpudata->req[0],
				   FREQ_QOS_MIN, policy->cpuinfo.min_freq);
	if (ret < 0) {
		dev_err(dev, "Failed to add min-freq constraint (%d)\n", ret);
		goto free_cpudata1;
	}

	ret = freq_qos_add_request(&policy->constraints, &cpudata->req[1],
				   FREQ_QOS_MAX, policy->cpuinfo.max_freq);
	if (ret < 0) {
		dev_err(dev, "Failed to add max-freq constraint (%d)\n", ret);
		goto free_cpudata2;
	}

	/* Initial processor data capability frequencies */
	cpudata->max_freq = max_freq;
	cpudata->min_freq = min_freq;
	cpudata->nominal_freq = nominal_freq;
	cpudata->lowest_nonlinear_freq = lowest_nonlinear_freq;

	policy->driver_data = cpudata;

	amd_pstate_boost_init(cpudata);

	return 0;

	freq_qos_remove_request(&cpudata->req[1]);
free_cpudata2:
	freq_qos_remove_request(&cpudata->req[0]);
free_cpudata1:
	kfree(cpudata);
	return ret;
}

static int amd_pstate_cpu_exit(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata;

	cpudata = policy->driver_data;

	freq_qos_remove_request(&cpudata->req[1]);
	freq_qos_remove_request(&cpudata->req[0]);
	kfree(cpudata);

	return 0;
}

/* Sysfs attributes */

/* This frequency is to indicate the maximum hardware frequency.
 * If boost is not active but supported, the frequency will be larger than the
 * one in cpuinfo.
 */
static ssize_t show_amd_pstate_max_freq(struct cpufreq_policy *policy,
					char *buf)
{
	int max_freq;
	struct amd_cpudata *cpudata;

	cpudata = policy->driver_data;

	max_freq = amd_get_max_freq(cpudata);
	if (max_freq < 0)
		return max_freq;

	return sprintf(&buf[0], "%u\n", max_freq);
}

static ssize_t show_amd_pstate_nominal_freq(struct cpufreq_policy *policy,
					    char *buf)
{
	int nominal_freq;
	struct amd_cpudata *cpudata;

	cpudata = policy->driver_data;

	nominal_freq = amd_get_nominal_freq(cpudata);
	if (nominal_freq < 0)
		return nominal_freq;

	return sprintf(&buf[0], "%u\n", nominal_freq);
}

static ssize_t show_amd_pstate_lowest_nonlinear_freq(struct cpufreq_policy *policy,
						     char *buf)
{
	int freq;
	struct amd_cpudata *cpudata;

	cpudata = policy->driver_data;

	freq = amd_get_lowest_nonlinear_freq(cpudata);
	if (freq < 0)
		return freq;

	return sprintf(&buf[0], "%u\n", freq);
}

static ssize_t show_amd_pstate_highest_perf(struct cpufreq_policy *policy,
					    char *buf)
{
	u32 perf;
	struct amd_cpudata *cpudata = policy->driver_data;

	perf = READ_ONCE(cpudata->highest_perf);

	return sprintf(&buf[0], "%u\n", perf);
}

static ssize_t show_amd_pstate_nominal_perf(struct cpufreq_policy *policy,
					    char *buf)
{
	u32 perf;
	struct amd_cpudata *cpudata = policy->driver_data;

	perf = READ_ONCE(cpudata->nominal_perf);

	return sprintf(&buf[0], "%u\n", perf);
}

static ssize_t show_amd_pstate_lowest_nonlinear_perf(struct cpufreq_policy *policy,
						     char *buf)
{
	u32 perf;
	struct amd_cpudata *cpudata = policy->driver_data;

	perf = READ_ONCE(cpudata->lowest_nonlinear_perf);

	return sprintf(&buf[0], "%u\n", perf);
}

static ssize_t show_amd_pstate_lowest_perf(struct cpufreq_policy *policy,
					   char *buf)
{
	u32 perf;
	struct amd_cpudata *cpudata = policy->driver_data;

	perf = READ_ONCE(cpudata->lowest_perf);

	return sprintf(&buf[0], "%u\n", perf);
}

cpufreq_freq_attr_ro(amd_pstate_max_freq);
cpufreq_freq_attr_ro(amd_pstate_nominal_freq);
cpufreq_freq_attr_ro(amd_pstate_lowest_nonlinear_freq);

cpufreq_freq_attr_ro(amd_pstate_highest_perf);
cpufreq_freq_attr_ro(amd_pstate_nominal_perf);
cpufreq_freq_attr_ro(amd_pstate_lowest_nonlinear_perf);
cpufreq_freq_attr_ro(amd_pstate_lowest_perf);

static struct freq_attr *amd_pstate_attr[] = {
	&amd_pstate_max_freq,
	&amd_pstate_nominal_freq,
	&amd_pstate_lowest_nonlinear_freq,
	&amd_pstate_highest_perf,
	&amd_pstate_nominal_perf,
	&amd_pstate_lowest_nonlinear_perf,
	&amd_pstate_lowest_perf,
	NULL,
};

static struct cpufreq_driver amd_pstate_driver = {
	.flags		= CPUFREQ_CONST_LOOPS | CPUFREQ_NEED_UPDATE_LIMITS,
	.verify		= amd_pstate_verify,
	.target		= amd_pstate_target,
	.init		= amd_pstate_cpu_init,
	.exit		= amd_pstate_cpu_exit,
	.set_boost	= amd_pstate_set_boost,
	.name		= "amd-pstate",
	.attr           = amd_pstate_attr,
};

static int __init amd_pstate_init(void)
{
	int ret;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return -ENODEV;

	if (!acpi_cpc_valid()) {
		pr_debug("%s, the _CPC object is not present in SBIOS\n",
			 __func__);
		return -ENODEV;
	}

	/* don't keep reloading if cpufreq_driver exists */
	if (cpufreq_get_current_driver())
		return -EEXIST;

	/* capability check */
	if (boot_cpu_has(X86_FEATURE_AMD_CPPC)) {
		pr_debug("%s, AMD CPPC MSR based functionality is supported\n",
			 __func__);
		amd_pstate_driver.adjust_perf = amd_pstate_adjust_perf;
	} else {
		static_call_update(amd_pstate_enable, cppc_enable);
		static_call_update(amd_pstate_init_perf, cppc_init_perf);
		static_call_update(amd_pstate_update_perf, cppc_update_perf);
	}

	/* enable amd pstate feature */
	ret = amd_pstate_enable(true);
	if (ret) {
		pr_err("%s, failed to enable amd-pstate with return %d\n",
		       __func__, ret);
		return ret;
	}

	ret = cpufreq_register_driver(&amd_pstate_driver);
	if (ret) {
		pr_err("%s, return %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

device_initcall(amd_pstate_init);

MODULE_AUTHOR("Huang Rui <ray.huang@amd.com>");
MODULE_DESCRIPTION("AMD Processor P-state Frequency Driver");
MODULE_LICENSE("GPL");
