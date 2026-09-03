#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <net/sock.h>
#include <linux/netlink.h>

#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <net/rtnetlink.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/tcp.h>

#include "rekernel.h"

static const char *binder_type_str[] = {
	"reply",
	"transaction",
	"free_buffer_full",
};

static int netlink_unit = NETLINK_REKERNEL_MIN;
static struct sock *netlink_socket;

static void netlink_rcv_msg(struct sk_buff *socket_buffer)
{
	/* We never need to consume anything from userspace. */
}

static struct netlink_kernel_cfg rekernel_cfg = {
	.input = netlink_rcv_msg,
};

static int start_rekernel_server(void)
{
	if (netlink_socket)
		return 0;

	for (netlink_unit = NETLINK_REKERNEL_MIN;
	     netlink_unit < NETLINK_REKERNEL_MAX; netlink_unit++) {
		netlink_socket = netlink_kernel_create(&init_net, netlink_unit,
							&rekernel_cfg);
		if (netlink_socket)
			break;
	}
	if (!netlink_socket) {
		pr_err("rekernel: failed to create netlink server!\n");
		return -1;
	}
	pr_info("rekernel: server created, netlink unit: %d\n", netlink_unit);
	return 0;
}

static int rekernel_send_netlink_message(char *msg, uint16_t len)
{
	struct sk_buff *skbuffer;
	struct nlmsghdr *nlhdr;

#ifdef CONFIG_REKERNEL_DEBUG
	pr_info("rekernel: sending: %s\n", msg);
#endif

	skbuffer = nlmsg_new(len, GFP_ATOMIC);
	if (!skbuffer) {
#ifdef CONFIG_REKERNEL_DEBUG
		pr_err("rekernel: netlink alloc failure.\n");
#endif
		return -1;
	}

	nlhdr = nlmsg_put(skbuffer, 0, 0, netlink_unit, len, 0);
	if (!nlhdr) {
#ifdef CONFIG_REKERNEL_DEBUG
		pr_err("rekernel: nlmsg_put failure.\n");
#endif
		nlmsg_free(skbuffer);
		return -1;
	}

	memcpy(nlmsg_data(nlhdr), msg, len);
	return netlink_unicast(netlink_socket, skbuffer, USER_PORT, MSG_DONTWAIT);
}

void rekernel_report_no_binder_rpc_code(int type, pid_t src_pid,
		struct task_struct *src, pid_t dst_pid,
		struct task_struct *dst, bool oneway, char *rpc_name)
{
	char binder_kmsg[PACKET_SIZE];

	if (start_rekernel_server() != 0)
		return;

	if (!dst || !frozen_task_group(dst))
		return;

	snprintf(binder_kmsg, sizeof(binder_kmsg),
		"type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d,rpc_name=%s,code=-1;",
		binder_type_str[type], oneway, src_pid, task_uid(src).val,
		dst_pid, task_uid(dst).val, rpc_name);
	rekernel_send_netlink_message(binder_kmsg, strlen(binder_kmsg));
}
EXPORT_SYMBOL(rekernel_report_no_binder_rpc_code);

void rekernel_report(int reporttype, int type, pid_t src_pid,
		struct task_struct *src, pid_t dst_pid,
		struct task_struct *dst, bool oneway, char *rpc_name,
		__u32 code)
{
	char binder_kmsg[PACKET_SIZE];

	if (start_rekernel_server() != 0)
		return;

	if (!dst || !frozen_task_group(dst))
		return;

	switch (reporttype) {
	case BINDER:
		snprintf(binder_kmsg, sizeof(binder_kmsg),
			"type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d,rpc_name=%s,code=%d;",
			binder_type_str[type], oneway, src_pid,
			task_uid(src).val, dst_pid, task_uid(dst).val,
			rpc_name, code);
		break;
	case SIGNAL:
		snprintf(binder_kmsg, sizeof(binder_kmsg),
			"type=Signal,signal=%d,killer_pid=%d,killer=%d,dst_pid=%d,dst=%d;",
			type, src_pid, task_uid(src).val, dst_pid,
			task_uid(dst).val);
		break;
	default:
		return;
	}
	rekernel_send_netlink_message(binder_kmsg, strlen(binder_kmsg));
}
EXPORT_SYMBOL(rekernel_report);

int __init rekernel_init(void)
{
	start_rekernel_server();
	return 0;
}

void __exit rekernel_exit(void)
{
}

module_init(rekernel_init);
module_exit(rekernel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sakion-Team, ported for android_kernel_xiaomi_sdm660");
MODULE_DESCRIPTION("Re:Kernel - report binder/signal activity against frozen processes");
