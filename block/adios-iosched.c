// SPDX-License-Identifier: GPL-2.0
/* Adaptive Deadline I/O Scheduler (ADIOS) - Linux 4.4 legacy elevator port
 * Original: Masahito Suzuki (firelzrd), ADIOS 3.3.0 for blk-mq (Linux 6.12)
 * Port: 4.4 single-queue elevator (elevator_ops)
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/ktime.h>

#define ADIOS_OPTYPES 4
#define ADIOS_READ 0
#define ADIOS_WRITE 1
#define ADIOS_DISCARD 2
#define ADIOS_OTHER 3

#define LM_BUCKETS_SMALL 64
#define LM_BUCKETS_LARGE 32
#define MAX_BATCH_LIMIT 256

struct latency_bucket_small {
	u64 sum;
	u32 count;
};

struct latency_bucket_large {
	u64 sum;
	u32 count;
};

struct lm_buckets {
	struct latency_bucket_small small[LM_BUCKETS_SMALL];
	struct latency_bucket_large large[LM_BUCKETS_LARGE];
};

struct latency_model_params {
	u64 latency_target[ADIOS_OPTYPES];
	u32 batch_limit[ADIOS_OPTYPES];
	u32 dl_prio[2];
	u64 global_latency_window;
	u64 global_latency_window_rotational;
	u8 bq_refill_below_ratio;
	u64 lat_model_latency_limit;
	u32 lm_shrink_at_kreqs;
	u32 lm_shrink_at_gbytes;
	u32 lm_shrink_resist;
	u32 compliance_flags;
};

struct latency_model {
	struct lm_buckets buckets;
	struct latency_model_params params;
};

struct dl_group {
	struct rb_root rb_root;
	struct list_head rqs;
	u64 deadline;
};

struct adios_data {
	struct request_queue *queue;
	struct latency_model models[ADIOS_OPTYPES];
	struct dl_group dl_groups[ADIOS_OPTYPES];
	spinlock_t lock;
	unsigned int batch_count[ADIOS_OPTYPES];
	unsigned int current_optype;
	u64 last_dispatch;
};

static u64 default_latency_target[ADIOS_OPTYPES] = {
	[ADIOS_READ] = 2000000ULL,
	[ADIOS_WRITE] = 10000000ULL,
	[ADIOS_DISCARD] = 5000000ULL,
	[ADIOS_OTHER] = 5000000ULL,
};

static u32 default_batch_limit[ADIOS_OPTYPES] = {
	[ADIOS_READ] = 32,
	[ADIOS_WRITE] = 16,
	[ADIOS_DISCARD] = 8,
	[ADIOS_OTHER] = 8,
};

static u32 default_dl_prio[2] = { 8, 0 };

static u64 default_global_latency_window = 16000000ULL;
static u64 default_global_latency_window_rotational = 22000000ULL;
static u8 default_bq_refill_below_ratio = 20;
static u64 default_lat_model_latency_limit = 500 * NSEC_PER_MSEC;
static u32 default_lm_shrink_at_kreqs = 5000;
static u32 default_lm_shrink_at_gbytes = 50;
static u32 default_lm_shrink_resist = 2;
static u32 default_compliance_flags = 0;

static inline u8 adios_optype(struct request *rq)
{
	/*
	 * 4.4 has no req_op()/REQ_OP_*: rq_data_dir() is the read/write bit and
	 * discard is the REQ_DISCARD flag (REQ_SECURE marks a secure discard).
	 */
	if (rq->cmd_flags & REQ_DISCARD)
		return ADIOS_DISCARD;
	if (rq->cmd_flags & REQ_SECURE)
		return ADIOS_DISCARD;
	return rq_data_dir(rq) == WRITE ? ADIOS_WRITE : ADIOS_READ;
}

