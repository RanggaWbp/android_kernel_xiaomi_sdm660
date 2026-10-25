// SPDX-License-Identifier: GPL-2.0
/*
 * Piece-Of-Cake (POC) fast idle CPU selector - Linux 4.4 backport
 *
 * Backported from upstream v2.6.3 (written against 6.18) into the
 * heavily-modified 4.4.302 sdm660 tree.  Self-contained on purpose:
 * 4.4 has no struct sched_domain_shared, no sched-ext, no uclamp,
 * no energy_model and no SCHED_CLUSTER, so the idle state lives in
 * this file's own per-LLC tables instead of in the sched domain.
 *
 * Idle state is tracked as one atomic64_t bitmap per last-level cache
 * domain (<= 64 CPUs per word).  The CFS wakeup path asks POC first;
 * on any ineligibility POC returns -1 and the original 4.4
 * select_idle_sibling() logic runs unchanged.
 *
 * Errors here are performance-only: the runqueue lock still guarantees
 * scheduling correctness, a stale idle bit can only cost a wasted
 * wakeup, never a crash.
 *
 * Copyright (C) 2026 Masahito Suzuki
 */

#ifdef CONFIG_SCHED_POC_SELECTOR

#define SCHED_POC_SELECTOR_AUTHOR   "Masahito Suzuki"
#define SCHED_POC_SELECTOR_PROGNAME "Piece-Of-Cake (POC) CPU Selector"
#define SCHED_POC_SELECTOR_VERSION  "2.6.3"

/*
 * 4.4: per_cpu(sd_llc_id, cpu) is the first CPU of the CPU's LLC
 * (see update_top_cache_domain() in kernel/sched/core.c).  We compact
 * those IDs into POC_NR_LLC slots.  A machine with more LLCs than
 * this simply falls back to CFS - correctness is never at risk.
 */
#define POC_NR_LLC		8
#define POC_MAX_LLC_CPUS	64	/* one 64-bit word per LLC */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/atomic.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/sysctl.h>
#include <linux/kobject.h>
#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include "sched.h"

/*
 * Static keys.  ALL of them are DEFINE_STATIC_KEY_TRUE here: upstream
 * ships smt_fallback / target_sticky / lockless_bitmap / count as FALSE,
 * but this tree wants every feature knob enabled out of the box.  Each
 * key gates a code path that is valid and cheap on its own, see the
 * comments on the individual keys below.
 */
DEFINE_STATIC_KEY_TRUE(poc_selector_active);	/* master switch */
static bool sched_poc_selector __maybe_unused = true; /* kernel.sched_poc_selector */

DEFINE_STATIC_KEY_TRUE(sched_poc_smt_fallback);
DEFINE_STATIC_KEY_TRUE(sched_poc_smt_consecutive);
DEFINE_STATIC_KEY_TRUE(sched_poc_smt_uniform);
DEFINE_STATIC_KEY_TRUE(sched_poc_target_sticky);
DEFINE_STATIC_KEY_TRUE(sched_poc_early_select);
DEFINE_STATIC_KEY_TRUE(sched_poc_greedy_search);
DEFINE_STATIC_KEY_TRUE(sched_poc_aligned);
DEFINE_STATIC_KEY_TRUE(sched_poc_packed);
DEFINE_STATIC_KEY_TRUE(sched_poc_rr_improved);
DEFINE_STATIC_KEY_TRUE(sched_poc_lockless_bitmap);
DEFINE_STATIC_KEY_TRUE(sched_poc_count_enabled);

/* debug counters (sched_poc_count_enabled) */
enum poc_level {
	POC_LV_SKIP = 0,	/* fast path ineligible, CFS handled it */
	POC_LV_NONE,		/* no idle CPU in the LLC */
	POC_LV_LOCAL,		/* target/prev/recent shortcut */
	POC_LV_CORE,		/* idle core found via the derived mask */
	POC_LV_RR,		/* round-robin pick */
	POC_LV_REJECT,		/* aligned re-validation failed */
	POC_NR_LEVELS
};

static DEFINE_PER_CPU(unsigned long[POC_NR_LEVELS], poc_debug_cnt);

static __always_inline void poc_count(enum poc_level lv)
{
	if (static_branch_likely(&sched_poc_count_enabled))
		this_cpu_inc(poc_debug_cnt[lv]);
}

