/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal subset of 6.12's <linux/sched/cpufreq.h> that the backported
 * Reflex governor needs.
 *
 * Linux 4.4.302 has no such header at all; everything else the governor
 * uses (struct update_util_data, the SCHED_CPUFREQ_* flags,
 * cpufreq_add_update_util_hook(), ...) already lives in <linux/sched.h>.
 *
 * Only the declarations the Reflex backport adds are here: the
 * definitions live in kernel/sched/cpufreq_schedutil.c (cpufreq_get_*
 * and cpufreq_cpu_dl_bw_exceeded) and in the same file as the exported
 * PELT decay helper.
 */
#ifndef _LINUX_SCHED_CPUFREQ_H
#define _LINUX_SCHED_CPUFREQ_H

#include <linux/types.h>

#ifdef CONFIG_CPU_FREQ

struct cpufreq_policy;

/* Exported helpers for external cpufreq governors (e.g. loadable modules) */
unsigned long cpufreq_get_capacity_ref_freq(struct cpufreq_policy *policy);
void cpufreq_get_effective_util(int cpu, unsigned long boost,
				unsigned long *out_util,
				unsigned long *out_bw_min,
				unsigned long *out_bw_max);
bool cpufreq_cpu_dl_bw_exceeded(int cpu, unsigned long bw_min);
u64 cpufreq_pelt_decay(u64 val, u64 delta_ns);

#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_SCHED_CPUFREQ_H */
