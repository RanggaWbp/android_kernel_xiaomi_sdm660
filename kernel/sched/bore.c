/*
 *  Burst-Oriented Response Enhancer (BORE) CPU Scheduler
 *  Copyright (C) 2021-2025 Masahito Suzuki <firelzrd@gmail.com>
 *
 *  4.4 classic-CFS reimplementation of BORE v7.0.0.  The EEVDF-only pieces
 *  (entity_lag / se->slice / se->deadline / pick_eevdf / reweight_eevdf /
 *  do_preempt_short / sched_update_scaling / min_base_slice_ns) do not exist
 *  on 4.4 and are dropped; burst penalty, credit, inheritance and the
 *  slice-protection tunables are kept and mapped onto vruntime / load.weight.
 */
#include <linux/cpuset.h>
#include <linux/sched/bore.h>
#include <linux/init.h>
#include <linux/math64.h>
#include "sched.h"

#ifdef CONFIG_SCHED_BORE

DEFINE_STATIC_KEY_TRUE(sched_bore_key);
unsigned int __read_mostly sched_bore                   = 1;
unsigned int __read_mostly sched_burst_inherit_type     = 2;
unsigned int __read_mostly sched_burst_protect_slice_lv = 1;
unsigned int __read_mostly sched_burst_smoothness       = 1;
unsigned int __read_mostly sched_burst_penalty_offset   = 24;
unsigned int __read_mostly sched_burst_penalty_scale    = 1536;
unsigned int __read_mostly sched_burst_cache_lifetime   = 75000000;

unsigned int __read_mostly sched_credit_cap_us = 16000;

static int __maybe_unused maxval_prio      =   39;
static int __maybe_unused maxval_6_bits    =   63;
static int __maybe_unused maxval_12_bits   = 4095;
static int __maybe_unused maxval_16_bits   = 65535;
static int __maybe_unused maxval_1_million = 1000000;

static int __maybe_unused bore_zero  = 0;
static int __maybe_unused bore_one   = 1;
static int __maybe_unused bore_two   = 2;
static int __maybe_unused bore_three = 3;

#define MAX_BURST_PENALTY ((40U << 8) - 1)
#define BURST_CACHE_SAMPLE_LIMIT 63
#define BURST_CACHE_SCAN_LIMIT (BURST_CACHE_SAMPLE_LIMIT * 2)

static u32 bore_reciprocal_lut[BURST_CACHE_SAMPLE_LIMIT + 1];

DEFINE_STATIC_KEY_TRUE(sched_burst_inherit_key);
DEFINE_STATIC_KEY_TRUE(sched_burst_ancestor_key);
DEFINE_STATIC_KEY_TRUE (sched_burst_protect_slice_cond_key);
DEFINE_STATIC_KEY_FALSE(sched_burst_protect_slice_prefer_key);
DEFINE_STATIC_KEY_FALSE(sched_credit_key);

/*
 * log2(v + 1) with an 8-bit fractional part, returned as a u32.
 * 6.12 uses an anonymous-union bitfield struct here; gnu89 forbids that,
 * so the exponent and mantissa are packed by hand instead.
 */
static inline u32 log2p1_u64_u32fp(u64 v, u8 fp)
{
	int clz;
	int exponent;
	u32 mantissa;

	if (unlikely(!v))
		return 0;
	clz = __builtin_clzll(v);
	exponent = 64 - clz;
	mantissa = (u32)(((v << clz) << 1) >> (64 - fp));
	return (exponent << fp) | mantissa;
}

void bore_note_sleep(struct task_struct *p, u64 now)
{
	p->bore.credit_sleep = now;
}

u64 bore_credit_ns(struct task_struct *p)
{
	u64 cap = (u64)sched_credit_cap_us * 1000ULL;
	u64 slept = 0, credit = 0;

	if (p->bore.credit_sleep) {
		slept = rq_clock(task_rq(p)) - p->bore.credit_sleep;
		if ((s64)slept > 0) {
			p->bore.credit_sleep = 0;
			credit = slept < cap ? slept : cap;
		} else {
			slept = 0;
		}
	}

	return credit;
}

static inline u32 calc_burst_penalty(u64 burst_time)
{
	u32 greed = log2p1_u64_u32fp(burst_time, 8);
	u32 tolerance = sched_burst_penalty_offset << 8;
	s32 diff = (s32)(greed - tolerance);
	u32 penalty = diff & ~(diff >> 31);
	u32 scaled_penalty = penalty * sched_burst_penalty_scale >> 10;
	s32 overflow = scaled_penalty - MAX_BURST_PENALTY;

	return scaled_penalty - (overflow & ~(overflow >> 31));
}