static __maybe_unused u64 lm_predict_latency(struct latency_model *model, u32 block_size)
{
	u8 idx = block_size >> 9;
	if (idx < LM_BUCKETS_SMALL) {
		struct latency_bucket_small *b = &model->buckets.small[idx];
		if (b->count)
			return div64_u64(b->sum, b->count);
	} else if (idx < LM_BUCKETS_SMALL + LM_BUCKETS_LARGE) {
		struct latency_bucket_large *b = &model->buckets.large[idx - LM_BUCKETS_SMALL];
		if (b->count)
			return div64_u64(b->sum, b->count);
	}
	return model->params.lat_model_latency_limit;
}

static void lm_update_latency(struct latency_model *model, u32 block_size, u64 measured)
{
	u8 idx = block_size >> 9;
	u64 limit = model->params.lat_model_latency_limit;
	if (measured > limit)
		measured = limit;

	if (idx < LM_BUCKETS_SMALL) {
		struct latency_bucket_small *b = &model->buckets.small[idx];
		b->sum = (b->sum * 7 + measured) / 8;
		if (b->count < 255)
			b->count++;
	} else if (idx < LM_BUCKETS_SMALL + LM_BUCKETS_LARGE) {
		struct latency_bucket_large *b = &model->buckets.large[idx - LM_BUCKETS_SMALL];
		b->sum = (b->sum * 7 + measured) / 8;
		if (b->count < 255)
			b->count++;
	}
}

static void adios_add_request(struct request_queue *q, struct request *rq)
{
	struct adios_data *ad = q->elevator->elevator_data;
	unsigned long flags;
	u8 optype = adios_optype(rq);

	spin_lock_irqsave(&ad->lock, flags);
	list_add_tail(&rq->queuelist, &ad->dl_groups[optype].rqs);
	elv_rb_add(&ad->dl_groups[optype].rb_root, rq);
	spin_unlock_irqrestore(&ad->lock, flags);
}

static int adios_dispatch(struct request_queue *q, int force)
{
	struct adios_data *ad = q->elevator->elevator_data;
	struct request *rq = NULL;
	unsigned long flags;
	u8 optype;
	int i, pass;

	spin_lock_irqsave(&ad->lock, flags);

	/*
	 * rq must be picked only after the batch-limit test passes: assigning
	 * it first and then continuing left a request linked in dl_groups
	 * while elv_dispatch_add_tail() also put it on q->queue_head,
	 * corrupting both lists.
	 *
	 * Pass 0 honours the per-group batch limit so one hot optype cannot
	 * starve the others; pass 1 ignores it.  Pass 1 is required because
	 * returning 0 with requests still queued tells __elv_next_request()
	 * the queue is empty, and a legacy request_queue is not re-kicked
	 * from the completion path -- the device would hang with requests
	 * stranded in dl_groups.  That only bit the busy partitions, which
	 * hit their batch limit within seconds.
	 */
	for (pass = 0; pass < 2 && !rq; pass++) {
		for (i = 0; i < ADIOS_OPTYPES; i++) {
			optype = (ad->current_optype + i) % ADIOS_OPTYPES;
			if (list_empty(&ad->dl_groups[optype].rqs))
				continue;
			if (!pass && !force &&
			    ad->batch_count[optype] >=
					ad->models[optype].params.batch_limit[optype]) {
				ad->batch_count[optype] = 0;
				continue;
			}
			rq = list_first_entry(&ad->dl_groups[optype].rqs,
				struct request, queuelist);
			list_del(&rq->queuelist);
			elv_rb_del(&ad->dl_groups[optype].rb_root, rq);
			ad->batch_count[optype]++;
			ad->current_optype = optype;
			ad->last_dispatch = ktime_to_ns(ktime_get());
			break;
		}
	}

	if (rq)
		elv_dispatch_add_tail(q, rq);

	spin_unlock_irqrestore(&ad->lock, flags);
	return rq ? 1 : 0;
}

