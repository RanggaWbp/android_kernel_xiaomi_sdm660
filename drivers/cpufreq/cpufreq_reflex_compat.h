/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Reflex CPUFreq governor: 6.12 -> 4.4.302 compatibility shims.
 *
 * cpufreq_reflex.c is otherwise the upstream v0.4.0 source; every place
 * where the 4.4 cpufreq/sched/EM API differs from the 6.12 one is hidden
 * behind the helpers below.
 *
 * This is a HEAVILY-MODIFIED Qualcomm 4.4.302 tree (sdm660/wayne).  The
 * subsystems the upstream governor leans on that are ABSENT here:
 *   - Energy Model (no <linux/energy_model.h>, no struct em_perf_domain)
 *   - ACPI CPPC (arm64, no CONFIG_ACPI_CPPC_LIB)
 *   - uclamp (no uclamp_max capping)
 *   - sched_ext (no scx_* cpuperf target)
 *   - fast frequency switching (stubbed out in cpufreq_schedutil.c)
 * All of those paths are compiled out in cpufreq_reflex.c; the governor
 * falls back to the cube-law power model for its speed floor.
 */
#ifndef _CPUFREQ_REFLEX_COMPAT_H
#define _CPUFREQ_REFLEX_COMPAT_H

#include <linux/bitops.h>
#include <linux/cpufreq.h>
#include <linux/math64.h>
#include <linux/types.h>

/*
 * 4.4 keeps arch_scale_freq_invariant() private to kernel/sched/sched.h.
 * The Reflex governor itself never calls it (the perf/adjust_perf fast
 * path is dropped), but mirror 4.4's own definition so any includer sees
 * the same answer schedutil does.
 */
#ifndef arch_scale_freq_invariant
#ifdef arch_scale_freq_capacity
#define arch_scale_freq_invariant()	(true)
#else
#define arch_scale_freq_invariant()	(false)
#endif
#endif

/*
 * 4.4 has no to_gov_attr_set(); the tunables kobject is embedded directly
 * as the first member of struct gov_attr_set.
 */
static inline struct gov_attr_set *to_gov_attr_set(struct kobject *kobj)
{
	return container_of(kobj, struct gov_attr_set, kobj);
}

/*
 * 4.4's map_util_freq()/map_util_perf() do not exist (they live in
 * <linux/energy_model.h> behind CONFIG_ENERGY_MODEL on newer trees), so
 * both are re-implemented here with the exact formulas 6.12 uses:
 *
 *	map_util_freq(util, freq, cap) = (freq + (freq >> 2)) * util / cap
 *	map_util_perf(util)            = util + (util >> 2)
 *
 * The (x + x>>2) factor is the 1.25x DVFS headroom (tipping point 80%).
 */
static inline unsigned long rfx_map_util_freq(unsigned long util,
					      unsigned long freq,
					      unsigned long cap)
{
	return (freq + (freq >> 2)) * util / cap;
}

static inline unsigned long rfx_map_util_perf(unsigned long util)
{
	return util + (util >> 2);
}

/*
 * 4.4 has neither cpufreq_driver_adjust_perf() nor
 * CPUFREQ_NEED_UPDATE_LIMITS, so the adjust_perf fast path can never be
 * selected and there is no driver flag to test. CPUFREQ_NEED_UPDATE_LIMITS
 * is defined purely so the code reads as upstream; the shim never reports
 * it and cpufreq_driver_has_adjust_perf() is always false (kept only for
 * source parity with the upstream governor).
 */
#ifndef CPUFREQ_NEED_UPDATE_LIMITS
#define CPUFREQ_NEED_UPDATE_LIMITS	BIT(0)
#endif

static inline bool cpufreq_driver_test_flags(u16 flags)
{
	return false;
}

static inline bool cpufreq_driver_has_adjust_perf(void)
{
	return false;
}

/*
 * 4.4's struct cpufreq_policy has no ->transition_delay_us; the only knob
 * is the driver's transition_latency, in ns.  Convert it the same way the
 * core derives sampling rates, and never return 0 (an interval of 0 would
 * disable rate limiting).
 */
static inline unsigned int
cpufreq_policy_transition_delay_us(struct cpufreq_policy *policy)
{
	unsigned int delay = policy->cpuinfo.transition_latency / NSEC_PER_USEC;

	return delay ? delay : 1;
}

/* 4.4's <linux/math64.h> has no DIV64_U64_ROUND_UP (Energy Model path only). */
#ifndef DIV64_U64_ROUND_UP
#define DIV64_U64_ROUND_UP(ll, d) \
	({ u64 _old = (ll); u64 _div = (d); div64_u64(_old + _div - 1, _div); })
#endif

#endif /* _CPUFREQ_REFLEX_COMPAT_H */