static inline u32 binary_smooth(u32 new, u32 old)
{
	u32 is_growing = (new > old);
	u32 increment = (new - old) * is_growing;
	u32 shift = sched_burst_smoothness;
	u32 smoothed = old + ((increment + (1U << shift) - 1) >> shift);

	return (new & ~(-is_growing)) | (smoothed & (-is_growing));
}

/*
 * BORE penalises a bursty task by dropping its load weight, which on classic
 * CFS makes it accrue vruntime faster (delta is divided by the weight).
 */
/*
 * fair. c owns cfs_rq_of()/task_of()/entity_is_task() and does not export
 * them, so mirror the two shapes here (this file only ever deals with a
 * task's own &p->se).
 */
#ifdef CONFIG_FAIR_GROUP_SCHED
static inline struct cfs_rq *bore_cfs_rq_of(struct sched_entity *se)
{
	return se->cfs_rq;
}
#else
static inline struct cfs_rq *bore_cfs_rq_of(struct sched_entity *se)
{
	return &task_rq(container_of(se, struct task_struct, se))->cfs;
}
#endif

static void reweight_task_by_prio(struct task_struct *p, int prio)
{
	struct sched_entity *se;
	unsigned long weight;

	if (idle_policy(p->policy))
		return;

	se = &p->se;
	weight = scale_load(prio_to_weight[prio]);

	if (se->on_rq) {
		p->bore.stop_update = true;
		reweight_entity(bore_cfs_rq_of(se), se, weight);
		p->bore.stop_update = false;
	} else {
		se->load.weight = weight;
	}
	se->load.inv_weight = prio_to_wmult[prio];
}

u8 effective_prio_bore(struct task_struct *p)
{
	int prio = p->static_prio - MAX_RT_PRIO;

	if (static_branch_likely(&sched_bore_key))
		prio += bore_score(p);
	prio &= ~(prio >> 31);
	if (prio > maxval_prio)
		prio = maxval_prio;
	return (u8)prio;
}

static void update_penalty(struct task_struct *p)
{
	struct bore_ctx *ctx = &p->bore;
	u8 prev_prio;
	u16 max_val;
	u32 is_kthread;
	u8 new_prio;
	s32 diff;

	prev_prio = effective_prio_bore(p);

	diff = (s32)ctx->curr_penalty - (s32)ctx->prev_penalty;
	max_val = ctx->curr_penalty - (diff & (diff >> 31));
	is_kthread = !!(p->flags & PF_KTHREAD);
	ctx->penalty = max_val & -(s32)(!is_kthread);

	new_prio = effective_prio_bore(p);
	if (new_prio != prev_prio)
		reweight_task_by_prio(p, new_prio);
}

void update_curr_bore(struct task_struct *p, u64 delta_exec)
{
	struct bore_ctx *ctx = &p->bore;
	u32 curr_penalty;

	if (ctx->stop_update)
		return;

	ctx->burst_time += delta_exec;
	curr_penalty = ctx->curr_penalty = calc_burst_penalty(ctx->burst_time);

	if (curr_penalty <= ctx->prev_penalty)
		return;
	update_penalty(p);
}

void restart_burst_bore(struct task_struct *p)
{
	struct bore_ctx *ctx = &p->bore;
	u32 new_penalty = binary_smooth(ctx->curr_penalty, ctx->prev_penalty);

	ctx->prev_penalty = new_penalty;
	ctx->curr_penalty = 0;
	ctx->burst_time = 0;
	update_penalty(p);
}

void restart_burst_rescale_deadline_bore(struct task_struct *p)
{
	/*
	 * 4.4 has no se->slice / se->deadline, so the EEVDF "rescale the
	 * remaining deadline" step degenerates to a plain burst restart.
	 */
	restart_burst_bore(p);
}

static inline bool task_is_bore_eligible(struct task_struct *p)
{
	return p && p->sched_class == &fair_sched_class && !p->exit_state;
}

#ifndef for_each_child_task
#define for_each_child_task(p, t) \
	list_for_each_entry_rcu(t, &(p)->children, sibling)
#endif

/* 4.4 spells this list_first_or_null_rcu(), same semantics. */
static inline struct task_struct *bore_first_child_rcu(struct task_struct *p)
{
	return list_first_or_null_rcu(&p->children, struct task_struct, sibling);
}

static inline u32 count_children_upto2(struct task_struct *p)
{
	struct list_head *head = &p->children;
	struct list_head *first = READ_ONCE(head->next);
	struct list_head *second = READ_ONCE(first->next);

	return (first != head) + (second != head);
}

static inline bool burst_cache_expired(struct bore_bc *bc, u64 now)
{
	u64 value = READ_ONCE(bc->value);
	u64 timestamp = value >> BORE_BC_TIMESTAMP_SHIFT;

	return now - timestamp > (u64)sched_burst_cache_lifetime;
}