static void adios_completed_request(struct request_queue *q, struct request *rq)
{
	struct adios_data *ad = q->elevator->elevator_data;
	unsigned long flags;
	u64 now, latency;
	u8 optype = adios_optype(rq);

	now = ktime_to_ns(ktime_get());
	latency = now > ad->last_dispatch ? now - ad->last_dispatch : 0;

	spin_lock_irqsave(&ad->lock, flags);
	lm_update_latency(&ad->models[optype], blk_rq_sectors(rq) << 9, latency);
	spin_unlock_irqrestore(&ad->lock, flags);
}

static void adios_merged_request(struct request_queue *q, struct request *req, int type)
{
	struct adios_data *ad = q->elevator->elevator_data;
	unsigned long flags;
	u8 optype = adios_optype(req);

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		spin_lock_irqsave(&ad->lock, flags);
		if (!RB_EMPTY_NODE(&req->rb_node)) {
			elv_rb_del(&ad->dl_groups[optype].rb_root, req);
			elv_rb_add(&ad->dl_groups[optype].rb_root, req);
		}
		spin_unlock_irqrestore(&ad->lock, flags);
	}
}

static void adios_merged_requests(struct request_queue *q, struct request *req,
				   struct request *next)
{
	struct adios_data *ad = q->elevator->elevator_data;
	unsigned long flags;
	u8 optype = adios_optype(next);

	/*
	 * kill knowledge of next, this one is a goner
	 */
	spin_lock_irqsave(&ad->lock, flags);
	if (!list_empty(&next->queuelist))
		list_del_init(&next->queuelist);
	if (!RB_EMPTY_NODE(&next->rb_node))
		elv_rb_del(&ad->dl_groups[optype].rb_root, next);
	spin_unlock_irqrestore(&ad->lock, flags);
}

static int adios_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct adios_data *ad;
	struct elevator_queue *eq;
	int i;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	ad->queue = q;
	spin_lock_init(&ad->lock);

	for (i = 0; i < ADIOS_OPTYPES; i++) {
		INIT_LIST_HEAD(&ad->dl_groups[i].rqs);
		ad->dl_groups[i].rb_root = RB_ROOT;
		ad->models[i].params.latency_target[i] = default_latency_target[i];
		ad->models[i].params.batch_limit[i] = default_batch_limit[i];
		ad->batch_count[i] = 0;
	}
	ad->models[0].params.dl_prio[0] = default_dl_prio[0];
	ad->models[0].params.dl_prio[1] = default_dl_prio[1];
	ad->models[0].params.global_latency_window = default_global_latency_window;
	ad->models[0].params.global_latency_window_rotational = default_global_latency_window_rotational;
	ad->models[0].params.bq_refill_below_ratio = default_bq_refill_below_ratio;
	ad->models[0].params.lat_model_latency_limit = default_lat_model_latency_limit;
	ad->models[0].params.lm_shrink_at_kreqs = default_lm_shrink_at_kreqs;
	ad->models[0].params.lm_shrink_at_gbytes = default_lm_shrink_at_gbytes;
	ad->models[0].params.lm_shrink_resist = default_lm_shrink_resist;
	ad->models[0].params.compliance_flags = default_compliance_flags;

	eq->elevator_data = ad;
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void adios_exit_queue(struct elevator_queue *e)
{
	struct adios_data *ad = e->elevator_data;
	int i;

	if (!ad)
		return;

	for (i = 0; i < ADIOS_OPTYPES; i++) {
		struct request *rq, *tmp;
		list_for_each_entry_safe(rq, tmp, &ad->dl_groups[i].rqs, queuelist) {
			list_del_init(&rq->queuelist);
			if (!RB_EMPTY_NODE(&rq->rb_node))
				elv_rb_del(&ad->dl_groups[i].rb_root, rq);
		}
	}
	kfree(ad);
}

static ssize_t adios_global_latency_window_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%llu\n", (unsigned long long)ad->models[0].params.global_latency_window);
}

static ssize_t adios_global_latency_window_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long long val;
	int i;

	if (sscanf(page, "%llu", &val) != 1)
		return -EINVAL;
	ad->models[0].params.global_latency_window = val;
	for (i = 1; i < ADIOS_OPTYPES; i++)
		ad->models[i].params.global_latency_window = val;
	return count;
}

