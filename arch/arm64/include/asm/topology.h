#ifndef __ASM_TOPOLOGY_H
#define __ASM_TOPOLOGY_H

#include <linux/cpumask.h>

struct cpu_topology {
	int thread_id;
	int core_id;
	int cluster_id;
	cpumask_t thread_sibling;
	cpumask_t core_sibling;
};

extern struct cpu_topology cpu_topology[NR_CPUS];

#define topology_physical_package_id(cpu)	(cpu_topology[cpu].cluster_id)
#define topology_core_id(cpu)		(cpu_topology[cpu].core_id)
#define topology_core_cpumask(cpu)	(&cpu_topology[cpu].core_sibling)
#define topology_sibling_cpumask(cpu)	(&cpu_topology[cpu].thread_sibling)

void init_cpu_topology(void);
void store_cpu_topology(unsigned int cpuid);
const struct cpumask *cpu_coregroup_mask(int cpu);
const struct cpumask *cpu_cluster_mask(int cpu);
unsigned long arch_get_cpu_efficiency(int cpu);

/*
 * SDM660 (and every other arm64 big.LITTLE SoC) has no CONFIG_NUMA, so the
 * generic cpu_cpu_mask() in <linux/topology.h> -- cpumask_of_node(
 * cpu_to_node(cpu)) -- degenerates to cpu_online_mask, i.e. one DIE sched
 * domain spanning all 8 CPUs across both clusters.
 *
 * EAS attaches energy data per sched domain level (init_sched_energy() ->
 * check_sched_energy_data()), and check_sched_energy_data() BUG_ON()s when two
 * CPUs inside the same group carry different cap_states. CPU0-3 use
 * CLUSTER_COST0 while CPU4-7 use CLUSTER_COST1, so a DIE group spanning both
 * clusters either trips that BUG_ON or makes init_sched_energy() bail out
 * early -- leaving sd->groups->sge unset, sd_ea NULL, and
 * select_energy_cpu_brute() returning without ever calling energy_diff().
 *
 * Redirect cpu_cpu_mask to cpu_cluster_mask so the DIE level binds to the
 * cluster. The matching #ifndef guard in <linux/topology.h> skips the generic
 * cpumask_of_node() definition, so cpu_cluster_mask() -- declared above and
 * defined in arch/arm64/kernel/topology.c -- is what gets called. It returns
 * cpu_topology[cpu].core_sibling, which update_siblings_masks() fills per
 * cluster_id, i.e. {cpu0..cpu3} on the little cluster and {cpu4..cpu7} on the
 * big cluster: the 4-CPU group that CLUSTER_COST0 / CLUSTER_COST1 describe.
 */
#define cpu_cpu_mask cpu_cluster_mask

struct sched_domain;
#ifdef CONFIG_CPU_FREQ
#define arch_scale_freq_capacity cpufreq_scale_freq_capacity
extern unsigned long cpufreq_scale_freq_capacity(struct sched_domain *sd, int cpu);
extern unsigned long cpufreq_scale_max_freq_capacity(int cpu);
#endif
#define arch_scale_cpu_capacity scale_cpu_capacity
extern unsigned long scale_cpu_capacity(struct sched_domain *sd, int cpu);

#include <asm-generic/topology.h>

#endif /* _ASM_ARM_TOPOLOGY_H */