static void update_burst_cache(struct bore_bc *bc,
		struct task_struct *p, u32 count, u32 total, u64 now)
{
	u32 average;

	if (count == 1)
		average = total;
	else
		average = (u32)(((u64)total * bore_reciprocal_lut[count]) >> 32);

	if (average < p->bore.penalty)
		average = p->bore.penalty;

	WRITE_ONCE(bc->value,
		((now >> BORE_BC_TIMESTAMP_SHIFT) << BORE_BC_TIMESTAMP_SHIFT) |
		(u64)average);
}

static u32 inherit_from_parent(struct task_struct *parent,
		unsigned long clone_flags, u64 now)
{
	u64 value;
	struct bore_bc *bc;

	if (clone_flags & CLONE_PARENT)
		parent = rcu_dereference(parent->real_parent);

	bc = &parent->bore.subtree;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *child;
		u32 count = 0, total = 0, scan_count = 0;

		for_each_child_task(parent, child) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT)
				break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT)
				break;

			if (!task_is_bore_eligible(child))
				continue;
			count++;
			total += child->bore.penalty;
		}

		update_burst_cache(bc, parent, count, total, now);
	}

	value = READ_ONCE(bc->value);
	return (u32)(value & BORE_BC_PENALTY_MASK);
}

static u32 inherit_from_ancestor_hub(struct task_struct *parent,
		unsigned long clone_flags, u64 now)
{
	u64 value;
	struct task_struct *ancestor = parent;
	struct task_struct *next;
	u32 sole_child_count = 0;
	struct bore_bc *bc;

	if (clone_flags & CLONE_PARENT) {
		ancestor = rcu_dereference(ancestor->real_parent);
		sole_child_count = 1;
	}

	while ((next = rcu_dereference(ancestor->real_parent)) != ancestor &&
			count_children_upto2(ancestor) <= sole_child_count) {
		ancestor = next;
		sole_child_count = 1;
	}

	bc = &ancestor->bore.subtree;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *direct_child;
		struct task_struct *descendant;
		u32 count = 0, total = 0, scan_count = 0;

		for_each_child_task(ancestor, direct_child) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT)
				break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT)
				break;

			descendant = direct_child;
			while (count_children_upto2(descendant) == 1) {
				struct task_struct *next_descendant =
					bore_first_child_rcu(descendant);
				if (!next_descendant)
					break;
				descendant = next_descendant;
			}

			if (!task_is_bore_eligible(descendant))
				continue;
			count++;
			total += descendant->bore.penalty;
		}

		update_burst_cache(bc, ancestor, count, total, now);
	}

	value = READ_ONCE(bc->value);
	return (u32)(value & BORE_BC_PENALTY_MASK);
}

static u32 inherit_from_thread_group(struct task_struct *p, u64 now)
{
	u64 value;
	struct task_struct *leader = p->group_leader;
	struct task_struct *sibling;
	struct bore_bc *bc = &leader->bore.group;

	if (burst_cache_expired(bc, now)) {
		u32 count = 0, total = 0, scan_count = 0;

		for_each_thread(leader, sibling) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT)
				break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT)
				break;

			if (!task_is_bore_eligible(sibling))
				continue;
			count++;
			total += sibling->bore.penalty;
		}

		update_burst_cache(bc, leader, count, total, now);
	}

	value = READ_ONCE(bc->value);
	return (u32)(value & BORE_BC_PENALTY_MASK);
}

void task_fork_bore(struct task_struct *p,
		struct task_struct *parent, unsigned long clone_flags, u64 now)
{
	struct bore_ctx *ctx;
	u32 inherited_penalty;

	if (!static_branch_likely(&sched_bore_key) || !task_is_bore_eligible(p))
		return;

	rcu_read_lock();
	ctx = &p->bore;
	if (clone_flags & CLONE_THREAD)
		inherited_penalty = inherit_from_thread_group(parent, now);
	else if (static_branch_likely(&sched_burst_inherit_key))
		inherited_penalty = static_branch_likely(&sched_burst_ancestor_key) ?
			inherit_from_ancestor_hub(parent, clone_flags, now) :
			inherit_from_parent(parent, clone_flags, now);
	else
		inherited_penalty = 0;

	if (ctx->prev_penalty < inherited_penalty)
		ctx->prev_penalty = inherited_penalty;
	ctx->curr_penalty  = 0;
	ctx->burst_time    = 0;
	ctx->stop_update   = false;
	ctx->futex_waiting = false;
	update_penalty(p);
	rcu_read_unlock();
}

void reset_task_bore(struct task_struct *p)
{
	memset(&p->bore, 0, sizeof(struct bore_ctx));
}