static unsigned long poc_sum_level(enum poc_level lvl)
{
	unsigned long sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu(poc_debug_cnt[lvl], cpu);
	return sum;
}

/* per-CPU round-robin phase, offset by CPU id to de-correlate wakeups */
static DEFINE_PER_CPU(u32, poc_rr_counter);

/* 4.4 has no ror32(); added to include/linux/bitops.h only in 4.16. */
static __always_inline u32 poc_ror32(u32 word, unsigned int shift)
{
	shift &= 31;
	if (!shift)
		return word;
	return (word >> shift) | (word << (32 - shift));
}

/*
 * Lowest set bit of a rotated 64-bit word, de-rotated back into the
 * LLC-local bit index.
 */
static __always_inline int poc_rr_pick(u64 mask, unsigned int rot)
{
	int bit;

	if (!mask)
		return -1;
	rot &= 63;
	if (rot) {
		mask = (mask >> rot) | (mask << ((64 - rot) & 63));
		bit = __ffs64(mask);
		bit = (bit + rot) & 63;
	} else {
		bit = __ffs64(mask);
	}
	return bit;
}

/*
 * Per-LLC idle state.  Two representations, picked by
 * sched_poc_lockless_bitmap:
 *   - lockless (default): atomic64_t word updated with a cmpxchg loop.
 *     4.4 lacks atomic64_or()/atomic64_andnot() (both landed later), so
 *     the loop below is the 4.4 equivalent.
 *   - locked: plain u64 guarded by a raw spinlock.
 */
static atomic64_t poc_idle_cpus_mask[POC_NR_LLC];
static u64 poc_locked_idle_mask[POC_NR_LLC];
static raw_spinlock_t poc_llc_lock[POC_NR_LLC];

/* compact index <- per_cpu(sd_llc_id, cpu) map (sd_llc_id is a CPU no.) */
static int poc_llc_of_id[NR_CPUS];

static int poc_llc_base[POC_NR_LLC];	/* first CPU of the LLC */
static int poc_llc_size[POC_NR_LLC];
static bool poc_llc_ok[POC_NR_LLC];	/* size <= POC_MAX_LLC_CPUS */
static int poc_llc_nr;

/* SMT layout, per LLC (see poc_build_smt_layout) */
static int poc_smt_shift[POC_NR_LLC];		/* stride between siblings */
static u64 poc_smt_primary[POC_NR_LLC];		/* first-sibling bitmask */
static bool poc_smt_repr[POC_NR_LLC];		/* layout representable */
static bool poc_smt_on;			/* SMT active anywhere */

static int poc_llc_idx(int cpu)
{
	int id;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return POC_NR_LLC;
	id = per_cpu(sd_llc_id, cpu);
	if (id < 0 || id >= nr_cpu_ids)
		return POC_NR_LLC;
	return poc_llc_of_id[id];
}

static u64 poc_idle_mask(int llc)
{
	unsigned long flags;
	u64 mask;

	if (static_branch_likely(&sched_poc_lockless_bitmap))
		return atomic64_read(&poc_idle_cpus_mask[llc]);

	raw_spin_lock_irqsave(&poc_llc_lock[llc], flags);
	mask = poc_locked_idle_mask[llc];
	raw_spin_unlock_irqrestore(&poc_llc_lock[llc], flags);
	return mask;
}

static void poc_set_bit(int llc, int bit)
{
	unsigned long flags;
	u64 old, new;

	if (static_branch_likely(&sched_poc_lockless_bitmap)) {
		do {
			old = atomic64_read(&poc_idle_cpus_mask[llc]);
			new = old | (1ULL << bit);
			if (new == old)
				break;
		} while (atomic64_cmpxchg(&poc_idle_cpus_mask[llc],
					  old, new) != old);
		return;
	}

	raw_spin_lock_irqsave(&poc_llc_lock[llc], flags);
	poc_locked_idle_mask[llc] |= (1ULL << bit);
	raw_spin_unlock_irqrestore(&poc_llc_lock[llc], flags);
}

static void poc_clear_bit(int llc, int bit)
{
	unsigned long flags;
	u64 old, new;

	if (static_branch_likely(&sched_poc_lockless_bitmap)) {
		do {
			old = atomic64_read(&poc_idle_cpus_mask[llc]);
			new = old & ~(1ULL << bit);
			if (new == old)
				break;
		} while (atomic64_cmpxchg(&poc_idle_cpus_mask[llc],
					  old, new) != old);
		return;
	}

	raw_spin_lock_irqsave(&poc_llc_lock[llc], flags);
	poc_locked_idle_mask[llc] &= ~(1ULL << bit);
	raw_spin_unlock_irqrestore(&poc_llc_lock[llc], flags);
}

