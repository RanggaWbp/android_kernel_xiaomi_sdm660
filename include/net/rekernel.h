#ifndef __UAPI_LINUX_RE_KERNEL_H
#define __UAPI_LINUX_RE_KERNEL_H

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/freezer.h>

#define MIN_USERAPP_UID     10000
#define MAX_SYSTEM_UID      2000
#define RESERVE_ORDER       17
#define WARN_AHEAD_SPACE    (1 << RESERVE_ORDER)
#define LINE_ERROR          (-1)
#define LINE_SUCCESS        (0)

enum report_type {
	BINDER,
	SIGNAL,
};
enum binder_type {
	REPLY,
	TRANSACTION,
	OVERFLOW,
};

/*
 * Kernel 4.4 (pre v2 cgroup-freezer) only exposes cgroup_freezing().
 * cgroup_task_frozen() does not exist on this tree, so we fall back
 * to cgroup_freezing() alone.
 */
static inline bool frozen_task_group(struct task_struct *task)
{
	return cgroup_freezing(task);
}

extern void rekernel_report_no_binder_rpc_code(int type, pid_t src_pid,
		struct task_struct *src, pid_t dst_pid,
		struct task_struct *dst, bool oneway, char *rpc_name);
extern void rekernel_report(int reporttype, int type, pid_t src_pid,
		struct task_struct *src, pid_t dst_pid,
		struct task_struct *dst, bool oneway, char *rpc_name,
		__u32 code);

#endif /* __UAPI_LINUX_RE_KERNEL_H */
