/*
 *  SFI Processor P-States Driver
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  Author: Vishwesh M Rudramuni <vishwesh.m.rudramuni@intel.com>
 *  Author: Srinidhi Kasagar <srinidhi.kasagar@intel.com>
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sfi.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <asm/msr.h>

#define SFI_FREQ_MAX		32
#define SFI_FREQ_MASK		0xff00

static DEFINE_PER_CPU(struct cpufreq_frequency_table *, drv_data);
static struct sfi_freq_table_entry sfi_cpufreq_array[SFI_FREQ_MAX];
static int num_freq_table_entries;

static int sfi_parse_freq(struct sfi_table_header *table)
{
	struct sfi_table_simple *sb;
	struct sfi_freq_table_entry *pentry;
	int totallen;

	sb = (struct sfi_table_simple *)table;
	num_freq_table_entries = SFI_GET_NUM_ENTRIES(sb,
			struct sfi_freq_table_entry);
	if (num_freq_table_entries <= 1) {
		pr_err("No p-states discovered\n");
		return -ENODEV;
	}

	pentry = (struct sfi_freq_table_entry *)sb->pentry;
	totallen = num_freq_table_entries * sizeof(*pentry);
	memcpy(sfi_cpufreq_array, pentry, totallen);

	return 0;
}

static int sfi_cpufreq_target(struct cpufreq_policy *policy,
				unsigned int index)
{
	struct cpufreq_frequency_table *freq_table =
			per_cpu(drv_data, policy->cpu);
	unsigned int next_perf_state = 0; /* Index into perf table */
	u32 lo, hi;

	next_perf_state = freq_table[index].driver_data;

	rdmsr_on_cpu(policy->cpu, MSR_IA32_PERF_CTL, &lo, &hi);
	lo = (lo & ~INTEL_PERF_CTL_MASK) |
		((u32) sfi_cpufreq_array[next_perf_state].ctrl_val &
		INTEL_PERF_CTL_MASK);
	wrmsr_on_cpu(policy->cpu, MSR_IA32_PERF_CTL, lo, hi);

	return 0;
}

static int sfi_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *freq_table;
	unsigned int i, result, valid_states = 0;
	unsigned int cpu = policy->cpu;

	freq_table = kzalloc(sizeof(*freq_table) *
			(num_freq_table_entries + 1), GFP_KERNEL);
	if (!freq_table)
		return -ENOMEM;

	per_cpu(drv_data, cpu) = freq_table;

	policy->shared_type = CPUFREQ_SHARED_TYPE_HW;

	policy->cpuinfo.transition_latency = 0;
	for (i = 0; i < num_freq_table_entries; i++) {
		/* detect transition latency */
		if ((sfi_cpufreq_array[i].latency * 1000) >
		    policy->cpuinfo.transition_latency)
			policy->cpuinfo.transition_latency =
				sfi_cpufreq_array[i].latency * 1000;

		/* initialize the freq table */
		freq_table[valid_states].driver_data = i;
		freq_table[valid_states].frequency =
				sfi_cpufreq_array[i].freq_mhz * 1000;
		valid_states++;

		pr_debug("     P%d: %d MHz, %d uS\n",
			i,
			(u32) sfi_cpufreq_array[i].freq_mhz,
			(u32) sfi_cpufreq_array[i].latency);
	}
	freq_table[valid_states].frequency = CPUFREQ_TABLE_END;

	result = cpufreq_table_validate_and_show(policy, freq_table);
	if (result)
		goto err_free;

	pr_debug("CPU%u - SFI performance management activated.\n", cpu);

	return result;

err_free:
	per_cpu(drv_data, cpu) = NULL;
	kfree(freq_table);

	return result;
}

static int sfi_cpufreq_cpu_exit(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *freq_table =
			per_cpu(drv_data, policy->cpu);

	kfree(freq_table);
	return 0;
}

static struct cpufreq_driver sfi_cpufreq_driver = {
	.flags		= CPUFREQ_CONST_LOOPS,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= sfi_cpufreq_target,
	.init		= sfi_cpufreq_cpu_init,
	.exit		= sfi_cpufreq_cpu_exit,
	.name		= "sfi-cpufreq",
	.attr		= cpufreq_generic_attr,
};

static int __init sfi_cpufreq_init(void)
{
	int ret;

	/* parse the freq table from SFI */
	ret = sfi_table_parse(SFI_SIG_FREQ, NULL, NULL, sfi_parse_freq);
	if (ret)
		return ret;

	return cpufreq_register_driver(&sfi_cpufreq_driver);
}
late_initcall(sfi_cpufreq_init);

static void __exit sfi_cpufreq_exit(void)
{
	cpufreq_unregister_driver(&sfi_cpufreq_driver);
}
module_exit(sfi_cpufreq_exit);

MODULE_AUTHOR("Vishwesh M Rudramuni <vishwesh.m.rudramuni@intel.com>");
MODULE_DESCRIPTION("SFI P-States Driver");
MODULE_LICENSE("GPL");