/*
 * Derive the "whole core idle" bitmask from the idle CPU bitmask.
 *
 * sched_poc_smt_uniform (on): general constant-stride derivation,
 *   core = m & (m >> shift) & primary
 * sched_poc_smt_consecutive (on): the shift==1 special case of the
 *   above with the classic 0x5555... mask.
 * neither, or a layout we cannot express: no cores are reported and
 * the caller falls back to the plain CPU mask (still correct, just
 * not SMT-aware).
 */
static u64 poc_idle_core_mask(int llc, u64 cpu_mask)
{
	if (!poc_smt_on)
		return 0;
	if (!poc_smt_repr[llc])
		return 0;
	if (static_branch_likely(&sched_poc_smt_uniform))
		return cpu_mask & (cpu_mask >> poc_smt_shift[llc]) &
		       poc_smt_primary[llc];
	if (static_branch_likely(&sched_poc_smt_consecutive))
		return cpu_mask & (cpu_mask >> 1) & 0x5555555555555555ULL;
	return 0;
}

/*
 * Compact per_cpu(sd_llc_id, cpu) into POC_NR_LLC slots and derive each
 * LLC's base CPU / size.  Called from sched_poc_resync().
 */
static void poc_build_map(void)
{
	int cpu, i, slot, id;

	for (i = 0; i < NR_CPUS; i++)
		poc_llc_of_id[i] = -1;
	for (i = 0; i < POC_NR_LLC; i++) {
		poc_llc_base[i] = -1;
		poc_llc_size[i] = 0;
		poc_llc_ok[i] = false;
	}
	poc_llc_nr = 0;

	for_each_possible_cpu(cpu) {
		id = per_cpu(sd_llc_id, cpu);
		if (id < 0 || id >= nr_cpu_ids)
			return;			/* unknown LLC -> all !ok */
		slot = poc_llc_of_id[id];
		if (slot < 0) {
			if (poc_llc_nr >= POC_NR_LLC)
				return;		/* too many LLCs */
			slot = poc_llc_nr++;
			poc_llc_of_id[id] = slot;
			poc_llc_base[slot] = id;
		}
		poc_llc_size[slot]++;
	}

	/*
	 * A slot is usable only when EVERY CPU of that LLC falls inside
	 * [base, base + 64); otherwise the single-word bitmap cannot
	 * represent it and the caller must fall back to CFS.
	 */
	for (i = 0; i < poc_llc_nr; i++) {
		int base = poc_llc_base[i];
		int cnt = 0;

		for_each_possible_cpu(cpu) {
			if (poc_llc_of_id[per_cpu(sd_llc_id, cpu)] != i)
				continue;
			if (cpu - base < 0 || cpu - base >= POC_MAX_LLC_CPUS)
				continue;
			cnt++;
		}
		poc_llc_ok[i] = (cnt > 0 && cnt == poc_llc_size[i]);
	}
}

/*
 * Per-LLC SMT layout: sibling stride and primary-sibling bitmask.
 * topology_sibling_cpumask() is the 4.4 arm64 SMT accessor
 * (&cpu_topology[cpu].thread_sibling); 6.18 uses cpu_smt_mask().
 */
static void poc_build_smt_layout(void)
{
	int llc, cpu, shift, bit;
	bool smt_on = false;

	for (llc = 0; llc < poc_llc_nr; llc++) {
		int base = poc_llc_base[llc];

		shift = -1;
		for_each_possible_cpu(cpu) {
			const struct cpumask *sib;
			int s;

			if (poc_llc_of_id[per_cpu(sd_llc_id, cpu)] != llc)
				continue;
			bit = cpu - base;
			if (bit < 0 || bit >= POC_MAX_LLC_CPUS)
				continue;
			sib = topology_sibling_cpumask(cpu);
			if (cpumask_weight(sib) <= 1)
				continue;
			smt_on = true;
			s = cpu - cpumask_first(sib);
			if (s <= 0)
				s = 1;
			if (shift < 0)
				shift = s;
			else if (shift != s)
				shift = 0;	/* non-uniform stride */
		}

		if (shift <= 0) {
			/*
			 * No SMT here, or a layout the stride math cannot
			 * express: keep the bitmask arithmetic harmless.
			 */
			poc_smt_shift[llc] = 1;
			poc_smt_primary[llc] = 0x5555555555555555ULL;
			poc_smt_repr[llc] = (shift == -1);
		} else {
			u64 primary = 0;

			for_each_possible_cpu(cpu) {
				if (poc_llc_of_id[per_cpu(sd_llc_id, cpu)] != llc)
					continue;
				bit = cpu - base;
				if (bit < 0 || bit >= POC_MAX_LLC_CPUS)
					continue;
				if (bit % shift == 0)
					primary |= 1ULL << bit;
			}
			poc_smt_shift[llc] = shift;
			poc_smt_primary[llc] = primary;
			poc_smt_repr[llc] = true;
		}
	}

	poc_smt_on = smt_on;
}