static void update_inherit_type(void)
{
	switch (sched_burst_inherit_type) {
	case 1:
		static_branch_enable(&sched_burst_inherit_key);
		static_branch_disable(&sched_burst_ancestor_key);
		break;
	case 2:
		static_branch_enable(&sched_burst_inherit_key);
		static_branch_enable(&sched_burst_ancestor_key);
		break;
	default:
		static_branch_disable(&sched_burst_inherit_key);
		break;
	}
}

void __init sched_init_bore(void)
{
	int i;

	printk(KERN_INFO "%s %s by %s\n",
		SCHED_BORE_PROGNAME, SCHED_BORE_VERSION, SCHED_BORE_AUTHOR);

	for (i = 1; i <= BURST_CACHE_SAMPLE_LIMIT; i++)
		bore_reciprocal_lut[i] = (u32)div64_u64(0xffffffffULL + i, i);

	reset_task_bore(&init_task);
	update_inherit_type();

	if (sched_burst_protect_slice_lv == 1 ||
	    sched_burst_protect_slice_lv == 2)
		static_branch_enable(&sched_burst_protect_slice_cond_key);
	else
		static_branch_disable(&sched_burst_protect_slice_cond_key);

	if (sched_burst_protect_slice_lv >= 2)
		static_branch_enable(&sched_burst_protect_slice_prefer_key);
	else
		static_branch_disable(&sched_burst_protect_slice_prefer_key);

	if (sched_credit_cap_us)
		static_branch_enable(&sched_credit_key);
}

static void readjust_all_task_weights(void)
{
	struct task_struct *task;
	struct rq *rq;
	struct rq_flags rf;

	read_lock(&tasklist_lock);
	for_each_process(task) {
		if (!task_is_bore_eligible(task))
			continue;
		rq = task_rq_lock(task, &rf);
		update_rq_clock(rq);
		reweight_task_by_prio(task, effective_prio_bore(task));
		task_rq_unlock(rq, task, &rf);
	}
	read_unlock(&tasklist_lock);
}

int sched_bore_update_handler(const struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	if (sched_bore)
		static_branch_enable(&sched_bore_key);
	else
		static_branch_disable(&sched_bore_key);

	readjust_all_task_weights();

	return 0;
}

int sched_burst_inherit_type_update_handler(const struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	update_inherit_type();

	return 0;
}

int sched_burst_protect_slice_lv_update_handler(const struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	if (sched_burst_protect_slice_lv == 1 ||
	    sched_burst_protect_slice_lv == 2)
		static_branch_enable(&sched_burst_protect_slice_cond_key);
	else
		static_branch_disable(&sched_burst_protect_slice_cond_key);

	if (sched_burst_protect_slice_lv >= 2)
		static_branch_enable(&sched_burst_protect_slice_prefer_key);
	else
		static_branch_disable(&sched_burst_protect_slice_prefer_key);

	return 0;
}

int sched_credit_cap_us_update_handler(const struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	if (sched_credit_cap_us)
		static_branch_enable(&sched_credit_key);
	else
		static_branch_disable(&sched_credit_key);

	return 0;
}

#ifdef CONFIG_SYSCTL
/*
 * 6.12 uses SYSCTL_ZERO/ONE/TWO/THREE and register_sysctl_init(); neither
 * exists on 4.4, so use plain static bounds and register_sysctl() from a
 * subsys_initcall -- the same pattern the POC selector uses in this tree.
 */
static struct ctl_table sched_bore_sysctls[] = {
	{
		.procname	= "sched_bore",
		.data		= &sched_bore,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sched_bore_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &bore_one,
	},
	{
		.procname	= "sched_burst_inherit_type",
		.data		= &sched_burst_inherit_type,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sched_burst_inherit_type_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &bore_two,
	},
	{
		.procname	= "sched_burst_smoothness",
		.data		= &sched_burst_smoothness,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &bore_three,
	},
	{
		.procname	= "sched_burst_penalty_offset",
		.data		= &sched_burst_penalty_offset,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &maxval_6_bits,
	},
	{
		.procname	= "sched_burst_penalty_scale",
		.data		= &sched_burst_penalty_scale,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &maxval_12_bits,
	},
	{
		.procname	= "sched_burst_cache_lifetime",
		.data		= &sched_burst_cache_lifetime,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "sched_burst_protect_slice_lv",
		.data		= &sched_burst_protect_slice_lv,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sched_burst_protect_slice_lv_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &bore_three,
	},
	{
		.procname	= "sched_credit_cap_us",
		.data		= &sched_credit_cap_us,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sched_credit_cap_us_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &maxval_1_million,
	},
	{ } /* sentinel: register_sysctl() walks off the end without it */
};

static int __init sched_bore_sysctl_init(void)
{
	register_sysctl("kernel", sched_bore_sysctls);
	return 0;
}

subsys_initcall(sched_bore_sysctl_init);

#endif /* CONFIG_SYSCTL */
#endif /* CONFIG_SCHED_BORE */