static ssize_t adios_latency_target_read_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%llu\n", (unsigned long long)ad->models[0].params.latency_target[0]);
}

static ssize_t adios_latency_target_read_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long long val;
	int i;

	if (sscanf(page, "%llu", &val) != 1)
		return -EINVAL;
	ad->models[0].params.latency_target[0] = val;
	for (i = 1; i < ADIOS_OPTYPES; i++)
		ad->models[i].params.latency_target[0] = val;
	return count;
}

static ssize_t adios_latency_target_write_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%llu\n", (unsigned long long)ad->models[0].params.latency_target[1]);
}

static ssize_t adios_latency_target_write_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long long val;
	int i;

	if (sscanf(page, "%llu", &val) != 1)
		return -EINVAL;
	ad->models[0].params.latency_target[1] = val;
	for (i = 1; i < ADIOS_OPTYPES; i++)
		ad->models[i].params.latency_target[1] = val;
	return count;
}

static ssize_t adios_batch_limit_read_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%u\n", ad->models[0].params.batch_limit[0]);
}

static ssize_t adios_batch_limit_read_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int i;

	if (sscanf(page, "%lu", &val) != 1)
		return -EINVAL;
	ad->models[0].params.batch_limit[0] = val;
	for (i = 1; i < ADIOS_OPTYPES; i++)
		ad->models[i].params.batch_limit[0] = val;
	return count;
}

static ssize_t adios_batch_limit_write_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%u\n", ad->models[0].params.batch_limit[1]);
}

static ssize_t adios_batch_limit_write_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int i;

	if (sscanf(page, "%lu", &val) != 1)
		return -EINVAL;
	ad->models[0].params.batch_limit[1] = val;
	for (i = 1; i < ADIOS_OPTYPES; i++)
		ad->models[i].params.batch_limit[1] = val;
	return count;
}

static ssize_t adios_version_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "3.3.0-4.4-port\n");
}

static struct elv_fs_entry adios_attrs[] = {
	__ATTR(version, 0444, adios_version_show, NULL),
	__ATTR(global_latency_window, 0644, adios_global_latency_window_show, adios_global_latency_window_store),
	__ATTR(latency_target_read, 0644, adios_latency_target_read_show, adios_latency_target_read_store),
	__ATTR(latency_target_write, 0644, adios_latency_target_write_show, adios_latency_target_write_store),
	__ATTR(batch_limit_read, 0644, adios_batch_limit_read_show, adios_batch_limit_read_store),
	__ATTR(batch_limit_write, 0644, adios_batch_limit_write_show, adios_batch_limit_write_store),
	__ATTR_NULL
};

static struct elevator_type iosched_adios = {
	.ops = {
		/* elevator_merge_fn intentionally NULL: elv_merge() falls back to the
		 * merge hash + last_merge, which is where back/front merges come from.
		 * Pointing it at elv_merge() would recurse into itself (elevator.c
		 * calls elevator_merge_fn from elv_merge()) and blow the stack. */
		.elevator_merged_fn		= adios_merged_request,
		.elevator_merge_req_fn	= adios_merged_requests,
		.elevator_dispatch_fn	= adios_dispatch,
		.elevator_add_req_fn	= adios_add_request,
		.elevator_completed_req_fn = adios_completed_request,
		.elevator_init_fn		= adios_init_queue,
		.elevator_exit_fn		= adios_exit_queue,
		.elevator_former_req_fn	= elv_rb_former_request,
		.elevator_latter_req_fn	= elv_rb_latter_request,
	},
	.elevator_attrs = adios_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};

static int __init adios_init(void)
{
	return elv_register(&iosched_adios);
}

static void __exit adios_exit(void)
{
	elv_unregister(&iosched_adios);
}

module_init(adios_init);
module_exit(adios_exit);

MODULE_AUTHOR("Masahito Suzuki (firelzrd), ported to 4.4");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Adaptive Deadline I/O Scheduler (ADIOS) legacy elevator port");