/*
 * Re-derive the LLC map and push the current idle state into the
 * bitmaps.  Runs at init and on every CPU hotplug transition, so a
 * re-enabled or long-disabled selector never sees stale bits.
 */
void sched_poc_resync(void)
{
	int llc, cpu;

	poc_build_map();
	poc_build_smt_layout();

	for (llc = 0; llc < POC_NR_LLC; llc++) {
		atomic64_set(&poc_idle_cpus_mask[llc], 0);
		poc_locked_idle_mask[llc] = 0;
	}

	for_each_online_cpu(cpu) {
		llc = poc_llc_idx(cpu);
		if (llc >= POC_NR_LLC || !poc_llc_ok[llc])
			continue;
		if (idle_cpu(cpu))
			poc_set_bit(llc, cpu - poc_llc_base[llc]);
		else
			poc_clear_bit(llc, cpu - poc_llc_base[llc]);
	}
}
EXPORT_SYMBOL_GPL(sched_poc_resync);

/*
 * Idle bookkeeping hook.  Called from cpu_idle_loop() when a CPU enters
 * or leaves idle and from set_task_cpu() when a task lands on a CPU.
 */
void sched_poc_set_idle(int cpu, bool idle)
{
	int llc, bit;

	if (!static_branch_likely(&poc_selector_active))
		return;

	llc = poc_llc_idx(cpu);
	if (llc >= POC_NR_LLC || !poc_llc_ok[llc])
		return;

	bit = cpu - poc_llc_base[llc];
	if (bit < 0 || bit >= POC_MAX_LLC_CPUS)
		return;

	if (idle)
		poc_set_bit(llc, bit);
	else
		poc_clear_bit(llc, bit);
}
EXPORT_SYMBOL_GPL(sched_poc_set_idle);

static u64 poc_affinity_mask(const struct cpumask *allowed, int base)
{
	u64 mask = 0;
	int i;

	for_each_cpu(i, allowed) {
		if (i >= base && i < base + POC_MAX_LLC_CPUS)
			mask |= 1ULL << (i - base);
	}
	return mask;
}

/*
 * sched_poc_select_cpu - O(1) idle CPU lookup for the CFS wakeup path.
 *
 * Returns an idle CPU inside @target's LLC that the task may run on, or
 * -1 when the fast path is not eligible / nothing idle is left, in which
 * case the caller MUST run the normal 4.4 select_idle_sibling() logic.
 *
 * Selection order (each step gated by its static key):
 *   1. target sticky           (sched_poc_target_sticky)
 *   2. prev / waker locality    (sched_poc_early_select off)
 *   3. saturation gate          (sched_poc_greedy_search off)
 *   4. packed single-CTZ pick  (sched_poc_packed, LLC <= 32 CPUs)
 *      plain round-robin pick  (otherwise, sched_poc_rr_improved)
 *   5. idle_cpu() re-validation (sched_poc_aligned)
 */
int sched_poc_select_cpu(struct task_struct *p, int prev_cpu, int target)
{
	int llc, base, bit, bit_t, bit_p, bit_r, cpu, rot;
	u64 affinity, mask, core = 0;

	if (!static_branch_likely(&poc_selector_active) || !sched_poc_selector)
		return -1;

	llc = poc_llc_idx(target);
	if (llc < 0 || llc >= POC_NR_LLC || llc >= poc_llc_nr ||
	    !poc_llc_ok[llc]) {
		poc_count(POC_LV_SKIP);
		return -1;
	}

	base = poc_llc_base[llc];
	bit_t = target - base;
	if (bit_t < 0 || bit_t >= POC_MAX_LLC_CPUS) {
		poc_count(POC_LV_SKIP);
		return -1;
	}

	affinity = poc_affinity_mask(tsk_cpus_allowed(p), base);
	if (!affinity) {
		poc_count(POC_LV_SKIP);
		return -1;
	}

	mask = poc_idle_mask(llc) & affinity;
	if (!mask) {
		poc_count(POC_LV_NONE);
		return -1;
	}

	/*
	 * SMT: prefer a fully idle core.  With sched_poc_smt_fallback on
	 * and no idle core left, hand the (SMT-sensitive) decision back
	 * to CFS instead of stealing an SMT sibling.
	 */
	if (poc_smt_on) {
		core = poc_idle_core_mask(llc, mask);
		if (static_branch_likely(&sched_poc_smt_fallback) && !core) {
			poc_count(POC_LV_SKIP);
			return -1;
		}
		if (core)
			mask = core;
	}

	/* Level 1s: the wakeup target itself is idle - keep it there. */
	if (static_branch_likely(&sched_poc_target_sticky) &&
	    (mask & (1ULL << bit_t))) {
		cpu = target;
		poc_count(POC_LV_LOCAL);
		goto out;
	}

	/*
	 * Level 1p/1r: cache locality.  Skipped when early_select is on,
	 * which spreads the load instead of hugging the previous CPU.
	 */
	if (!static_branch_likely(&sched_poc_early_select)) {
		bit_p = prev_cpu - base;
		if (prev_cpu != target && bit_p >= 0 &&
		    bit_p < POC_MAX_LLC_CPUS && (mask & (1ULL << bit_p))) {
			cpu = prev_cpu;
			poc_count(POC_LV_LOCAL);
			goto out;
		}

		/*
		 * "recent" = the CPU we are running on, i. the waker.
		 * raw_smp_processor_id() because this is also reached from
		 * the NUMA path (task_numa_compare()), where preemption
		 * may be enabled; the value is only a locality hint.
		 */
		bit_r = raw_smp_processor_id() - base;
		if (bit_r >= 0 && bit_r < POC_MAX_LLC_CPUS &&
		    (mask & (1ULL << bit_r))) {
			cpu = base + bit_r;
			poc_count(POC_LV_LOCAL);
			goto out;
		}
	}

	/*
	 * Saturation gate: when only a quarter of the LLC is idle the
	 * wide CFS scan is likely to find a better fit, so bail out.
	 * greedy_search disables the gate entirely.
	 */
	if (!static_branch_likely(&sched_poc_greedy_search) &&
	    hweight64(mask) * 4 < (u64)poc_llc_size[llc]) {
		poc_count(POC_LV_SKIP);
		return -1;
	}

	rot = (int)__this_cpu_add_return(poc_rr_counter, 1);
	if (static_branch_likely(&sched_poc_rr_improved))
		rot = (int)(((u32)rot * 0x9E3779B1u) >> 26);	/* 0..63 */
	else
		rot &= 63;

	if (static_branch_likely(&sched_poc_packed) &&
	    poc_llc_size[llc] <= 32) {
		/*
		 * Pack idle cores into the low half and all idle CPUs
		 * into the high half, rotate both by the same phase and
		 * resolve with a single count-trailing-zeros.
		 */
		u32 all, cls = 0;
		u64 packed;
		int raw, rot32 = rot & 31;

		all = poc_ror32((u32)mask, rot32);
		if (core)
			cls = poc_ror32((u32)core, rot32);
		packed = (u64)cls | ((u64)all << 32);
		if (!packed) {
			poc_count(POC_LV_NONE);
			return -1;
		}

		raw = __ffs64(packed);
		cpu = base + (((raw & 31) + rot32) & 31);
		poc_count(core ? POC_LV_CORE : POC_LV_RR);
		goto out;
	}

	bit = poc_rr_pick(mask, rot);
	if (bit < 0) {
		poc_count(POC_LV_NONE);
		return -1;
	}
	cpu = base + bit;
	poc_count(core ? POC_LV_CORE : POC_LV_RR);

out:
	if (cpu < 0 || cpu >= nr_cpu_ids) {
		poc_count(POC_LV_REJECT);
		return -1;
	}

	/*
	 * Re-validation: the bitmap is advisory, the rq lock is not.
	 * If the CPU stopped being idle while we were looking at it,
	 * fall back to CFS rather than return a busy CPU.
	 */
	if (static_branch_likely(&sched_poc_aligned) && !idle_cpu(cpu)) {
		poc_count(POC_LV_REJECT);
		return -1;
	}

	return cpu;
}
EXPORT_SYMBOL_GPL(sched_poc_select_cpu);

/*
 * Flip the master key.  Enabling also rebuilds the LLC map and pushes
 * the current idle state in, so a re-enabled selector never runs on a
 * stale bitmap.
 */
static void __maybe_unused poc_reevaluate_active(void)
{
	if (sched_poc_selector) {
		static_branch_enable(&poc_selector_active);
		sched_poc_resync();
	} else {
		static_branch_disable(&poc_selector_active);
	}
}

#ifdef CONFIG_SYSCTL

/*
 * 4.4 has proc_dointvec()/proc_dointvec_minmax() but no
 * proc_douintvec_minmax() and no SYSCTL_ZERO/SYSCTL_ONE macros, so the
 * bool knobs go through a plain int handler that coerces to 0/1.
 */
#define POC_SYSCTL_BOOL_HANDLER(fname, key)				\
static int fname(struct ctl_table *table, int write, void __user *buffer,	\
		 size_t *lenp, loff_t *ppos)				\
{									\
	int val = static_branch_likely(&(key)) ? 1 : 0;			\
	struct ctl_table tmp = {					\
		.data    = &val,					\
		.maxlen  = sizeof(val),					\
	};								\
	int ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);	\
									\
	if (!ret && write) {						\
		if (val)						\
			static_branch_enable(&(key));			\
		else							\
			static_branch_disable(&(key));			\
		poc_reevaluate_active();				\
	}								\
	return ret;							\
}

static int sched_poc_selector_sysctl_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int val = sched_poc_selector ? 1 : 0;
	struct ctl_table tmp = {
		.data    = &val,
		.maxlen  = sizeof(val),
	};
	int ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);

	if (!ret && write) {
		sched_poc_selector = !!val;
		poc_reevaluate_active();
	}
	return ret;
}

POC_SYSCTL_BOOL_HANDLER(sched_poc_smt_fallback_handler,
			sched_poc_smt_fallback)
POC_SYSCTL_BOOL_HANDLER(sched_poc_smt_consecutive_handler,
			sched_poc_smt_consecutive)
POC_SYSCTL_BOOL_HANDLER(sched_poc_smt_uniform_handler,
			sched_poc_smt_uniform)
POC_SYSCTL_BOOL_HANDLER(sched_poc_target_sticky_handler,
			sched_poc_target_sticky)
POC_SYSCTL_BOOL_HANDLER(sched_poc_early_select_handler,
			sched_poc_early_select)
POC_SYSCTL_BOOL_HANDLER(sched_poc_greedy_search_handler,
			sched_poc_greedy_search)
POC_SYSCTL_BOOL_HANDLER(sched_poc_aligned_handler,
			sched_poc_aligned)
POC_SYSCTL_BOOL_HANDLER(sched_poc_packed_handler,
			sched_poc_packed)
POC_SYSCTL_BOOL_HANDLER(sched_poc_rr_improved_handler,
			sched_poc_rr_improved)
POC_SYSCTL_BOOL_HANDLER(sched_poc_lockless_bitmap_handler,
			sched_poc_lockless_bitmap)
POC_SYSCTL_BOOL_HANDLER(sched_poc_count_handler,
			sched_poc_count_enabled)

/*
 * NOTE: the array must be terminated by an empty entry - without it
 * register_sysctl() walks off the end of the table.
 */
#define POC_CTL_BOOL(nm, fn) {			\
	.procname	= nm,			\
	.maxlen		= sizeof(int),		\
	.mode		= 0644,			\
	.proc_handler	= fn,			\
}

static struct ctl_table sched_poc_sysctls[] = {
	POC_CTL_BOOL("sched_poc_selector", sched_poc_selector_sysctl_handler)
	POC_CTL_BOOL("sched_poc_smt_fallback", sched_poc_smt_fallback_handler)
	POC_CTL_BOOL("sched_poc_smt_consecutive", sched_poc_smt_consecutive_handler)
	POC_CTL_BOOL("sched_poc_smt_uniform", sched_poc_smt_uniform_handler)
	POC_CTL_BOOL("sched_poc_target_sticky", sched_poc_target_sticky_handler)
	POC_CTL_BOOL("sched_poc_early_select", sched_poc_early_select_handler)
	POC_CTL_BOOL("sched_poc_greedy_search", sched_poc_greedy_search_handler)
	POC_CTL_BOOL("sched_poc_aligned", sched_poc_aligned_handler)
	POC_CTL_BOOL("sched_poc_packed", sched_poc_packed_handler)
	POC_CTL_BOOL("sched_poc_rr_improved", sched_poc_rr_improved_handler)
	POC_CTL_BOOL("sched_poc_lockless_bitmap", sched_poc_lockless_bitmap_handler)
	POC_CTL_BOOL("sched_poc_count", sched_poc_count_handler)
	{ } /* sentinel: end of table */
};

#endif /* CONFIG_SYSCTL */

/* CPU hotplug: an LLC's membership changes, so re-derive everything. */
static int poc_cpu_callback(struct notifier_block *nfb,
			    unsigned long action, void *hcpu)
{
	switch (action) {
	case CPU_ONLINE:
	case CPU_DEAD:
	case CPU_DOWN_FAILED:
		sched_poc_resync();
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block poc_cpu_nb = {
	.notifier_call = poc_cpu_callback,
};

/* /sys/kernel/poc_selector - minimal status surface */
static ssize_t poc_version_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", SCHED_POC_SELECTOR_VERSION);
}

static ssize_t poc_active_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n",
		       static_branch_likely(&poc_selector_active) ? 1 : 0);
}

static ssize_t poc_llc_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	int llc;
	ssize_t len = 0;

	for (llc = 0; llc < POC_NR_LLC; llc++) {
		if (!poc_llc_ok[llc])
			continue;
		len += sprintf(buf + len, "llc%d base=%d size=%d idle=0x%016llx\n",
			       llc, poc_llc_base[llc], poc_llc_size[llc],
			       (unsigned long long)poc_idle_mask(llc));
	}
	return len;
}

static ssize_t poc_count_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf + len, "skip   %lu\n", poc_sum_level(POC_LV_SKIP));
	len += sprintf(buf + len, "none   %lu\n", poc_sum_level(POC_LV_NONE));
	len += sprintf(buf + len, "local  %lu\n", poc_sum_level(POC_LV_LOCAL));
	len += sprintf(buf + len, "core   %lu\n", poc_sum_level(POC_LV_CORE));
	len += sprintf(buf + len, "rr     %lu\n", poc_sum_level(POC_LV_RR));
	len += sprintf(buf + len, "reject %lu\n", poc_sum_level(POC_LV_REJECT));
	return len;
}

static struct kobj_attribute poc_version_attr =
	__ATTR(version, 0444, poc_version_show, NULL);
static struct kobj_attribute poc_active_attr =
	__ATTR(active, 0444, poc_active_show, NULL);
static struct kobj_attribute poc_llc_attr =
	__ATTR(llc, 0444, poc_llc_show, NULL);
static struct kobj_attribute poc_count_attr =
	__ATTR(count, 0444, poc_count_show, NULL);

static struct attribute *poc_attrs[] = {
	&poc_version_attr.attr,
	&poc_active_attr.attr,
	&poc_llc_attr.attr,
	&poc_count_attr.attr,
	NULL,
};

static struct attribute_group poc_attr_group = {
	.attrs = poc_attrs,
};

static int __init sched_poc_init(void)
{
	int cpu;
	struct kobject *kobj;

	for_each_possible_cpu(cpu)
		per_cpu(poc_rr_counter, cpu) = (u32)cpu;

	sched_poc_resync();

#ifdef CONFIG_SYSCTL
	register_sysctl("kernel", sched_poc_sysctls);
#endif
	register_cpu_notifier(&poc_cpu_nb);

	kobj = kobject_create_and_add("poc_selector", kernel_kobj);
	if (kobj && sysfs_create_group(kobj, &poc_attr_group))
		kobject_put(kobj);

	printk(KERN_INFO "%s %s by %s (12/12 static keys enabled)\n",
	       SCHED_POC_SELECTOR_PROGNAME, SCHED_POC_SELECTOR_VERSION,
	       SCHED_POC_SELECTOR_AUTHOR);
	return 0;
}
subsys_initcall(sched_poc_init);

#endif /* CONFIG_SCHED_POC_SELECTOR */
