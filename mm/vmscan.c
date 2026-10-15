/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/prefetch.h>
#include <linux/printk.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#ifdef CONFIG_LRU_GEN
#include <linux/memory.h>
#include <linux/shmem_fs.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/page-flags.h>
#endif

#include <linux/swapops.h>
#include <linux/balloon_compaction.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/vmscan.h>

struct scan_control {
	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/* Allocation order */
	int order;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/* Scan (total_size >> priority) pages at once */
	int priority;

	/* The highest zone to isolate pages for reclaim from */
	enum zone_type reclaim_idx;

	unsigned int may_writepage:1;

	/* Can mapped pages be reclaimed? */
	unsigned int may_unmap:1;

	/* Can pages be swapped as part of reclaim? */
	unsigned int may_swap:1;

	/* Can cgroups be reclaimed below their normal consumption range? */
	unsigned int may_thrash:1;

	unsigned int hibernation_mode:1;

	/* One of the zones is ready for compaction */
	unsigned int compaction_ready:1;

	/* MGLRU scan state */
	unsigned int force_deactivate:1;
	unsigned int skipped_deactivate:1;
	unsigned int memcg_low_reclaim:1;

	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	/*
	 * Reclaim pages from a vma. If the page is shared by other tasks
	 * it is zapped from a vma without reclaim so it ends up remaining
	 * on memory until last task zap it.
	 */
	struct vm_area_struct *target_vma;
};

#define lru_to_page(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCH
#define prefetch_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetch(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetch_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * From 0 .. 100.  Higher means more swappy.
 */
int vm_swappiness = 60;
/*
 * The total number of pages which are beyond the high watermark within all
 * zones.
 */
unsigned long vm_total_pages;

#ifdef CONFIG_KSWAPD_CPU_AFFINITY_MASK
char *kswapd_cpu_mask = CONFIG_KSWAPD_CPU_AFFINITY_MASK;
#else
char *kswapd_cpu_mask = NULL;
#endif

static LIST_HEAD(shrinker_list);
static DECLARE_RWSEM(shrinker_rwsem);

#ifdef CONFIG_MEMCG
static bool global_reclaim(struct scan_control *sc)
{
	return !sc->target_mem_cgroup;
}

/**
 * sane_reclaim - is the usual dirty throttling mechanism operational?
 * @sc: scan_control in question
 *
 * The normal page dirty throttling mechanism in balance_dirty_pages() is
 * completely broken with the legacy memcg and direct stalling in
 * shrink_page_list() is used for throttling instead, which lacks all the
 * niceties such as fairness, adaptive pausing, bandwidth proportional
 * allocation and configurability.
 *
 * This function tests whether the vmscan currently in progress can assume
 * that the normal dirty throttling mechanism is operational.
 */
static bool sane_reclaim(struct scan_control *sc)
{
	struct mem_cgroup *memcg = sc->target_mem_cgroup;

	if (!memcg)
		return true;
#ifdef CONFIG_CGROUP_WRITEBACK
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return true;
#endif
	return false;
}
#else
static bool global_reclaim(struct scan_control *sc)
{
	return true;
}

static bool sane_reclaim(struct scan_control *sc)
{
	return true;
}
#endif

/*
 * This misses isolated pages which are not accounted for to save counters.
 * As the data only determines if reclaim or compaction continues, it is
 * not expected that isolated pages will be a dominating factor.
 */
unsigned long zone_reclaimable_pages(struct zone *zone)
{
	unsigned long nr;

	nr = zone_page_state_snapshot(zone, NR_ZONE_LRU_FILE);
	if (get_nr_swap_pages() > 0)
		nr += zone_page_state_snapshot(zone, NR_ZONE_LRU_ANON);

	return nr;
}

unsigned long pgdat_reclaimable_pages(struct pglist_data *pgdat)
{
	unsigned long nr;

	nr = node_page_state_snapshot(pgdat, NR_ACTIVE_FILE) +
	     node_page_state_snapshot(pgdat, NR_INACTIVE_FILE) +
	     node_page_state_snapshot(pgdat, NR_ISOLATED_FILE);

	if (get_nr_swap_pages() > 0)
		nr += node_page_state_snapshot(pgdat, NR_ACTIVE_ANON) +
		      node_page_state_snapshot(pgdat, NR_INACTIVE_ANON) +
		      node_page_state_snapshot(pgdat, NR_ISOLATED_ANON);

	return nr;
}

bool pgdat_reclaimable(struct pglist_data *pgdat)
{
	return node_page_state_snapshot(pgdat, NR_PAGES_SCANNED) <
		pgdat_reclaimable_pages(pgdat) * 6;
}

static unsigned long get_lru_size(struct lruvec *lruvec, enum lru_list lru)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, lru);

	return node_page_state_pgdat(lruvec_pgdat(lruvec), NR_LRU_BASE + lru);
}

/*
 * Add a shrinker callback to be called from the vm.
 */
int register_shrinker(struct shrinker *shrinker)
{
	size_t size = sizeof(*shrinker->nr_deferred);

	/*
	 * If we only have one possible node in the system anyway, save
	 * ourselves the trouble and disable NUMA aware behavior. This way we
	 * will save memory and some small loop time later.
	 */
	if (nr_node_ids == 1)
		shrinker->flags &= ~SHRINKER_NUMA_AWARE;

	if (shrinker->flags & SHRINKER_NUMA_AWARE)
		size *= nr_node_ids;

	shrinker->nr_deferred = kzalloc(size, GFP_KERNEL);
	if (!shrinker->nr_deferred)
		return -ENOMEM;

	down_write(&shrinker_rwsem);
	list_add_tail(&shrinker->list, &shrinker_list);
	up_write(&shrinker_rwsem);
	return 0;
}
EXPORT_SYMBOL(register_shrinker);

/*
 * Remove one
 */
void unregister_shrinker(struct shrinker *shrinker)
{
	if (!shrinker->nr_deferred)
		return;
	down_write(&shrinker_rwsem);
	list_del(&shrinker->list);
	up_write(&shrinker_rwsem);
	kfree(shrinker->nr_deferred);
	shrinker->nr_deferred = NULL;
}
EXPORT_SYMBOL(unregister_shrinker);

#define SHRINK_BATCH 128

static unsigned long do_shrink_slab(struct shrink_control *shrinkctl,
				    struct shrinker *shrinker,
				    unsigned long nr_scanned,
				    unsigned long nr_eligible)
{
	unsigned long freed = 0;
	unsigned long long delta;
	long total_scan;
	long freeable;
	long nr;
	long new_nr;
	int nid = shrinkctl->nid;
	long batch_size = shrinker->batch ? shrinker->batch
					  : SHRINK_BATCH;
	long scanned = 0, next_deferred;
	long min_cache_size = batch_size;

	if (current_is_kswapd())
		min_cache_size = 0;

	freeable = shrinker->count_objects(shrinker, shrinkctl);
	if (freeable == 0)
		return 0;

	/*
	 * copy the current shrinker scan count into a local variable
	 * and zero it so that other concurrent shrinker invocations
	 * don't also do this scanning work.
	 */
	nr = atomic_long_xchg(&shrinker->nr_deferred[nid], 0);

	total_scan = nr;
	delta = (4 * nr_scanned) / shrinker->seeks;
	delta *= freeable;
	do_div(delta, nr_eligible + 1);
	total_scan += delta;
	if (total_scan < 0) {
		pr_err("shrink_slab: %pF negative objects to delete nr=%ld\n",
		       shrinker->scan_objects, total_scan);
		total_scan = freeable;
		next_deferred = nr;
	} else
		next_deferred = total_scan;

	/*
	 * We need to avoid excessive windup on filesystem shrinkers
	 * due to large numbers of GFP_NOFS allocations causing the
	 * shrinkers to return -1 all the time. This results in a large
	 * nr being built up so when a shrink that can do some work
	 * comes along it empties the entire cache due to nr >>>
	 * freeable. This is bad for sustaining a working set in
	 * memory.
	 *
	 * Hence only allow the shrinker to scan the entire cache when
	 * a large delta change is calculated directly.
	 */
	if (delta < freeable / 4)
		total_scan = min(total_scan, freeable / 2);

	/*
	 * Avoid risking looping forever due to too large nr value:
	 * never try to free more than twice the estimate number of
	 * freeable entries.
	 */
	if (total_scan > freeable * 2)
		total_scan = freeable * 2;

	trace_mm_shrink_slab_start(shrinker, shrinkctl, nr,
				   nr_scanned, nr_eligible,
				   freeable, delta, total_scan);

	/*
	 * Normally, we should not scan less than batch_size objects in one
	 * pass to avoid too frequent shrinker calls, but if the slab has less
	 * than batch_size objects in total and we are really tight on memory,
	 * we will try to reclaim all available objects, otherwise we can end
	 * up failing allocations although there are plenty of reclaimable
	 * objects spread over several slabs with usage less than the
	 * batch_size.
	 *
	 * We detect the "tight on memory" situations by looking at the total
	 * number of objects we want to scan (total_scan). If it is greater
	 * than the total number of objects on slab (freeable), we must be
	 * scanning at high prio and therefore should try to reclaim as much as
	 * possible.
	 */
	while (total_scan > min_cache_size ||
	       total_scan >= freeable) {
		unsigned long ret;
		unsigned long nr_to_scan = min(batch_size, total_scan);

		shrinkctl->nr_to_scan = nr_to_scan;
		ret = shrinker->scan_objects(shrinker, shrinkctl);
		if (ret == SHRINK_STOP)
			break;
		freed += ret;

		count_vm_events(SLABS_SCANNED, nr_to_scan);
		total_scan -= nr_to_scan;
		scanned += nr_to_scan;

		cond_resched();
	}

	if (next_deferred >= scanned)
		next_deferred -= scanned;
	else
		next_deferred = 0;
	/*
	 * move the unused scan count back into the shrinker in a
	 * manner that handles concurrent updates. If we exhausted the
	 * scan, there is no need to do an update.
	 */
	if (next_deferred > 0)
		new_nr = atomic_long_add_return(next_deferred,
						&shrinker->nr_deferred[nid]);
	else
		new_nr = atomic_long_read(&shrinker->nr_deferred[nid]);

	trace_mm_shrink_slab_end(shrinker, nid, freed, nr, new_nr, total_scan);
	return freed;
}

static void shrink_slab_lmk(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 unsigned long nr_scanned,
				 unsigned long nr_eligible)
{
	struct shrinker *shrinker;

	if (nr_scanned == 0)
		nr_scanned = SWAP_CLUSTER_MAX;

	if (!down_read_trylock(&shrinker_rwsem))
		goto out;

	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
		};

		if (!(shrinker->flags & SHRINKER_LMK))
			continue;

		do_shrink_slab(&sc, shrinker, nr_scanned, nr_eligible);
	}

	up_read(&shrinker_rwsem);
out:
	cond_resched();
}

/**
 * shrink_slab - shrink slab caches
 * @gfp_mask: allocation context
 * @nid: node whose slab caches to target
 * @memcg: memory cgroup whose slab caches to target
 * @nr_scanned: pressure numerator
 * @nr_eligible: pressure denominator
 *
 * Call the shrink functions to age shrinkable caches.
 *
 * @nid is passed along to shrinkers with SHRINKER_NUMA_AWARE set,
 * unaware shrinkers will receive a node id of 0 instead.
 *
 * @memcg specifies the memory cgroup to target. If it is not NULL,
 * only shrinkers with SHRINKER_MEMCG_AWARE set will be called to scan
 * objects from the memory cgroup specified. Otherwise all shrinkers
 * are called, and memcg aware shrinkers are supposed to scan the
 * global list then.
 *
 * @nr_scanned and @nr_eligible form a ratio that indicate how much of
 * the available objects should be scanned.  Page reclaim for example
 * passes the number of pages scanned and the number of pages on the
 * LRU lists that it considered on @nid, plus a bias in @nr_scanned
 * when it encountered mapped pages.  The ratio is further biased by
 * the ->seeks setting of the shrink function, which indicates the
 * cost to recreate an object relative to that of an LRU page.
 *
 * Returns the number of reclaimed slab objects.
 */
static unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 unsigned long nr_scanned,
				 unsigned long nr_eligible)
{
	struct shrinker *shrinker;
	unsigned long freed = 0;

	if (memcg && !memcg_kmem_is_active(memcg))
		return 0;

	if (nr_scanned == 0)
		nr_scanned = SWAP_CLUSTER_MAX;

	if (!down_read_trylock(&shrinker_rwsem)) {
		/*
		 * If we would return 0, our callers would understand that we
		 * have nothing else to shrink and give up trying. By returning
		 * 1 we keep it going and assume we'll be able to shrink next
		 * time.
		 */
		freed = 1;
		goto out;
	}

	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};

		if (shrinker->flags & SHRINKER_LMK)
			continue;

		if (memcg && !(shrinker->flags & SHRINKER_MEMCG_AWARE))
			continue;

		if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
			sc.nid = 0;

		freed += do_shrink_slab(&sc, shrinker, nr_scanned, nr_eligible);
	}

	up_read(&shrinker_rwsem);
out:
	cond_resched();
	return freed;
}

void drop_slab_node(int nid)
{
	unsigned long freed;

	do {
		struct mem_cgroup *memcg = NULL;

		freed = 0;
		do {
			freed += shrink_slab(GFP_KERNEL, nid, memcg,
					     1000, 1000);
		} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
	} while (freed > 10);
}

void drop_slab(void)
{
	int nid;

	for_each_online_node(nid)
		drop_slab_node(nid);
}

static inline int is_page_cache_freeable(struct page *page)
{
	/*
	 * A freeable page cache page is referenced only by the caller
	 * that isolated the page, the page cache radix tree and
	 * optional buffer heads at page->private.
	 */
	return page_count(page) - page_has_private(page) == 2;
}

static int may_write_to_inode(struct inode *inode, struct scan_control *sc)
{
	if (current->flags & PF_SWAPWRITE)
		return 1;
	if (!inode_write_congested(inode))
		return 1;
	if (inode_to_bdi(inode) == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page(page);
	if (page_mapping(page) == mapping)
		mapping_set_error(mapping, error);
	unlock_page(page);
}

/* possible outcome of pageout() */
typedef enum {
	/* failed to write page out, page is locked */
	PAGE_KEEP,
	/* move page to the active list, page is locked */
	PAGE_ACTIVATE,
	/* page has been sent to the disk successfully, page is unlocked */
	PAGE_SUCCESS,
	/* page is clean and locked */
	PAGE_CLEAN,
} pageout_t;

/*
 * pageout is called by shrink_page_list() for each dirty page.
 * Calls ->writepage().
 */
static pageout_t pageout(struct page *page, struct address_space *mapping,
			 struct scan_control *sc)
{
	/*
	 * If the page is dirty, only perform writeback if that write
	 * will be non-blocking.  To prevent this allocation from being
	 * stalled by pagecache activity.  But note that there may be
	 * stalls if we need to run get_block().  We could test
	 * PagePrivate for that.
	 *
	 * If this process is currently in __generic_file_write_iter() against
	 * this page's queue, we can perform writeback even if that
	 * will block.
	 *
	 * If the page is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 */
	if (!is_page_cache_freeable(page))
		return PAGE_KEEP;
	if (!mapping) {
		/*
		 * Some data journaling orphaned pages can have
		 * page->mapping == NULL while being dirty with clean buffers.
		 */
		if (page_has_private(page)) {
			if (try_to_free_buffers(page)) {
				ClearPageDirty(page);
				pr_info("%s: orphaned page\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;
	if (!may_write_to_inode(mapping->host, sc))
		return PAGE_KEEP;

	if (clear_page_dirty_for_io(page)) {
		int res;
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
		};

		SetPageReclaim(page);
		res = mapping->a_ops->writepage(page, &wbc);
		if (res < 0)
			handle_write_error(mapping, page, res);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			ClearPageReclaim(page);
			return PAGE_ACTIVATE;
		}

		if (!PageWriteback(page)) {
			/* synchronous write or broken a_ops? */
			ClearPageReclaim(page);
		}
		trace_mm_vmscan_writepage(page, trace_reclaim_flags(page));
		inc_zone_page_state(page, NR_VMSCAN_WRITE);
		return PAGE_SUCCESS;
	}

	return PAGE_CLEAN;
}

/*
 * Same as remove_mapping, but if the page is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
static int __remove_mapping(struct address_space *mapping, struct page *page,
			    bool reclaimed)
{
	unsigned long flags;
	struct mem_cgroup *memcg;

	BUG_ON(!PageLocked(page));
	BUG_ON(mapping != page_mapping(page));

	memcg = mem_cgroup_begin_page_stat(page);
	spin_lock_irqsave(&mapping->tree_lock, flags);
	/*
	 * The non racy check for a busy page.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the page, it may be possible that they dirty it then
	 * drop the reference. So if PageDirty is tested before page_count
	 * here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!PageDirty(page)    [good]
	 * SetPageDirty(page);
	 * put_page(page);
	 *				!page_count(page)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the page->flags
	 * load is not satisfied before that of page->_count.
	 *
	 * Note that if SetPageDirty is always performed via set_page_dirty,
	 * and thus under tree_lock, then this ordering is not required.
	 */
	if (!page_freeze_refs(page, 2))
		goto cannot_free;
	/* note: atomic_cmpxchg in page_freeze_refs provides the smp_rmb */
	if (unlikely(PageDirty(page))) {
		page_unfreeze_refs(page, 2);
		goto cannot_free;
	}

	if (PageSwapCache(page)) {
		swp_entry_t swap = { .val = page_private(page) };
		mem_cgroup_swapout(page, swap);
		__delete_from_swap_cache(page);
		spin_unlock_irqrestore(&mapping->tree_lock, flags);
		mem_cgroup_end_page_stat(memcg);
		swapcache_free(swap);
	} else {
		void (*freepage)(struct page *);
		void *shadow = NULL;

		freepage = mapping->a_ops->freepage;
		/*
		 * Remember a shadow entry for reclaimed file cache in
		 * order to detect refaults, thus thrashing, later on.
		 *
		 * But don't store shadows in an address space that is
		 * already exiting.  This is not just an optizimation,
		 * inode reclaim needs to empty out the radix tree or
		 * the nodes are lost.  Don't plant shadows behind its
		 * back.
		 */
		if (reclaimed && page_is_file_cache(page) &&
		    !mapping_exiting(mapping))
			shadow = workingset_eviction(mapping, page);
		__delete_from_page_cache(page, shadow, memcg);
		spin_unlock_irqrestore(&mapping->tree_lock, flags);
		mem_cgroup_end_page_stat(memcg);

		if (freepage != NULL)
			freepage(page);
	}

	return 1;

cannot_free:
	spin_unlock_irqrestore(&mapping->tree_lock, flags);
	mem_cgroup_end_page_stat(memcg);
	return 0;
}

/*
 * Attempt to detach a locked page from its ->mapping.  If it is dirty or if
 * someone else has a ref on the page, abort and return 0.  If it was
 * successfully detached, return 1.  Assumes the caller has a single ref on
 * this page.
 */
int remove_mapping(struct address_space *mapping, struct page *page)
{
	if (__remove_mapping(mapping, page, false)) {
		/*
		 * Unfreezing the refcount with 1 rather than 2 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		page_unfreeze_refs(page, 1);
		return 1;
	}
	return 0;
}

/**
 * putback_lru_page - put previously isolated page onto appropriate LRU list
 * @page: page to be put back to appropriate lru list
 *
 * Add previously isolated @page to appropriate LRU list.
 * Page may still be unevictable for other reasons.
 *
 * lru_lock must not be held, interrupts must be enabled.
 */
void putback_lru_page(struct page *page)
{
	bool is_unevictable;
	int was_unevictable = PageUnevictable(page);

	VM_BUG_ON_PAGE(PageLRU(page), page);

redo:
	ClearPageUnevictable(page);

	if (page_evictable(page)) {
		/*
		 * For evictable pages, we can use the cache.
		 * In event of a race, worst case is we end up with an
		 * unevictable page on [in]active list.
		 * We know how to handle that.
		 */
		is_unevictable = false;
		lru_cache_add(page);
	} else {
		/*
		 * Put unevictable pages directly on zone's unevictable
		 * list.
		 */
		is_unevictable = true;
		add_page_to_unevictable_list(page);
		/*
		 * When racing with an mlock or AS_UNEVICTABLE clearing
		 * (page is unlocked) make sure that if the other thread
		 * does not observe our setting of PG_lru and fails
		 * isolation/check_move_unevictable_pages,
		 * we see PG_mlocked/AS_UNEVICTABLE cleared below and move
		 * the page back to the evictable list.
		 *
		 * The other side is TestClearPageMlocked() or shmem_lock().
		 */
		smp_mb();
	}

	/*
	 * page's status can change while we move it among lru. If an evictable
	 * page is on unevictable list, it never be freed. To avoid that,
	 * check after we added it to the list, again.
	 */
	if (is_unevictable && page_evictable(page)) {
		if (!isolate_lru_page(page)) {
			put_page(page);
			goto redo;
		}
		/* This means someone else dropped this page from LRU
		 * So, it will be freed or putback to LRU again. There is
		 * nothing to do here.
		 */
	}

	if (was_unevictable && !is_unevictable)
		count_vm_event(UNEVICTABLE_PGRESCUED);
	else if (!was_unevictable && is_unevictable)
		count_vm_event(UNEVICTABLE_PGCULLED);

	put_page(page);		/* drop ref from isolate */
}

enum page_references {
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

static enum page_references page_check_references(struct page *page,
						  struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);

	/*
	 * Mlock lost the isolation race with us.  Let try_to_unmap()
	 * move the page to the unevictable list.
	 */
	if (vm_flags & VM_LOCKED)
		return PAGEREF_RECLAIM;

	if (referenced_ptes) {
		if (PageSwapBacked(page))
			return PAGEREF_ACTIVATE;
		/*
		 * All mapped pages start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file page is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated pages as well
		 * so that recently deactivated but used pages are
		 * quickly recovered.
		 */
		SetPageReferenced(page);

		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		/*
		 * Activate file-backed executable pages after first usage.
		 */
		if (vm_flags & VM_EXEC)
			return PAGEREF_ACTIVATE;

		return PAGEREF_KEEP;
	}

	/* Reclaim if clean, defer dirty pages to writeback */
	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
}

/* Check if a page is dirty or under writeback */
static void page_check_dirty_writeback(struct page *page,
				       bool *dirty, bool *writeback)
{
	struct address_space *mapping;

	/*
	 * Anonymous pages are not handled by flushers and must be written
	 * from reclaim context. Do not stall reclaim based on them
	 */
	if (!page_is_file_cache(page)) {
		*dirty = false;
		*writeback = false;
		return;
	}

	/* By default assume that the page flags are accurate */
	*dirty = PageDirty(page);
	*writeback = PageWriteback(page);

	/* Verify dirty/writeback state if the filesystem supports it */
	if (!page_has_private(page))
		return;

	mapping = page_mapping(page);
	if (mapping && mapping->a_ops->is_dirty_writeback)
		mapping->a_ops->is_dirty_writeback(page, dirty, writeback);
}

/*
 * shrink_page_list() returns the number of reclaimed pages
 */
static unsigned long shrink_page_list(struct list_head *page_list,
				      struct pglist_data *pgdat,
				      struct scan_control *sc,
				      enum ttu_flags ttu_flags,
				      unsigned long *ret_nr_dirty,
				      unsigned long *ret_nr_unqueued_dirty,
				      unsigned long *ret_nr_congested,
				      unsigned long *ret_nr_writeback,
				      unsigned long *ret_nr_immediate,
				      bool force_reclaim)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	int pgactivate = 0;
	unsigned long nr_unqueued_dirty = 0;
	unsigned long nr_dirty = 0;
	unsigned long nr_congested = 0;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_writeback = 0;
	unsigned long nr_immediate = 0;

	cond_resched();

	while (!list_empty(page_list)) {
		struct address_space *mapping;
		struct page *page;
		int may_enter_fs;
		enum page_references references = PAGEREF_RECLAIM;
		bool dirty, writeback;

		cond_resched();

		page = lru_to_page(page_list);
		list_del(&page->lru);

		if (!trylock_page(page))
			goto keep;

		VM_BUG_ON_PAGE(PageActive(page), page);

		sc->nr_scanned++;

		if (unlikely(!page_evictable(page)))
			goto cull_mlocked;

		if (!sc->may_unmap && page_mapped(page))
			goto keep_locked;

		/* Double the slab pressure for mapped and swapcache pages */
		if (page_mapped(page) || PageSwapCache(page))
			sc->nr_scanned++;

		may_enter_fs = (sc->gfp_mask & __GFP_FS) ||
			(PageSwapCache(page) && (sc->gfp_mask & __GFP_IO));

		/*
		 * The number of dirty pages determines if a zone is marked
		 * reclaim_congested which affects wait_iff_congested. kswapd
		 * will stall and start writing pages if the tail of the LRU
		 * is all dirty unqueued pages.
		 */
		page_check_dirty_writeback(page, &dirty, &writeback);
		if (dirty || writeback)
			nr_dirty++;

		if (dirty && !writeback)
			nr_unqueued_dirty++;

		/*
		 * Treat this page as congested if the underlying BDI is or if
		 * pages are cycling through the LRU so quickly that the
		 * pages marked for immediate reclaim are making it to the
		 * end of the LRU a second time.
		 */
		mapping = page_mapping(page);
		if (((dirty || writeback) && mapping &&
		     inode_write_congested(mapping->host)) ||
		    (writeback && PageReclaim(page)))
			nr_congested++;

		/*
		 * If a page at the tail of the LRU is under writeback, there
		 * are three cases to consider.
		 *
		 * 1) If reclaim is encountering an excessive number of pages
		 *    under writeback and this page is both under writeback and
		 *    PageReclaim then it indicates that pages are being queued
		 *    for IO but are being recycled through the LRU before the
		 *    IO can complete. Waiting on the page itself risks an
		 *    indefinite stall if it is impossible to writeback the
		 *    page due to IO error or disconnected storage so instead
		 *    note that the LRU is being scanned too quickly and the
		 *    caller can stall after page list has been processed.
		 *
		 * 2) Global or new memcg reclaim encounters a page that is
		 *    not marked for immediate reclaim, or the caller does not
		 *    have __GFP_FS (or __GFP_IO if it's simply going to swap,
		 *    not to fs). In this case mark the page for immediate
		 *    reclaim and continue scanning.
		 *
		 *    Require may_enter_fs because we would wait on fs, which
		 *    may not have submitted IO yet. And the loop driver might
		 *    enter reclaim, and deadlock if it waits on a page for
		 *    which it is needed to do the write (loop masks off
		 *    __GFP_IO|__GFP_FS for this reason); but more thought
		 *    would probably show more reasons.
		 *
		 * 3) Legacy memcg encounters a page that is already marked
		 *    PageReclaim. memcg does not have any dirty pages
		 *    throttling so we could easily OOM just because too many
		 *    pages are in writeback and there is nothing else to
		 *    reclaim. Wait for the writeback to complete.
		 */
		if (PageWriteback(page)) {
			/* Case 1 above */
			if (current_is_kswapd() &&
			    PageReclaim(page) &&
			    test_bit(PGDAT_WRITEBACK, &pgdat->flags)) {
				nr_immediate++;
				goto keep_locked;

			/* Case 2 above */
			} else if (sane_reclaim(sc) ||
			    !PageReclaim(page) || !may_enter_fs) {
				/*
				 * This is slightly racy - end_page_writeback()
				 * might have just cleared PageReclaim, then
				 * setting PageReclaim here end up interpreted
				 * as PageReadahead - but that does not matter
				 * enough to care.  What we do want is for this
				 * page to have PageReclaim set next time memcg
				 * reclaim reaches the tests above, so it will
				 * then wait_on_page_writeback() to avoid OOM;
				 * and it's also appropriate in global reclaim.
				 */
				SetPageReclaim(page);
				nr_writeback++;
				goto keep_locked;

			/* Case 3 above */
			} else {
				unlock_page(page);
				wait_on_page_writeback(page);
				/* then go back and try same page again */
				list_add_tail(&page->lru, page_list);
				continue;
			}
		}

		if (!force_reclaim)
			references = page_check_references(page, sc);

		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim the page below */
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 */
		if (PageAnon(page) && !PageSwapCache(page)) {
			if (!(sc->gfp_mask & __GFP_IO))
				goto keep_locked;
			if (!add_to_swap(page, page_list))
				goto activate_locked;
			may_enter_fs = 1;

			/* Adding to swap updated mapping */
			mapping = page_mapping(page);
		}

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (page_mapped(page) && mapping) {
			switch (try_to_unmap(page,
					ttu_flags|TTU_BATCH_FLUSH,
					sc->target_vma)) {
			case SWAP_FAIL:
				goto activate_locked;
			case SWAP_AGAIN:
				goto keep_locked;
			case SWAP_MLOCK:
				goto cull_mlocked;
			case SWAP_SUCCESS:
				; /* try to free the page below */
			}
		}

		if (PageDirty(page)) {
			/*
			 * Only kswapd can writeback filesystem pages to
			 * avoid risk of stack overflow but only writeback
			 * if many dirty pages have been encountered.
			 */
			if (page_is_file_cache(page) &&
					(!current_is_kswapd() ||
					 !test_bit(PGDAT_DIRTY, &pgdat->flags))) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principal to deactivate_page()
				 * except we already have the page isolated
				 * and know it's dirty
				 */
				inc_zone_page_state(page, NR_VMSCAN_IMMEDIATE);
				SetPageReclaim(page);

				goto keep_locked;
			}

			if (references == PAGEREF_RECLAIM_CLEAN)
				goto keep_locked;
			if (!may_enter_fs)
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/*
			 * Page is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after IO
			 * starts and then write it out here.
			 */
			try_to_unmap_flush_dirty();
			switch (pageout(page, mapping, sc)) {
			case PAGE_KEEP:
				goto keep_locked;
			case PAGE_ACTIVATE:
				goto activate_locked;
			case PAGE_SUCCESS:
				if (PageWriteback(page))
					goto keep;
				if (PageDirty(page))
					goto keep;

				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the page.
				 */
				if (!trylock_page(page))
					goto keep;
				if (PageDirty(page) || PageWriteback(page))
					goto keep_locked;
				mapping = page_mapping(page);
			case PAGE_CLEAN:
				; /* try to free the page below */
			}
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping.
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_complete_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 1) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */
		if (page_has_private(page)) {
			if (!try_to_release_page(page, sc->gfp_mask))
				goto activate_locked;
			if (!mapping && page_count(page) == 1) {
				unlock_page(page);
				if (put_page_testzero(page))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this page shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					nr_reclaimed++;
					continue;
				}
			}
		}

		if (!mapping || !__remove_mapping(mapping, page, true))
			goto keep_locked;

		/*
		 * At this point, we have no other references and there is
		 * no way to pick any more up (removed from LRU, removed
		 * from pagecache). Can use non-atomic bitops now (and
		 * we obviously don't have to worry about waking up a process
		 * waiting on the page lock, because there are no references.
		 */
		__ClearPageLocked(page);
free_it:
		nr_reclaimed++;

		/*
		 * Is there need to periodically free_page_list? It would
		 * appear not as the counts should be low
		 */
		list_add(&page->lru, &free_pages);
		/*
		 * If pagelist are from multiple zones, we should decrease
		 * NR_ISOLATED_ANON + x on freed pages in here.
		 */
		if (!pgdat)
			dec_node_page_state(page, NR_ISOLATED_ANON +
					page_is_file_cache(page));
		continue;

cull_mlocked:
		if (PageSwapCache(page))
			try_to_free_swap(page);
		unlock_page(page);
		list_add(&page->lru, &ret_pages);
		continue;

activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		if (PageSwapCache(page) && vm_swap_full(page_swap_info(page)))
			try_to_free_swap(page);
		VM_BUG_ON_PAGE(PageActive(page), page);
		SetPageActive(page);
		pgactivate++;
keep_locked:
		unlock_page(page);
keep:
		list_add(&page->lru, &ret_pages);
		VM_BUG_ON_PAGE(PageLRU(page) || PageUnevictable(page), page);
	}

	mem_cgroup_uncharge_list(&free_pages);
	try_to_unmap_flush();
	free_hot_cold_page_list(&free_pages, true);

	list_splice(&ret_pages, page_list);
	count_vm_events(PGACTIVATE, pgactivate);

	*ret_nr_dirty += nr_dirty;
	*ret_nr_congested += nr_congested;
	*ret_nr_unqueued_dirty += nr_unqueued_dirty;
	*ret_nr_writeback += nr_writeback;
	*ret_nr_immediate += nr_immediate;
	return nr_reclaimed;
}

unsigned long reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *page_list)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.priority = DEF_PRIORITY,
		.may_unmap = 1,
		/* Doesn't allow to write out dirty page */
		.may_writepage = 0,
	};
	unsigned long ret, dummy1, dummy2, dummy3, dummy4, dummy5;
	struct page *page, *next;
	LIST_HEAD(clean_pages);

	list_for_each_entry_safe(page, next, page_list, lru) {
		if (page_is_file_cache(page) && !PageDirty(page) &&
		    !__PageMovable(page)) {
			ClearPageActive(page);
			list_move(&page->lru, &clean_pages);
		}
	}

	ret = shrink_page_list(&clean_pages, zone->zone_pgdat, &sc,
			TTU_UNMAP|TTU_IGNORE_ACCESS,
			&dummy1, &dummy2, &dummy3, &dummy4, &dummy5, true);
	list_splice(&clean_pages, page_list);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE, -ret);
	return ret;
}

#ifdef CONFIG_PROCESS_RECLAIM
unsigned long reclaim_pages_from_list(struct list_head *page_list,
					struct vm_area_struct *vma)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.target_vma = vma,
	};

	unsigned long nr_reclaimed;
	struct page *page;
	unsigned long dummy1, dummy2, dummy3, dummy4, dummy5;

	list_for_each_entry(page, page_list, lru)
		ClearPageActive(page);

	nr_reclaimed = shrink_page_list(page_list, NULL, &sc,
			TTU_UNMAP|TTU_IGNORE_ACCESS,
			&dummy1, &dummy2, &dummy3, &dummy4, &dummy5, true);

	while (!list_empty(page_list)) {
		page = lru_to_page(page_list);
		list_del(&page->lru);
		dec_node_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		putback_lru_page(page);
	}

	return nr_reclaimed;
}
#endif

/*
 * Attempt to remove the specified page from its LRU.  Only take this page
 * if it is of the appropriate PageActive status.  Pages which are being
 * freed elsewhere are also ignored.
 *
 * page:	page to consider
 * mode:	one of the LRU isolation modes defined above
 *
 * returns 0 on success, -ve errno on failure.
 */
int __isolate_lru_page(struct page *page, isolate_mode_t mode)
{
	int ret = -EINVAL;

	/* Only take pages on the LRU. */
	if (!PageLRU(page))
		return ret;

	/* Compaction should not handle unevictable pages but CMA can do so */
	if (PageUnevictable(page) && !(mode & ISOLATE_UNEVICTABLE))
		return ret;

	ret = -EBUSY;

	/*
	 * To minimise LRU disruption, the caller can indicate that it only
	 * wants to isolate pages it will be able to operate on without
	 * blocking - clean pages for the most part.
	 *
	 * ISOLATE_CLEAN means that only clean pages should be isolated. This
	 * is used by reclaim when it is cannot write to backing storage
	 *
	 * ISOLATE_ASYNC_MIGRATE is used to indicate that it only wants to pages
	 * that it is possible to migrate without blocking
	 */
	if (mode & (ISOLATE_CLEAN|ISOLATE_ASYNC_MIGRATE)) {
		/* All the caller can do on PageWriteback is block */
		if (PageWriteback(page))
			return ret;

		if (PageDirty(page)) {
			struct address_space *mapping;
			bool migrate_dirty;

			/* ISOLATE_CLEAN means only clean pages */
			if (mode & ISOLATE_CLEAN)
				return ret;

			/*
			 * Only pages without mappings or that have a
			 * ->migratepage callback are possible to migrate
			 * without blocking. However, we can be racing with
			 * truncation so it's necessary to lock the page
			 * to stabilise the mapping as truncation holds
			 * the page lock until after the page is removed
			 * from the page cache.
			 */
			if (!trylock_page(page))
				return ret;

			mapping = page_mapping(page);
			migrate_dirty = !mapping || mapping->a_ops->migratepage;
			unlock_page(page);
			if (!migrate_dirty)
				return ret;
		}
	}

	if ((mode & ISOLATE_UNMAPPED) && page_mapped(page))
		return ret;

	if (likely(get_page_unless_zero(page))) {
		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
		ClearPageLRU(page);
		ret = 0;
	}

	return ret;
}

/*
 * zone_lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Appropriate locks must be held before calling this function.
 *
 * @nr_to_scan:	The number of pages to look through on the list.
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @mode:	One of the LRU isolation modes
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		isolate_mode_t mode, enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
	unsigned long scan, nr_pages;
	LIST_HEAD(pages_skipped);

	for (scan = 0; scan < nr_to_scan && nr_taken < nr_to_scan &&
					!list_empty(src); scan++) {
		struct page *page;
		int nr_pages;

		page = lru_to_page(src);
		prefetchw_prev_lru_page(page, src, flags);

		VM_BUG_ON_PAGE(!PageLRU(page), page);

		if (page_zonenum(page) > sc->reclaim_idx) {
			list_move(&page->lru, &pages_skipped);
			continue;
		}

		switch (__isolate_lru_page(page, mode)) {
		case 0:
			nr_pages = hpage_nr_pages(page);
			nr_taken += nr_pages;
			nr_zone_taken[page_zonenum(page)] += nr_pages;
			list_move(&page->lru, dst);
			break;

		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, src);
			continue;

		default:
			BUG();
		}
	}

	/*
	 * Splice any skipped pages to the start of the LRU list. Note that
	 * this disrupts the LRU order when reclaiming for lower zones but
	 * we cannot splice to the tail. If we did then the SWAP_CLUSTER_MAX
	 * scanning would soon rescan the same pages to skip and put the
	 * system at risk of premature OOM.
	 */
	if (!list_empty(&pages_skipped))
		list_splice(&pages_skipped, src);
	*nr_scanned = scan;
	trace_mm_vmscan_lru_isolate(sc->order, nr_to_scan, scan,
				    nr_taken, mode, is_file_lru(lru));
	for (scan = 0; scan < MAX_NR_ZONES; scan++) {
		nr_pages = nr_zone_taken[scan];
		if (!nr_pages)
			continue;

		update_lru_size(lruvec, lru, scan, -nr_pages);
	}
	return nr_taken;
}

/**
 * isolate_lru_page - tries to isolate a page from its LRU list
 * @page: page to isolate from its LRU list
 *
 * Isolates a @page from an LRU list, clears PageLRU and adjusts the
 * vmstat statistic corresponding to whatever LRU list the page was on.
 *
 * Returns 0 if the page was removed from an LRU list.
 * Returns -EBUSY if the page was not on an LRU list.
 *
 * The returned page will have PageLRU() cleared.  If it was found on
 * the active list, it will have PageActive set.  If it was found on
 * the unevictable list, it will have the PageUnevictable bit set. That flag
 * may need to be cleared by the caller before letting the page go.
 *
 * The vmstat statistic corresponding to the list on which the page was
 * found will be decremented.
 *
 * Restrictions:
 * (1) Must be called with an elevated refcount on the page. This is a
 *     fundamentnal difference from isolate_lru_pages (which is called
 *     without a stable reference).
 * (2) the lru_lock must not be held.
 * (3) interrupts must be enabled.
 */
int isolate_lru_page(struct page *page)
{
	int ret = -EBUSY;

	VM_BUG_ON_PAGE(!page_count(page), page);

	if (PageLRU(page)) {
		struct zone *zone = page_zone(page);
		struct lruvec *lruvec;

		spin_lock_irq(zone_lru_lock(zone));
		lruvec = mem_cgroup_page_lruvec(page, zone->zone_pgdat);
		if (PageLRU(page)) {
			int lru = page_lru(page);
			get_page(page);
			ClearPageLRU(page);
			del_page_from_lru_list(page, lruvec, lru);
			ret = 0;
		}
		spin_unlock_irq(zone_lru_lock(zone));
	}
	return ret;
}

static int too_many_isolated(struct pglist_data *pgdat, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!sane_reclaim(sc))
		return 0;

	if (file) {
		inactive = node_page_state_pgdat(pgdat, NR_INACTIVE_FILE);
		isolated = node_page_state_pgdat(pgdat, NR_ISOLATED_FILE);
	} else {
		inactive = node_page_state_pgdat(pgdat, NR_INACTIVE_ANON);
		isolated = node_page_state_pgdat(pgdat, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so
	 * they won't get blocked by normal direct-reclaimers.
	 */
	if ((sc->gfp_mask & (__GFP_IO | __GFP_FS)) == (__GFP_IO | __GFP_FS))
		inactive >>= 3;

	return isolated > inactive;
}

static noinline_for_stack void
putback_inactive_pages(struct lruvec *lruvec, struct list_head *page_list)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	LIST_HEAD(pages_to_free);

	/*
	 * Put back any unfreeable pages.
	 */
	while (!list_empty(page_list)) {
		struct page *page = lru_to_page(page_list);
		int lru;
		int file;

		VM_BUG_ON_PAGE(PageLRU(page), page);
		list_del(&page->lru);
		if (unlikely(!page_evictable(page))) {
			spin_unlock_irq(&pgdat->lru_lock);
			putback_lru_page(page);
			spin_lock_irq(&pgdat->lru_lock);
			continue;
		}

		lruvec = mem_cgroup_page_lruvec(page, pgdat);

		SetPageLRU(page);
		lru = page_lru(page);
		add_page_to_lru_list(page, lruvec, lru);

		file = is_file_lru(lru);
		if (IS_ENABLED(CONFIG_ZCACHE))
			if (file)
				SetPageWasActive(page);
		if (is_active_lru(lru)) {
			int numpages = hpage_nr_pages(page);
			reclaim_stat->recent_rotated[file] += numpages;
		}
		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&pgdat->lru_lock);
				mem_cgroup_uncharge(page);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&pgdat->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);
		}
	}

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
	list_splice(&pages_to_free, page_list);
}

/*
 * If a kernel thread (such as nfsd for loop-back mounts) services
 * a backing device by writing to the page cache it sets PF_LESS_THROTTLE.
 * In that case we should only throttle if the backing device it is
 * writing to is congested.  In other cases it is safe to throttle.
 */
static int current_may_throttle(void)
{
	return !(current->flags & PF_LESS_THROTTLE) ||
		current->backing_dev_info == NULL ||
		bdi_write_congested(current->backing_dev_info);
}

/*
 * shrink_inactive_list() is a helper for shrink_node().  It returns the number
 * of reclaimed pages
 */
static noinline_for_stack unsigned long
shrink_inactive_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		     struct scan_control *sc, enum lru_list lru)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_congested = 0;
	unsigned long nr_unqueued_dirty = 0;
	unsigned long nr_writeback = 0;
	unsigned long nr_immediate = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;

	while (unlikely(too_many_isolated(pgdat, file, sc))) {
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&pgdat->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, isolate_mode, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	reclaim_stat->recent_scanned[file] += nr_taken;

	if (global_reclaim(sc)) {
		__mod_node_page_state(pgdat, NR_PAGES_SCANNED, nr_scanned);
		if (current_is_kswapd())
			__count_vm_events(PGSCAN_KSWAPD, nr_scanned);
		else
			__count_vm_events(PGSCAN_DIRECT, nr_scanned);
	}
	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	if (nr_taken == 0)
		return 0;

	nr_reclaimed = shrink_page_list(&page_list, pgdat, sc, TTU_UNMAP,
				&nr_dirty, &nr_unqueued_dirty, &nr_congested,
				&nr_writeback, &nr_immediate,
				false);

	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	if (global_reclaim(sc)) {
		if (current_is_kswapd())
			__count_vm_events(PGSTEAL_KSWAPD, nr_reclaimed);
		else
			__count_vm_events(PGSTEAL_DIRECT, nr_reclaimed);
	}

	putback_inactive_pages(lruvec, &page_list);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);

	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	mem_cgroup_uncharge_list(&page_list);
	free_hot_cold_page_list(&page_list, true);

	/*
	 * If reclaim is isolating dirty pages under writeback, it implies
	 * that the long-lived page allocation rate is exceeding the page
	 * laundering rate. Either the global limits are not being effective
	 * at throttling processes due to the page distribution throughout
	 * zones or there is heavy usage of a slow backing device. The
	 * only option is to throttle from reclaim context which is not ideal
	 * as there is no guarantee the dirtying process is throttled in the
	 * same way balance_dirty_pages() manages.
	 *
	 * Once a zone is flagged ZONE_WRITEBACK, kswapd will count the number
	 * of pages under pages flagged for immediate reclaim and stall if any
	 * are encountered in the nr_immediate check below.
	 */
	if (nr_writeback && nr_writeback == nr_taken)
		set_bit(PGDAT_WRITEBACK, &pgdat->flags);

	/*
	 * Legacy memcg will stall in page writeback so avoid forcibly
	 * stalling here.
	 */
	if (sane_reclaim(sc)) {
		/*
		 * Tag a zone as congested if all the dirty pages scanned were
		 * backed by a congested BDI and wait_iff_congested will stall.
		 */
		if (nr_dirty && nr_dirty == nr_congested)
			set_bit(PGDAT_CONGESTED, &pgdat->flags);

		/*
		 * If dirty pages are scanned that are not queued for IO, it
		 * implies that flushers are not keeping up. In this case, flag
		 * the pgdat PGDAT_DIRTY and kswapd will start writing pages from
		 * reclaim context.
		 */
		if (nr_unqueued_dirty == nr_taken)
			set_bit(PGDAT_DIRTY, &pgdat->flags);

		/*
		 * If kswapd scans pages marked marked for immediate
		 * reclaim and under writeback (nr_immediate), it implies
		 * that pages are cycling through the LRU faster than
		 * they are written so also forcibly stall.
		 */
		if (nr_immediate && current_may_throttle())
			congestion_wait(BLK_RW_ASYNC, HZ/10);
	}

	/*
	 * Stall direct reclaim for IO completions if underlying BDIs or zone
	 * is congested. Allow kswapd to continue until it starts encountering
	 * unqueued dirty pages or cycling through the LRU too quickly.
	 */
	if (!sc->hibernation_mode && !current_is_kswapd() &&
	    current_may_throttle())
		wait_iff_congested(pgdat, BLK_RW_ASYNC, HZ/10);

	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			nr_scanned, nr_reclaimed,
			sc->priority, file);
	return nr_reclaimed;
}

/*
 * This moves pages from the active list to the inactive list.
 *
 * We move them the other way if the page is referenced by one or more
 * processes, from rmap.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold zone_lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()) so we
 * should drop zone_lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_count against each page.
 * But we had to alter page->flags anyway.
 */

static void move_active_pages_to_lru(struct lruvec *lruvec,
				     struct list_head *list,
				     struct list_head *pages_to_free,
				     enum lru_list lru)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	unsigned long pgmoved = 0;
	struct page *page;
	int nr_pages;

	while (!list_empty(list)) {
		page = lru_to_page(list);
		lruvec = mem_cgroup_page_lruvec(page, pgdat);

		VM_BUG_ON_PAGE(PageLRU(page), page);
		SetPageLRU(page);

		nr_pages = hpage_nr_pages(page);
		update_lru_size(lruvec, lru, page_zonenum(page), nr_pages);
		list_move(&page->lru, &lruvec->lists[lru]);
		pgmoved += nr_pages;

		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&pgdat->lru_lock);
				mem_cgroup_uncharge(page);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&pgdat->lru_lock);
			} else
				list_add(&page->lru, pages_to_free);
		}
	}
	if (!is_active_lru(lru))
		__count_vm_events(PGDEACTIVATE, pgmoved);
}

static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	unsigned long nr_rotated = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, isolate_mode, lru);
	if (global_reclaim(sc))
		__mod_node_page_state(pgdat, NR_PAGES_SCANNED, nr_scanned);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	reclaim_stat->recent_scanned[file] += nr_taken;

	__count_vm_events(PGREFILL, nr_scanned);

	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		if (unlikely(!page_evictable(page))) {
			putback_lru_page(page);
			continue;
		}

		if (unlikely(buffer_heads_over_limit)) {
			if (page_has_private(page) && trylock_page(page)) {
				if (page_has_private(page))
					try_to_release_page(page, 0);
				unlock_page(page);
			}
		}

		if (page_referenced(page, 0, sc->target_mem_cgroup,
				    &vm_flags)) {
			nr_rotated += hpage_nr_pages(page);
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && page_is_file_cache(page)) {
				list_add(&page->lru, &l_active);
				continue;
			}
		}

		ClearPageActive(page);	/* we are de-activating */
		if (IS_ENABLED(CONFIG_ZCACHE))
			/*
			 * For zcache to know whether the page is from active
			 * file list
			 */
			SetPageWasActive(page);
		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);
	/*
	 * Count referenced pages from currently used mappings as rotated,
	 * even though only some of them are actually re-activated.  This
	 * helps balance scan pressure between file and anonymous pages in
	 * get_scan_count.
	 */
	reclaim_stat->recent_rotated[file] += nr_rotated;

	move_active_pages_to_lru(lruvec, &l_active, &l_hold, lru);
	move_active_pages_to_lru(lruvec, &l_inactive, &l_hold, lru - LRU_ACTIVE);
	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&pgdat->lru_lock);

	mem_cgroup_uncharge_list(&l_hold);
	free_hot_cold_page_list(&l_hold, true);
}

#ifdef CONFIG_SWAP
static bool inactive_anon_is_low_global(struct pglist_data *pgdat)
{
	unsigned long active, inactive;

	active = node_page_state_pgdat(pgdat, NR_ACTIVE_ANON);
	inactive = node_page_state_pgdat(pgdat, NR_INACTIVE_ANON);

	return inactive * pgdat->inactive_ratio < active;
}

/**
 * inactive_anon_is_low - check if anonymous pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * Returns true if the zone does not have enough inactive anon pages,
 * meaning some active anon pages need to be deactivated.
 */
static bool inactive_anon_is_low(struct lruvec *lruvec)
{
	/*
	 * If we don't have swap space, anonymous page deactivation
	 * is pointless.
	 */
	if (!total_swap_pages)
		return false;

	if (!mem_cgroup_disabled())
		return mem_cgroup_inactive_anon_is_low(lruvec);

	return inactive_anon_is_low_global(lruvec_pgdat(lruvec));
}
#else
static inline bool inactive_anon_is_low(struct lruvec *lruvec)
{
	return false;
}
#endif

/**
 * inactive_file_is_low - check if file pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * When the system is doing streaming IO, memory pressure here
 * ensures that active file pages get deactivated, until more
 * than half of the file pages are on the inactive list.
 *
 * Once we get to that situation, protect the system's working
 * set from being evicted by disabling active file page aging.
 *
 * This uses a different ratio than the anonymous pages, because
 * the page cache uses a use-once replacement algorithm.
 */
static bool inactive_file_is_low(struct lruvec *lruvec)
{
	unsigned long inactive;
	unsigned long active;

	inactive = get_lru_size(lruvec, LRU_INACTIVE_FILE);
	active = get_lru_size(lruvec, LRU_ACTIVE_FILE);

	return active > inactive;
}

static bool inactive_list_is_low(struct lruvec *lruvec, enum lru_list lru)
{
	if (is_file_lru(lru))
		return inactive_file_is_low(lruvec);
	else
		return inactive_anon_is_low(lruvec);
}

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	if (is_active_lru(lru)) {
		if (inactive_list_is_low(lruvec, lru))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * nr[0] = anon inactive pages to scan; nr[1] = anon active pages to scan
 * nr[2] = file inactive pages to scan; nr[3] = file active pages to scan
 */
static void get_scan_count(struct lruvec *lruvec, int swappiness,
			   struct scan_control *sc, unsigned long *nr,
			   unsigned long *lru_pages)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	u64 fraction[2];
	u64 denominator = 0;	/* gcc */
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	unsigned long anon_prio, file_prio;
	enum scan_balance scan_balance;
	unsigned long anon, file;
	bool force_scan = false;
	unsigned long ap, fp;
	enum lru_list lru;
	bool some_scanned;
	int pass;

	/*
	 * If the zone or memcg is small, nr[l] can be 0.  This
	 * results in no scanning on this priority and a potential
	 * priority drop.  Global direct reclaim can go to the next
	 * zone and tends to have no problems. Global kswapd is for
	 * zone balancing and it needs to scan a minimum amount. When
	 * reclaiming for a memcg, a priority drop can cause high
	 * latencies, so it's better to scan a minimum amount there as
	 * well.
	 */
	if (current_is_kswapd()) {
		if (!pgdat_reclaimable(pgdat))
			force_scan = true;
		if (!mem_cgroup_lruvec_online(lruvec))
			force_scan = true;
	}
	if (!global_reclaim(sc))
		force_scan = true;

	/* If we have no swap space, do not bother scanning anon pages. */
	if (!sc->may_swap || (get_nr_swap_pages() <= 0)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Global reclaim will swap to prevent OOM even with no
	 * swappiness, but memcg users want to use this knob to
	 * disable swapping for individual groups completely when
	 * using the memory controller's swap limit feature would be
	 * too expensive.
	 */
	if (!global_reclaim(sc) && !swappiness) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	/*
	 * Prevent the reclaimer from falling into the cache trap: as
	 * cache pages start out inactive, every cache fault will tip
	 * the scan balance towards the file LRU.  And as the file LRU
	 * shrinks, so does the window for rotation from references.
	 * This means we have a runaway feedback loop where a tiny
	 * thrashing file LRU becomes infinitely more attractive than
	 * anon pages.  Try to detect this based on file LRU size.
	 */
	if (global_reclaim(sc)) {
		unsigned long pgdatfile;
		unsigned long pgdatfree;
		int z;
		unsigned long total_high_wmark = 0;

		pgdatfree = sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
		pgdatfile = node_page_state_pgdat(pgdat, NR_ACTIVE_FILE) +
			   node_page_state_pgdat(pgdat, NR_INACTIVE_FILE);

		for (z = 0; z < MAX_NR_ZONES; z++) {
			struct zone *zone = &pgdat->node_zones[z];
			if (!populated_zone(zone))
				continue;

			total_high_wmark += high_wmark_pages(zone);
		}

		if (unlikely(pgdatfile + pgdatfree <= total_high_wmark)) {
			scan_balance = SCAN_ANON;
			goto out;
		}
	}

	/*
	 * If there is enough inactive page cache, i.e. if the size of the
	 * inactive list is greater than that of the active list *and* the
	 * inactive list actually has some pages to scan on this priority, we
	 * do not reclaim anything from the anonymous working set right now.
	 * Without the second condition we could end up never scanning an
	 * lruvec even if it has plenty of old anonymous pages unless the
	 * system is under heavy pressure.
	 */
	if (!IS_ENABLED(CONFIG_BALANCE_ANON_FILE_RECLAIM) &&
			!inactive_file_is_low(lruvec) &&
			get_lru_size(lruvec, LRU_INACTIVE_FILE) >> sc->priority) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;

	/*
	 * With swappiness at 100, anonymous and file have the same priority.
	 * This scanning priority is essentially the inverse of IO cost.
	 */
	anon_prio = swappiness;
	file_prio = 200 - anon_prio;

	/*
	 * OK, so we have swap space and a fair amount of page cache
	 * pages.  We use the recently rotated / recently scanned
	 * ratios to determine how valuable each cache is.
	 *
	 * Because workloads change over time (and to avoid overflow)
	 * we keep these statistics as a floating average, which ends
	 * up weighing recent references more than old ones.
	 *
	 * anon in [0], file in [1]
	 */

	anon  = get_lru_size(lruvec, LRU_ACTIVE_ANON) +
		get_lru_size(lruvec, LRU_INACTIVE_ANON);
	file  = get_lru_size(lruvec, LRU_ACTIVE_FILE) +
		get_lru_size(lruvec, LRU_INACTIVE_FILE);

	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);
	if (unlikely(reclaim_stat->recent_scanned[0] > anon / 4)) {
		reclaim_stat->recent_scanned[0] /= 2;
		reclaim_stat->recent_rotated[0] /= 2;
	}

	if (unlikely(reclaim_stat->recent_scanned[1] > file / 4)) {
		reclaim_stat->recent_scanned[1] /= 2;
		reclaim_stat->recent_rotated[1] /= 2;
	}

	/*
	 * The amount of pressure on anon vs file pages is inversely
	 * proportional to the fraction of recently scanned pages on
	 * each list that were recently referenced and in active use.
	 */
	ap = anon_prio * (reclaim_stat->recent_scanned[0] + 1);
	ap /= reclaim_stat->recent_rotated[0] + 1;

	fp = file_prio * (reclaim_stat->recent_scanned[1] + 1);
	fp /= reclaim_stat->recent_rotated[1] + 1;
	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp + 1;
out:
	some_scanned = false;
	/* Only use force_scan on second pass. */
	for (pass = 0; !some_scanned && pass < 2; pass++) {
		*lru_pages = 0;
		for_each_evictable_lru(lru) {
			int file = is_file_lru(lru);
			unsigned long size;
			unsigned long scan;

			size = get_lru_size(lruvec, lru);
			scan = size >> sc->priority;

			if (!scan && pass && force_scan)
				scan = min(size, SWAP_CLUSTER_MAX);

			switch (scan_balance) {
			case SCAN_EQUAL:
				/* Scan lists relative to size */
				break;
			case SCAN_FRACT:
				/*
				 * Scan types proportional to swappiness and
				 * their relative recent reclaim efficiency.
				 */
				scan = div64_u64(scan * fraction[file],
							denominator);
				break;
			case SCAN_FILE:
			case SCAN_ANON:
				/* Scan one type exclusively */
				if ((scan_balance == SCAN_FILE) != file) {
					size = 0;
					scan = 0;
				}
				break;
			default:
				/* Look ma, no brain */
				BUG();
			}

			*lru_pages += size;
			nr[lru] = scan;

			/*
			 * Skip the second pass and don't force_scan,
			 * if we found something to scan.
			 */
			some_scanned |= !!scan;
		}
	}
}

/*
 * lruvec_lru_size: pages on @lru for @lruvec, minus those in zones above
 * @zone_idx (4.9 semantics, adapted to 4.4 memcg/zone helpers).
 */
unsigned long lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru, int zone_idx)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	unsigned long lru_size;
	int zid;

	if (!mem_cgroup_disabled())
		lru_size = mem_cgroup_get_lru_size(lruvec, lru);
	else
		lru_size = node_page_state_pgdat(pgdat, NR_LRU_BASE + lru);

	for (zid = zone_idx + 1; zid < MAX_NR_ZONES; zid++) {
		struct zone *zone = &pgdat->node_zones[zid];
		unsigned long size;

		if (!zone->managed_pages)
			continue;

		size = zone_page_state(zone, NR_ZONE_LRU_BASE + !!is_file_lru(lru));
		lru_size -= min(size, lru_size);
	}

	return lru_size;
}

#ifdef CONFIG_LRU_GEN

/******************************************************************************
 *                          shorthand helpers
 ******************************************************************************/

#ifdef CONFIG_MEMCG
static inline struct mem_cgroup *lruvec_memcg_4_9(struct lruvec *lruvec)
{
	/*
	 * The node lruvec is not embedded in a mem_cgroup_per_node, so
	 * container_of() would hand back a bogus object and ->memcg would
	 * read whatever happens to sit at that offset.  mem_cgroup_lruvec()
	 * returns exactly that lruvec whenever the memory controller is
	 * disabled, and 4.4's shrink_node_memcg() then feeds it straight to
	 * lru_gen_shrink_lruvec().  The struct's own comment says the memcg
	 * back pointer exists because container_of() cannot be used here.
	 */
	if (lruvec == node_lruvec(lruvec_pgdat(lruvec)))
		return NULL;

	return container_of(lruvec, struct mem_cgroup_per_node, lruvec)->memcg;
}
#else
static inline struct mem_cgroup *lruvec_memcg_4_9(struct lruvec *lruvec)
{
	return NULL;
}
#endif

#define DEFINE_MAX_SEQ(lruvec)						\
	unsigned long max_seq = READ_ONCE((lruvec)->evictable.max_seq)

#define DEFINE_MIN_SEQ(lruvec)						\
	unsigned long min_seq[ANON_AND_FILE] = {			\
		READ_ONCE((lruvec)->evictable.min_seq[0]),		\
		READ_ONCE((lruvec)->evictable.min_seq[1]),		\
	}

#define for_each_gen_type_zone(gen, type, zone)				\
	for ((gen) = 0; (gen) < MAX_NR_GENS; (gen)++)			\
		for ((type) = 0; (type) < ANON_AND_FILE; (type)++)	\
			for ((zone) = 0; (zone) < MAX_NR_ZONES; (zone)++)

static int page_lru_gen(struct page *page)
{
	unsigned long flags = READ_ONCE(page->flags);

	return ((flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

static int page_lru_tier(struct page *page)
{
	int refs;
	unsigned long flags = READ_ONCE(page->flags);

	refs = (flags & LRU_REFS_FLAGS) == LRU_REFS_FLAGS ?
	       ((flags & LRU_REFS_MASK) >> LRU_REFS_PGOFF) + 1 : 0;

	return lru_tier_from_refs(refs);
}

static int get_swappiness(struct mem_cgroup *memcg)
{
	/*
	 * 4.4 drives the MGLRU path for global reclaim through
	 * shrink_node_memcg() with the node lruvec, which has no memcg, and
	 * both mem_cgroup_get_nr_swap_pages() and mem_cgroup_swappiness()
	 * dereference their argument.  Fall back to the global knob, exactly
	 * as 4.4's own mem_cgroup_swappiness() does for the root case.
	 */
	if (!memcg)
		return vm_swappiness;

	return mem_cgroup_get_nr_swap_pages(memcg) >= MIN_BATCH_SIZE ?
	       mem_cgroup_swappiness(memcg) : 0;
}

static struct lruvec *get_lruvec(int nid, struct mem_cgroup *memcg)
{
	struct pglist_data *pgdat = NODE_DATA(nid);

#ifdef CONFIG_MEMCG
	if (memcg)
		return &memcg->nodeinfo[nid]->lruvec;
#endif
	return pgdat ? &pgdat->lruvec : NULL;
}

static int get_nr_gens(struct lruvec *lruvec, int type)
{
	return lruvec->evictable.max_seq - lruvec->evictable.min_seq[type] + 1;
}

static bool __maybe_unused seq_is_valid(struct lruvec *lruvec)
{
	return get_nr_gens(lruvec, 1) >= MIN_NR_GENS &&
	       get_nr_gens(lruvec, 1) <= get_nr_gens(lruvec, 0) &&
	       get_nr_gens(lruvec, 0) <= MAX_NR_GENS;
}

/******************************************************************************
 *                          mm_struct list
 ******************************************************************************/

static struct lru_gen_mm_list *get_mm_list(struct mem_cgroup *memcg)
{
	static struct lru_gen_mm_list mm_list = {
		.fifo = LIST_HEAD_INIT(mm_list.fifo),
		.lock = __SPIN_LOCK_UNLOCKED(mm_list.lock),
	};

#ifdef CONFIG_MEMCG
	if (memcg)
		return &memcg->mm_list;
#endif
	return &mm_list;
}

/*
 * 4.9 keeps get_mem_cgroup_from_mm() static in mm/memcontrol.c, so MGLRU needs
 * its own copy. Like upstream it holds a css reference that lru_gen_del_mm()
 * drops again -- that pairing is what keeps the css refcount balanced.
 * css_tryget() (not css_tryget_online) is the 4.9 idiom: an offlined css must
 * not be able to spin this loop.
 */
static struct mem_cgroup *lru_gen_get_memcg_from_mm(struct mm_struct *mm)
{
#ifdef CONFIG_MEMCG
	struct mem_cgroup *memcg;

	if (mem_cgroup_disabled())
		return NULL;

	rcu_read_lock();
	do {
		if (unlikely(!mm))
			memcg = root_mem_cgroup;
		else {
			memcg = mem_cgroup_from_task(rcu_dereference(mm->owner));
			if (unlikely(!memcg))
				memcg = root_mem_cgroup;
		}
	} while (!css_tryget(&memcg->css));
	rcu_read_unlock();

	return memcg;
#else
	return NULL;
#endif
}

void lru_gen_add_mm(struct mm_struct *mm)
{
	int nid;
	struct mem_cgroup *memcg = lru_gen_get_memcg_from_mm(mm);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	VM_BUG_ON_MM(!list_empty(&mm->lrugen.list), mm);
#ifdef CONFIG_MEMCG
	VM_BUG_ON_MM(mm->lrugen.memcg, mm);
	mm->lrugen.memcg = memcg;
#endif
	spin_lock(&mm_list->lock);

	list_add_tail(&mm->lrugen.list, &mm_list->fifo);

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(nid, memcg);

		if (!lruvec)
			continue;

		if (lruvec->mm_walk.tail == &mm_list->fifo)
			lruvec->mm_walk.tail = lruvec->mm_walk.tail->prev;
	}

	spin_unlock(&mm_list->lock);
}

void lru_gen_del_mm(struct mm_struct *mm)
{
	int nid;
	struct lru_gen_mm_list *mm_list;
	struct mem_cgroup *memcg = NULL;

	if (list_empty(&mm->lrugen.list))
		return;

#ifdef CONFIG_MEMCG
	memcg = mm->lrugen.memcg;
#endif
	mm_list = get_mm_list(memcg);

	spin_lock(&mm_list->lock);

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(nid, memcg);

		if (!lruvec)
			continue;

		if (lruvec->mm_walk.tail == &mm->lrugen.list)
			lruvec->mm_walk.tail = lruvec->mm_walk.tail->next;

		if (lruvec->mm_walk.head != &mm->lrugen.list)
			continue;

		lruvec->mm_walk.head = lruvec->mm_walk.head->next;
		if (lruvec->mm_walk.head == &mm_list->fifo)
			WRITE_ONCE(lruvec->mm_walk.seq, lruvec->mm_walk.seq + 1);
	}

	list_del_init(&mm->lrugen.list);

	spin_unlock(&mm_list->lock);

#ifdef CONFIG_MEMCG
	if (mm->lrugen.memcg) {
		css_put(&mm->lrugen.memcg->css);
		mm->lrugen.memcg = NULL;
	}
#endif
}

#ifdef CONFIG_MEMCG
void lru_gen_migrate_mm(struct mm_struct *mm)
{
	struct mem_cgroup *memcg;

	lockdep_assert_held(&mm->owner->alloc_lock);

	if (mem_cgroup_disabled())
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_task(mm->owner);
	rcu_read_unlock();
	if (memcg == mm->lrugen.memcg)
		return;

	VM_BUG_ON_MM(!mm->lrugen.memcg, mm);
	VM_BUG_ON_MM(list_empty(&mm->lrugen.list), mm);

	lru_gen_del_mm(mm);
	lru_gen_add_mm(mm);
}
#endif

#define BLOOM_FILTER_SHIFT	15

static inline int filter_gen_from_seq(unsigned long seq)
{
	return seq % NR_BLOOM_FILTERS;
}

static void get_item_key(void *item, int *key)
{
	u32 hash = hash_ptr(item, BLOOM_FILTER_SHIFT * 2);

	BUILD_BUG_ON(BLOOM_FILTER_SHIFT * 2 > BITS_PER_TYPE(u32));

	key[0] = hash & (BIT(BLOOM_FILTER_SHIFT) - 1);
	key[1] = hash >> BLOOM_FILTER_SHIFT;
}

static void clear_bloom_filter(struct lruvec *lruvec, unsigned long seq)
{
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	lockdep_assert_held(&get_mm_list(lruvec_memcg_4_9(lruvec))->lock);

	filter = lruvec->mm_walk.filters[gen];
	if (filter) {
		bitmap_clear(filter, 0, BIT(BLOOM_FILTER_SHIFT));
		return;
	}

	filter = bitmap_zalloc(BIT(BLOOM_FILTER_SHIFT), GFP_ATOMIC);
	WRITE_ONCE(lruvec->mm_walk.filters[gen], filter);
}

static void set_bloom_filter(struct lruvec *lruvec, unsigned long seq, void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(lruvec->mm_walk.filters[gen]);
	if (!filter)
		return;

	get_item_key(item, key);

	if (!test_bit(key[0], filter))
		set_bit(key[0], filter);
	if (!test_bit(key[1], filter))
		set_bit(key[1], filter);
}

static bool test_bloom_filter(struct lruvec *lruvec, unsigned long seq, void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(lruvec->mm_walk.filters[gen]);
	if (!filter)
		return false;

	get_item_key(item, key);

	return test_bit(key[0], filter) && test_bit(key[1], filter);
}

static void reset_mm_stats(struct lruvec *lruvec, bool last, struct mm_walk_args *args)
{
	int i;
	int hist = lru_hist_from_seq(args->max_seq);

	lockdep_assert_held(&get_mm_list(lruvec_memcg_4_9(lruvec))->lock);

	for (i = 0; i < NR_MM_STATS; i++) {
		WRITE_ONCE(lruvec->mm_walk.stats[hist][i],
			   lruvec->mm_walk.stats[hist][i] + args->mm_stats[i]);
		args->mm_stats[i] = 0;
	}

	if (!last || NR_HIST_GENS == 1)
		return;

	hist = lru_hist_from_seq(args->max_seq + 1);
	for (i = 0; i < NR_MM_STATS; i++)
		WRITE_ONCE(lruvec->mm_walk.stats[hist][i], 0);
}

static bool should_skip_mm(struct mm_struct *mm, struct mm_walk_args *args)
{
	int type;
	unsigned long size = 0;

	if (!lru_gen_mm_is_active(mm) && !node_isset(args->node_id, mm->lrugen.nodes))
		return true;

	if (mm_is_oom_victim(mm))
		return true;

	for (type = !args->swappiness; type < ANON_AND_FILE; type++) {
		size += type ? get_mm_counter(mm, MM_FILEPAGES) :
			       get_mm_counter(mm, MM_ANONPAGES) +
			       get_mm_counter(mm, MM_SHMEMPAGES);
	}

	if (size < MIN_BATCH_SIZE)
		return true;

	if (!mmget_not_zero(mm))
		return true;

	node_clear(args->node_id, mm->lrugen.nodes);

	return false;
}

/* To support multiple walkers that concurrently walk an mm_struct list. */
static bool get_next_mm(struct lruvec *lruvec, struct mm_walk_args *args,
			struct mm_struct **iter)
{
	bool first = false;
	bool last = true;
	struct mm_struct *mm = NULL;
	struct lru_gen_mm_walk *mm_walk = &lruvec->mm_walk;
	struct lru_gen_mm_list *mm_list = get_mm_list(args->memcg);

	if (*iter)
		mmput_async(*iter);
	else if (args->max_seq <= READ_ONCE(mm_walk->seq))
		return false;

	spin_lock(&mm_list->lock);

	VM_BUG_ON(args->max_seq > mm_walk->seq + 1);
	VM_BUG_ON(*iter && args->max_seq < mm_walk->seq);
	VM_BUG_ON(*iter && !mm_walk->nr_walkers);

	if (args->max_seq <= mm_walk->seq) {
		if (!*iter)
			last = false;
		goto done;
	}

	if (mm_walk->head == &mm_list->fifo) {
		VM_BUG_ON(mm_walk->nr_walkers);
		mm_walk->head = mm_walk->head->next;
		first = true;
	}

	while (!mm && mm_walk->head != &mm_list->fifo) {
		mm = list_entry(mm_walk->head, struct mm_struct, lrugen.list);

		mm_walk->head = mm_walk->head->next;

		if (mm_walk->tail == &mm->lrugen.list) {
			mm_walk->tail = mm_walk->tail->next;
			args->use_filter = false;
		}

		if (should_skip_mm(mm, args))
			mm = NULL;
	}

	if (mm_walk->head == &mm_list->fifo)
		WRITE_ONCE(mm_walk->seq, mm_walk->seq + 1);
done:
	if (*iter && !mm)
		mm_walk->nr_walkers--;
	if (!*iter && mm)
		mm_walk->nr_walkers++;

	if (mm_walk->nr_walkers)
		last = false;

	if (mm && first)
		clear_bloom_filter(lruvec, args->max_seq + 1);

	if (*iter || last)
		reset_mm_stats(lruvec, last, args);

	spin_unlock(&mm_list->lock);

	*iter = mm;

	return last;
}

/******************************************************************************
 *                          refault feedback loop
 ******************************************************************************/

/*
 * A feedback loop modeled after the PID controller. Currently supports the
 * proportional (P) and the integral (I) terms; the derivative (D) term can be
 * added if necessary. The setpoint (SP) is the desired position; the process
 * variable (PV) is the measured position. The error is the difference between
 * the SP and the PV. A positive error results in a positive control output
 * correction, which, in our case, is to allow eviction.
 *
 * The P term is refaulted % of the current generation being evicted. The I
 * term is the exponential moving average of refaulted % of previously evicted
 * generations, using the smoothing factor 1/2.
 *
 * Our goal is to maintain proportional refaulted % across all tiers.
 */
struct ctrl_pos {
	unsigned long refaulted;
	unsigned long total;
	int gain;
};

static void read_ctrl_pos(struct lruvec *lruvec, int type, int tier, int gain,
			  struct ctrl_pos *pos)
{
	struct lrugen *lrugen = &lruvec->evictable;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);

	pos->refaulted = lrugen->avg_refaulted[type][tier] +
			 atomic_long_read(&lrugen->refaulted[hist][type][tier]);
	pos->total = lrugen->avg_total[type][tier] +
		     atomic_long_read(&lrugen->evicted[hist][type][tier]);
	if (tier)
		pos->total += lrugen->protected[hist][type][tier - 1];
	pos->gain = gain;
}

static void reset_ctrl_pos(struct lruvec *lruvec, int gen, int type)
{
	int tier;
	int hist = lru_hist_from_seq(gen);
	struct lrugen *lrugen = &lruvec->evictable;
	bool carryover = gen == lru_gen_from_seq(lrugen->min_seq[type]);
	bool clear = carryover ? NR_HIST_GENS == 1 : NR_HIST_GENS > 1;

	if (!carryover && !clear)
		return;

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		if (carryover) {
			unsigned long sum;

			sum = lrugen->avg_refaulted[type][tier] +
			      atomic_long_read(&lrugen->refaulted[hist][type][tier]);
			WRITE_ONCE(lrugen->avg_refaulted[type][tier], sum / 2);

			sum = lrugen->avg_total[type][tier] +
			      atomic_long_read(&lrugen->evicted[hist][type][tier]);
			if (tier)
				sum += lrugen->protected[hist][type][tier - 1];
			WRITE_ONCE(lrugen->avg_total[type][tier], sum / 2);
		}

		if (clear) {
			atomic_long_set(&lrugen->refaulted[hist][type][tier], 0);
			atomic_long_set(&lrugen->evicted[hist][type][tier], 0);
			if (tier)
				WRITE_ONCE(lrugen->protected[hist][type][tier - 1], 0);
		}
	}
}

static bool positive_ctrl_err(struct ctrl_pos *sp, struct ctrl_pos *pv)
{
	/*
	 * Allow eviction if the PV has a limited number of refaulted pages or a
	 * lower refaulted % than the SP.
	 */
	return pv->refaulted < MIN_BATCH_SIZE ||
	       pv->refaulted * max(sp->total, 1UL) * sp->gain <=
	       sp->refaulted * max(pv->total, 1UL) * pv->gain;
}

/******************************************************************************
 *                          the aging
 ******************************************************************************/

static int page_update_gen(struct page *page, int gen)
{
	unsigned long old_flags, new_flags;

	VM_BUG_ON(gen >= MAX_NR_GENS);

	do {
		new_flags = old_flags = READ_ONCE(page->flags);

		if (!(new_flags & LRU_GEN_MASK)) {
			new_flags |= BIT(PG_referenced);
			continue;
		}

		new_flags &= ~LRU_GEN_MASK;
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
		new_flags &= ~(LRU_REFS_MASK | LRU_REFS_FLAGS);
	} while (new_flags != old_flags &&
		 cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	return ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

static void page_inc_gen(struct page *page, struct lruvec *lruvec, bool reclaiming)
{
	int old_gen, new_gen;
	unsigned long old_flags, new_flags;
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	struct lrugen *lrugen = &lruvec->evictable;

	old_gen = lru_gen_from_seq(lrugen->min_seq[type]);

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		VM_BUG_ON_PAGE(!(new_flags & LRU_GEN_MASK), page);

		new_gen = ((new_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
		/* page_update_gen() has updated this page? */
		if (new_gen >= 0 && new_gen != old_gen) {
			list_move(&page->lru, &lrugen->lists[new_gen][type][zone]);
			return;
		}

		new_gen = (old_gen + 1) % MAX_NR_GENS;

		new_flags &= ~LRU_GEN_MASK;
		new_flags |= (new_gen + 1UL) << LRU_GEN_PGOFF;
		new_flags &= ~(LRU_REFS_MASK | LRU_REFS_FLAGS);
		/* for end_page_writeback() */
		if (reclaiming)
			new_flags |= BIT(PG_reclaim);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	lru_gen_update_size(page, lruvec, old_gen, new_gen);
	if (reclaiming)
		list_move(&page->lru, &lrugen->lists[new_gen][type][zone]);
	else
		list_move_tail(&page->lru, &lrugen->lists[new_gen][type][zone]);
}

static void update_batch_size(struct page *page, int old_gen, int new_gen,
			      struct mm_walk_args *args)
{
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);

	VM_BUG_ON(old_gen >= MAX_NR_GENS);
	VM_BUG_ON(new_gen >= MAX_NR_GENS);

	args->batch_size++;

	args->nr_pages[old_gen][type][zone] -= delta;
	args->nr_pages[new_gen][type][zone] += delta;
}

static void reset_batch_size(struct lruvec *lruvec, struct mm_walk_args *args)
{
	int gen, type, zone;
	struct lrugen *lrugen = &lruvec->evictable;

	args->batch_size = 0;

	for_each_gen_type_zone(gen, type, zone) {
		enum lru_list lru = type * LRU_FILE;
		int delta = args->nr_pages[gen][type][zone];

		if (!delta)
			continue;

		args->nr_pages[gen][type][zone] = 0;
		WRITE_ONCE(lrugen->sizes[gen][type][zone],
			   lrugen->sizes[gen][type][zone] + delta);

		if (lru_gen_is_active(lruvec, gen))
			lru += LRU_ACTIVE;
		/* node/zone only; per-memcg lru_zone_size is owned by lru_gen_update_size() */
		__update_lru_size(lruvec, lru, zone, delta);
	}
}

static int should_skip_vma(unsigned long start, unsigned long end, struct mm_walk *walk)
{
	struct address_space *mapping;
	struct vm_area_struct *vma = walk->vma;
	struct mm_walk_args *args = walk->private;

	if ((!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))) || is_vm_hugetlb_page(vma) ||
	    (vma->vm_flags & (VM_LOCKED | VM_SPECIAL | VM_SEQ_READ)))
		return true;

	if (vma_is_anonymous(vma))
		return !args->swappiness;

	if (WARN_ON_ONCE(!vma->vm_file || !vma->vm_file->f_mapping))
		return true;

	mapping = vma->vm_file->f_mapping;
	if (!mapping->a_ops->writepage)
		return true;

	return (shmem_mapping(mapping) && !args->swappiness) || mapping_unevictable(mapping);
}

/*
 * 4.9 port: mm_walk only exposes pmd_entry/pte_entry (no p4d/pud callbacks,
 * no get_next_vma batching). Walk per PMD via pmd_entry; PTE scan is done
 * inside walk_pte_range() which is called from walk_mm_pmd().
 */
static int walk_pte_range_4_9(pmd_t *pmd, unsigned long start, unsigned long end,
			      struct mm_walk *walk)
{
	pte_t *pte;
	spinlock_t *ptl;
	unsigned long addr;
	int worth = 0;
	struct mm_walk_args *args = walk->private;
	int old_gen, new_gen = lru_gen_from_seq(args->max_seq);

	pte_t *pte0;

	pte0 = pte = pte_offset_map_lock(walk->mm, pmd, start, &ptl);
	arch_enter_lazy_mmu_mode();

	for (addr = start; addr != end; addr += PAGE_SIZE, pte++) {
		struct page *page;
		unsigned long pfn = pte_pfn(*pte);

		args->mm_stats[MM_LEAF_TOTAL]++;

		if (!pte_present(*pte) || is_zero_pfn(pfn))
			continue;

		if (WARN_ON_ONCE(pte_special(*pte)))
			continue;

		if (!pte_young(*pte)) {
			args->mm_stats[MM_LEAF_OLD]++;
			continue;
		}

		if (!pfn_valid(pfn))
			continue;
		if (pfn < args->start_pfn || pfn >= args->end_pfn)
			continue;

		page = compound_head(pfn_to_page(pfn));
		if (page_to_nid(page) != args->node_id)
			continue;

		if (page_memcg_rcu(page) != args->memcg)
			continue;

		if (!ptep_test_and_clear_young(walk->vma, addr, pte))
			continue;

		args->mm_stats[MM_LEAF_YOUNG]++;

		if (pte_dirty(*pte) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) && !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_update_gen(page, new_gen);
		if (old_gen >= 0 && old_gen != new_gen)
			update_batch_size(page, old_gen, new_gen, args);

		worth++;
	}

	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(pte0, ptl);

	return worth >= MIN_BATCH_SIZE / 2;
}

static int walk_pmd_range_4_9(pmd_t *pmd, unsigned long addr, unsigned long next,
			      struct mm_walk *walk)
{
	struct mm_walk_args *args = walk->private;
	struct lruvec *lruvec = get_lruvec(args->node_id, args->memcg);

	if (pmd_trans_huge(*pmd) || pmd_none(*pmd))
		return 0;

	args->mm_stats[MM_NONLEAF_TOTAL]++;

	if (args->use_filter && !test_bloom_filter(lruvec, args->max_seq, pmd))
		return 0;

	args->mm_stats[MM_NONLEAF_PREV]++;

	if (!walk_pte_range_4_9(pmd, addr, next, walk))
		return 0;

	args->mm_stats[MM_NONLEAF_CUR]++;

	set_bloom_filter(lruvec, args->max_seq + 1, pmd);
	return 0;
}

static void walk_mm(struct lruvec *lruvec, struct mm_struct *mm, struct mm_walk_args *args)
{
	int err;
	struct mm_walk walk = {
		.pmd_entry = walk_pmd_range_4_9,
		.test_walk = should_skip_vma,
		.mm = mm,
		.private = args,
	};

	args->next_addr = FIRST_USER_ADDRESS;

	do {
		unsigned long start = args->next_addr;
		unsigned long end = mm->highest_vm_end;

		err = -EBUSY;

		rcu_read_lock();
#ifdef CONFIG_MEMCG
		if (args->memcg && atomic_read(&args->memcg->moving_account)) {
			rcu_read_unlock();
			break;
		}
#endif
		if (!down_read_trylock(&mm->mmap_sem)) {
			rcu_read_unlock();
			break;
		}

		err = walk_page_range(start, end, &walk);
		args->next_addr = end;

		up_read(&mm->mmap_sem);

		if (args->batch_size) {
			spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);
			reset_batch_size(lruvec, args);
			spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);
		}

		rcu_read_unlock();
		cond_resched();
	} while (err == -EAGAIN && args->next_addr && !mm_is_oom_victim(mm));
}


static struct mm_walk_args *alloc_mm_walk_args(void)
{
	if (!current->reclaim_state || !current->reclaim_state->mm_walk_args)
		return kzalloc(sizeof(struct mm_walk_args), GFP_KERNEL);

	return current->reclaim_state->mm_walk_args;
}

static void free_mm_walk_args(struct mm_walk_args *args)
{
	if (!current->reclaim_state || !current->reclaim_state->mm_walk_args)
		kfree(args);
}

static bool inc_min_seq(struct lruvec *lruvec, int type)
{
	int gen, zone;
	int remaining = MAX_BATCH_SIZE;
	struct lrugen *lrugen = &lruvec->evictable;

	VM_BUG_ON(!seq_is_valid(lruvec));

	if (get_nr_gens(lruvec, type) != MAX_NR_GENS)
		return true;

	gen = lru_gen_from_seq(lrugen->min_seq[type]);

	for (zone = 0; zone < MAX_NR_ZONES; zone++) {
		struct list_head *head = &lrugen->lists[gen][type][zone];

		while (!list_empty(head)) {
			struct page *page = lru_to_page(head);

			VM_BUG_ON_PAGE(PageTail(page), page);
			VM_BUG_ON_PAGE(PageUnevictable(page), page);
			VM_BUG_ON_PAGE(PageActive(page), page);
			VM_BUG_ON_PAGE(page_is_file_cache(page) != type, page);
			VM_BUG_ON_PAGE(page_zonenum(page) != zone, page);

			prefetchw_prev_lru_page(page, head, flags);

			page_inc_gen(page, lruvec, false);

			if (!--remaining)
				return false;
		}
	}

	reset_ctrl_pos(lruvec, gen, type);
	WRITE_ONCE(lrugen->min_seq[type], lrugen->min_seq[type] + 1);

	return true;
}

static bool try_to_inc_min_seq(struct lruvec *lruvec, int swappiness)
{
	int gen, type, zone;
	bool success = false;
	struct lrugen *lrugen = &lruvec->evictable;
	DEFINE_MIN_SEQ(lruvec);

	VM_BUG_ON(!seq_is_valid(lruvec));

	for (type = 0; type < ANON_AND_FILE; type++) {
		while (lrugen->max_seq - min_seq[type] >= MIN_NR_GENS) {
			gen = lru_gen_from_seq(min_seq[type]);

			for (zone = 0; zone < MAX_NR_ZONES; zone++) {
				if (!list_empty(&lrugen->lists[gen][type][zone]))
					goto next;
			}

			min_seq[type]++;
		}
next:
		;
	}

	min_seq[0] = min(min_seq[0], min_seq[1]);
	if (swappiness)
		min_seq[1] = max(min_seq[0], lrugen->min_seq[1]);

	for (type = 0; type < ANON_AND_FILE; type++) {
		if (min_seq[type] == lrugen->min_seq[type])
			continue;

		gen = lru_gen_from_seq(lrugen->min_seq[type]);
		reset_ctrl_pos(lruvec, gen, type);
		WRITE_ONCE(lrugen->min_seq[type], min_seq[type]);
		success = true;
	}

	return success;
}

static void inc_max_seq(struct lruvec *lruvec, unsigned long max_seq)
{
	int gen, type, zone;
	struct lrugen *lrugen = &lruvec->evictable;

	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	VM_BUG_ON(!seq_is_valid(lruvec));

	if (max_seq != lrugen->max_seq)
		goto unlock;

	if (!try_to_inc_min_seq(lruvec, true)) {
		for (type = ANON_AND_FILE - 1; type >= 0; type--) {
			while (!inc_min_seq(lruvec, type)) {
				spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);
				cond_resched();
				spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);
			}
		}
	}

	gen = lru_gen_from_seq(lrugen->max_seq - 1);
	for (type = 0; type < ANON_AND_FILE; type++) {
		for (zone = 0; zone < MAX_NR_ZONES; zone++) {
			enum lru_list lru = type * LRU_FILE;
			long delta = lrugen->sizes[gen][type][zone];

			if (!delta)
				continue;

			WARN_ON_ONCE(delta != (int)delta);

			/*
			 * Node/zone counters only. lru_zone_size per memcg is
			 * maintained by lru_gen_update_size(); this path must not
			 * touch it or it underflows (lru_size -456 observed).
			 */
			__update_lru_size(lruvec, lru, zone, delta);
			__update_lru_size(lruvec, lru + LRU_ACTIVE, zone, -delta);
		}
	}

	gen = lru_gen_from_seq(lrugen->max_seq + 1);
	for (type = 0; type < ANON_AND_FILE; type++) {
		for (zone = 0; zone < MAX_NR_ZONES; zone++) {
			enum lru_list lru = type * LRU_FILE;
			long delta = lrugen->sizes[gen][type][zone];

			if (!delta)
				continue;

			WARN_ON_ONCE(delta != (int)delta);

			__update_lru_size(lruvec, lru, zone, -delta);
			__update_lru_size(lruvec, lru + LRU_ACTIVE, zone, delta);
		}
	}

	for (type = 0; type < ANON_AND_FILE; type++)
		reset_ctrl_pos(lruvec, gen, type);

	WRITE_ONCE(lrugen->timestamps[gen], jiffies);
	/* make sure all preceding modifications appear first */
	smp_store_release(&lrugen->max_seq, lrugen->max_seq + 1);
unlock:
	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);
}

/* Main function used by the foreground, the background and the user-triggered aging. */
static bool try_to_inc_max_seq(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
			       unsigned long max_seq, bool use_filter,
			       struct mem_cgroup *memcg)
{
	bool last;
	struct mm_walk_args *args;
	struct mm_struct *mm = NULL;
	struct lrugen *lrugen = &lruvec->evictable;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	int nid = pgdat->node_id;

	VM_BUG_ON(max_seq > READ_ONCE(lrugen->max_seq));

	/*
	 * If we are not from run_aging() and clearing the accessed bit may
	 * trigger page faults, then don't proceed to clearing all accessed
	 * PTEs. Instead, fallback to lru_gen_look_around(), which only clears a
	 * handful of accessed PTEs. This is less efficient but causes fewer
	 * page faults on CPUs that don't have the capability.
	 */
	/* 4.9 arm64 has no ARM64_HW_AF cap: arch_has_hw_pte_young() is always
	 * false here, so the conservative path is always taken. */
	if ((current->flags & PF_MEMALLOC) && !arch_has_hw_pte_young(false)) {
		inc_max_seq(lruvec, max_seq);
		return true;
	}

	args = alloc_mm_walk_args();
	if (!args)
		return false;

	args->memcg = memcg;
	args->max_seq = max_seq;
	args->start_pfn = pgdat->node_start_pfn;
	args->end_pfn = pgdat_end_pfn(pgdat);
	args->node_id = nid;
	args->swappiness = swappiness;
	args->use_filter = use_filter;

	do {
		last = get_next_mm(lruvec, args, &mm);
		if (mm)
			walk_mm(lruvec, mm, args);

		cond_resched();
	} while (mm);

	free_mm_walk_args(args);

	if (!last) {
		/* don't wait unless we may have trouble reclaiming */
		if (!current_is_kswapd() && sc->priority < DEF_PRIORITY - 2)
			wait_event_killable(lruvec->mm_walk.wait,
					    max_seq < READ_ONCE(lrugen->max_seq));

		return max_seq < READ_ONCE(lrugen->max_seq);
	}

	VM_BUG_ON(max_seq != READ_ONCE(lrugen->max_seq));

	inc_max_seq(lruvec, max_seq);
	/* either we see any waiters or they will see updated max_seq */
	if (waitqueue_active(&lruvec->mm_walk.wait))
		wake_up_all(&lruvec->mm_walk.wait);

	wakeup_flusher_threads(0, WB_REASON_VMSCAN);

	return true;
}

static long get_nr_evictable(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
			     unsigned long max_seq, unsigned long *min_seq, bool *low)
{
	int gen, type, zone;
	long max = 0;
	long min = 0;
	struct lrugen *lrugen = &lruvec->evictable;

	for (type = !swappiness; type < ANON_AND_FILE; type++) {
		unsigned long seq;

		for (seq = min_seq[type]; seq <= max_seq; seq++) {
			long size = 0;

			gen = lru_gen_from_seq(seq);

			for (zone = 0; zone <= sc->reclaim_idx; zone++)
				size += READ_ONCE(lrugen->sizes[gen][type][zone]);

			max += size;
			if (type && max_seq - seq >= MIN_NR_GENS)
				min += size;
		}
	}

	*low = max_seq - min_seq[1] <= MIN_NR_GENS && min < MIN_BATCH_SIZE;

	return max > 0 ? max : 0;
}

static bool age_lruvec(struct lruvec *lruvec, struct scan_control *sc,
		       unsigned long min_ttl, struct mem_cgroup *memcg)
{
	bool low;
	long nr_to_scan;
	int swappiness = get_swappiness(memcg);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	/* 4.9: no mem_cgroup_below_min(); low protection not honored on this path */
	if (false)
		return false;

	if (min_ttl) {
		int gen = lru_gen_from_seq(min_seq[1]);
		unsigned long birth = READ_ONCE(lruvec->evictable.timestamps[gen]);

		if (time_is_after_jiffies(birth + min_ttl))
			return false;
	}

	nr_to_scan = get_nr_evictable(lruvec, sc, swappiness, max_seq, min_seq, &low);
	if (!nr_to_scan)
		return false;

	nr_to_scan >>= sc->priority;

	/*
	 * memcg is NULL when the lruvec is the node one (memory controller
	 * off), and mem_cgroup_online() dereferences its argument.  NULL
	 * behaves like the online/disabled case: no extra scan budget.
	 */
	if (memcg && !mem_cgroup_online(memcg))
		nr_to_scan++;

	if (nr_to_scan && low)
		try_to_inc_max_seq(lruvec, sc, swappiness, max_seq, true, memcg);

	return true;
}

/* Protect the working set accessed within the last N milliseconds. */
static unsigned long lru_gen_min_ttl __read_mostly;

static void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	bool success = false;
	unsigned long min_ttl = READ_ONCE(lru_gen_min_ttl);

	VM_BUG_ON(!current_is_kswapd());

	if (!sc->force_deactivate) {
		sc->force_deactivate = 1;
		return;
	}

	current->reclaim_state->mm_walk_args = &pgdat->mm_walk_args;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);

		if (age_lruvec(lruvec, sc, min_ttl, memcg))
			success = true;

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	if (!success && mutex_trylock(&oom_lock)) {
		struct oom_control oc = {
			.gfp_mask = sc->gfp_mask,
			.order = sc->order,
		};

		/* to avoid overkilling */
		if (!oom_reaping_in_progress())
			out_of_memory(&oc);

		mutex_unlock(&oom_lock);
	}

	current->reclaim_state->mm_walk_args = NULL;
}

/* Scan the vicinity of an accessed PTE when shrink_page_list() uses the rmap. */
static bool sort_page(struct page *page, struct lruvec *lruvec, int tier_idx)
{
	bool success;
	int gen = page_lru_gen(page);
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int tier = page_lru_tier(page);
	int delta = hpage_nr_pages(page);
	struct lrugen *lrugen = &lruvec->evictable;

	VM_BUG_ON_PAGE(gen >= MAX_NR_GENS, page);

	/* an mlocked page? */
	if (!page_evictable(page)) {
		success = lru_gen_del_page(page, lruvec, true);
		VM_BUG_ON_PAGE(!success, page);
		SetPageUnevictable(page);
		add_page_to_lru_list(page, lruvec, LRU_UNEVICTABLE);
		__count_vm_events(UNEVICTABLE_PGCULLED, delta);
		return true;
	}

	/* a lazy-free page that has been written into? */
	if (type && PageDirty(page) && PageAnon(page)) {
		success = lru_gen_del_page(page, lruvec, true);
		VM_BUG_ON_PAGE(!success, page);
		SetPageSwapBacked(page);
		add_page_to_lru_list_tail(page, lruvec, LRU_INACTIVE_FILE);
		return true;
	}

	/* page_update_gen() has updated this page? */
	if (gen != lru_gen_from_seq(lrugen->min_seq[type])) {
		list_move(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	/* protect this page if its tier has a higher refaulted % */
	if (tier > tier_idx) {
		int hist = lru_hist_from_seq(gen);

		page_inc_gen(page, lruvec, false);
		WRITE_ONCE(lrugen->protected[hist][type][tier - 1],
			   lrugen->protected[hist][type][tier - 1] + delta);
		/* 4.9: no per-type WORKINGSET_ACTIVATE node stat; skipped */
		return true;
	}

	/* mark this page for reclaim if it's pending writeback */
	if (PageWriteback(page) || (type && PageDirty(page))) {
		page_inc_gen(page, lruvec, true);
		return true;
	}

	return false;
}

static bool isolate_page(struct page *page, struct lruvec *lruvec, struct scan_control *sc)
{
	bool success;

	if (!sc->may_unmap && page_mapped(page))
		return false;

	if (!(sc->may_writepage && (sc->gfp_mask & __GFP_IO)) &&
	    (PageDirty(page) || (PageAnon(page) && !PageSwapCache(page))))
		return false;

	if (!get_page_unless_zero(page))
		return false;

	if (!PageLRU(page)) {
		put_page(page);
		return false;
	}
	ClearPageLRU(page);

	success = lru_gen_del_page(page, lruvec, true);
	VM_BUG_ON_PAGE(!success, page);

	return true;
}

static int scan_pages(struct lruvec *lruvec, struct scan_control *sc,
		      int type, int tier, struct list_head *list)
{
	int gen, zone;
	enum vm_event_item item;
	int sorted = 0;
	int scanned = 0;
	int isolated = 0;
	int remaining = MAX_BATCH_SIZE;
	struct lrugen *lrugen = &lruvec->evictable;
	VM_BUG_ON(!list_empty(list));

	if (get_nr_gens(lruvec, type) == MIN_NR_GENS)
		return 0;

	gen = lru_gen_from_seq(lrugen->min_seq[type]);

	for (zone = sc->reclaim_idx; zone >= 0; zone--) {
		LIST_HEAD(moved);
		int skipped = 0;
		struct list_head *head = &lrugen->lists[gen][type][zone];

		while (!list_empty(head)) {
			struct page *page = lru_to_page(head);
			int delta = hpage_nr_pages(page);

			VM_BUG_ON_PAGE(PageTail(page), page);
			VM_BUG_ON_PAGE(PageUnevictable(page), page);
			VM_BUG_ON_PAGE(PageActive(page), page);
			VM_BUG_ON_PAGE(page_is_file_cache(page) != type, page);
			VM_BUG_ON_PAGE(page_zonenum(page) != zone, page);

			prefetchw_prev_lru_page(page, head, flags);

			scanned += delta;

			if (sort_page(page, lruvec, tier))
				sorted += delta;
			else if (isolate_page(page, lruvec, sc)) {
				list_add(&page->lru, list);
				isolated += delta;
			} else {
				list_move(&page->lru, &moved);
				skipped += delta;
			}

			if (!--remaining || max(isolated, skipped) >= MIN_BATCH_SIZE)
				break;
		}

		if (skipped) {
			list_splice(&moved, head);
			__count_zid_vm_events(PGSCAN_SKIP, zone, skipped);
		}

		if (!remaining || isolated >= MIN_BATCH_SIZE)
			break;
	}

	item = current_is_kswapd() ? PGSCAN_KSWAPD : PGSCAN_DIRECT;
	if (global_reclaim(sc)) {
		__count_vm_events(item, isolated);
		__count_vm_events(PGREFILL, sorted);
	}

	/*
	 * We may have trouble finding eligible pages due to reclaim_idx,
	 * may_unmap and may_writepage. Check `remaining` to make sure we won't
	 * be stuck if we aren't making enough progress.
	 */
	return isolated || !remaining ? scanned : 0;
}

static int get_tier_idx(struct lruvec *lruvec, int type)
{
	int tier;
	struct ctrl_pos sp, pv;

	/*
	 * Ideally we don't want to evict upper tiers that have higher refaulted
	 * %. However, we need to leave a margin for the fluctuation in
	 * refaulted %. So we use a larger gain factor to make sure upper tiers
	 * are indeed more active. We choose 2 because the lowest upper tier
	 * would have twice of refaulted % of the base tier, according to their
	 * numbers of accesses.
	 */
	read_ctrl_pos(lruvec, type, 0, 1, &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, 2, &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	return tier - 1;
}

static int get_type_to_scan(struct lruvec *lruvec, int swappiness, int *tier_idx)
{
	int type, tier;
	struct ctrl_pos sp, pv;
	int gain[ANON_AND_FILE] = { swappiness, 200 - swappiness };

	/*
	 * Compare refaulted % between the base tiers of anon and file to
	 * determine which type to evict. Also need to compare refaulted % of
	 * the upper tiers of the selected type with that of the base tier of
	 * the other type to determine which tier of the selected type to evict.
	 */
	read_ctrl_pos(lruvec, 0, 0, gain[0], &sp);
	read_ctrl_pos(lruvec, 1, 0, gain[1], &pv);
	type = positive_ctrl_err(&sp, &pv);

	read_ctrl_pos(lruvec, !type, 0, gain[!type], &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, gain[type], &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	*tier_idx = tier - 1;

	return type;
}

static int isolate_pages(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
			 int *type_scanned, struct list_head *list)
{
	int i;
	int type;
	int scanned;
	int tier = -1;
	DEFINE_MIN_SEQ(lruvec);

	VM_BUG_ON(!seq_is_valid(lruvec));

	/*
	 * Try to select a type based on generations and swappiness, and if that
	 * fails, fall back to get_type_to_scan(). When anon and file are both
	 * available from the same generation, swappiness 200 is interpreted as
	 * anon first and swappiness 1 is interpreted as file first.
	 */
	if (!swappiness)
		type = 1;
	else if (min_seq[0] < min_seq[1])
		type = 0;
	else if (swappiness == 1)
		type = 1;
	else if (swappiness == 200)
		type = 0;
	else
		type = get_type_to_scan(lruvec, swappiness, &tier);

	for (i = !swappiness; i < ANON_AND_FILE; i++) {
		if (tier < 0)
			tier = get_tier_idx(lruvec, type);

		scanned = scan_pages(lruvec, sc, type, tier, list);
		if (scanned)
			break;

		type = !type;
		tier = -1;
	}

	*type_scanned = type;

	return scanned;
}

/* Main function used by the foreground, the background and the user-triggered eviction. */
static int evict_pages(struct lruvec *lruvec, struct scan_control *sc, int swappiness)
{
	int type;
	int scanned;
	int reclaimed;
	LIST_HEAD(list);
	struct page *page;
	enum vm_event_item item;
	unsigned long nr_dirty, nr_unqueued_dirty, nr_congested, nr_writeback, nr_immediate;
	struct mm_walk_args *args;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	scanned = isolate_pages(lruvec, sc, swappiness, &type, &list);

	if (try_to_inc_min_seq(lruvec, swappiness))
		scanned++;

	if (get_nr_gens(lruvec, 1) == MIN_NR_GENS)
		scanned = 0;

	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	if (list_empty(&list))
		return scanned;

	reclaimed = shrink_page_list(&list, pgdat, sc, TTU_UNMAP,
				     &nr_dirty, &nr_unqueued_dirty, &nr_congested,
				     &nr_writeback, &nr_immediate, false);
	/*
	 * We need to prevent rejected pages from being added back to the same
	 * lists they were isolated from. Otherwise we may risk looping on them
	 * forever.
	 */
	list_for_each_entry(page, &list, lru) {
		if (!PageReclaim(page) || !(PageDirty(page) || PageWriteback(page)))
			SetPageActive(page);

		ClearPageReferenced(page);
		ClearPageWorkingset(page);
	}

	spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	putback_inactive_pages(lruvec, &list);

	args = current->reclaim_state ? current->reclaim_state->mm_walk_args : NULL;
	if (args && args->batch_size)
		reset_batch_size(lruvec, args);

	item = current_is_kswapd() ? PGSTEAL_KSWAPD : PGSTEAL_DIRECT;
	if (global_reclaim(sc))
		__count_vm_events(item, reclaimed);

	spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);

	mem_cgroup_uncharge_list(&list);
	free_hot_cold_page_list(&list, true);

	sc->nr_reclaimed += reclaimed;

	return scanned;
}

static long get_nr_to_scan(struct lruvec *lruvec, struct scan_control *sc,
			   int swappiness, struct mem_cgroup *memcg)
{
	bool low;
	long nr_to_scan;
	int priority = sc->priority;
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (false)
		return 0;

	if (sc->nr_reclaimed >= sc->nr_to_reclaim) {
		priority = DEF_PRIORITY;
		sc->force_deactivate = 0;
	}

	nr_to_scan = get_nr_evictable(lruvec, sc, swappiness, max_seq, min_seq, &low);
	if (!nr_to_scan)
		return 0;

	nr_to_scan >>= priority;

	/*
	 * memcg is NULL when the lruvec is the node one (memory controller
	 * off), and mem_cgroup_online() dereferences its argument.  NULL
	 * behaves like the online/disabled case: no extra scan budget.
	 */
	if (memcg && !mem_cgroup_online(memcg))
		nr_to_scan++;

	if (!nr_to_scan)
		return 0;

	if (current_is_kswapd()) {
		/* leave the work to lru_gen_age_node() */
		if (max_seq - min_seq[1] < MIN_NR_GENS)
			return 0;

		if (!low)
			sc->force_deactivate = 0;

		return nr_to_scan;
	}

	if (max_seq - min_seq[1] >= MIN_NR_GENS)
		return nr_to_scan;

	/* move onto slab and other memcgs if we haven't tried them all */
	if (!sc->force_deactivate) {
		sc->skipped_deactivate = 1;
		return 0;
	}

	return try_to_inc_max_seq(lruvec, sc, swappiness, max_seq, true, memcg) ? nr_to_scan : 0;
}

/* Returns the number of pages MGLRU actually scanned for this lruvec. */
static unsigned long lru_gen_shrink_lruvec(struct lruvec *lruvec,
					   struct scan_control *sc,
					   struct mem_cgroup *memcg)
{
	struct blk_plug plug;
	long scanned = 0;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	/*
	 * Take the memcg from the caller (shrink_node_memcg, which owns it)
	 * rather than re-deriving it from the lruvec: lruvec_memcg_4_9()
	 * uses container_of(), which is only valid for a memcg lruvec, and
	 * on this integration it has been seen to hand back a pointer that
	 * is neither NULL nor a mem_cgroup.
	 */
	printk_once(KERN_ERR
		    "lru_gen: lruvec=%p pgdat=%p node_lruvec=%p disabled=%d memcg=%p\n",
		    lruvec, pgdat, node_lruvec(pgdat),
		    mem_cgroup_disabled(), memcg);

	lru_add_drain();

	if (current_is_kswapd())
		current->reclaim_state->mm_walk_args = &pgdat->mm_walk_args;

	blk_start_plug(&plug);

	while (true) {
		int delta;
		int swappiness;
		long nr_to_scan;

		if (sc->may_swap)
			swappiness = get_swappiness(memcg);
		else if (!global_reclaim(sc) && get_swappiness(memcg))
			swappiness = 1;
		else
			swappiness = 0;

		nr_to_scan = get_nr_to_scan(lruvec, sc, swappiness, memcg);
		if (!nr_to_scan)
			break;

		delta = evict_pages(lruvec, sc, swappiness);
		if (!delta)
			break;

		scanned += delta;
		if (scanned >= nr_to_scan)
			break;

		cond_resched();
	}

	blk_finish_plug(&plug);

	if (current_is_kswapd())
		current->reclaim_state->mm_walk_args = NULL;

	return scanned;
}

/******************************************************************************
 *                          state change
 ******************************************************************************/

#ifdef CONFIG_LRU_GEN_ENABLED
DEFINE_STATIC_KEY_TRUE(lru_gen_static_key);
#else
DEFINE_STATIC_KEY_FALSE(lru_gen_static_key);
#endif

static int lru_gen_nr_swapfiles;

static bool __maybe_unused state_is_valid(struct lruvec *lruvec)
{
	int gen, type, zone;
	enum lru_list lru;
	struct lrugen *lrugen = &lruvec->evictable;

	for_each_evictable_lru(lru) {
		type = is_file_lru(lru);

		if (lrugen->enabled[type] && !list_empty(&lruvec->lists[lru]))
			return false;
	}

	for_each_gen_type_zone(gen, type, zone) {
		if (!lrugen->enabled[type] && !list_empty(&lrugen->lists[gen][type][zone]))
			return false;

		/* unlikely but not a bug when reset_batch_size() is pending */
		VM_WARN_ON(!lrugen->enabled[type] && lrugen->sizes[gen][type][zone]);
	}

	return true;
}

static bool fill_lists(struct lruvec *lruvec)
{
	enum lru_list lru;
	int remaining = MAX_BATCH_SIZE;

	for_each_evictable_lru(lru) {
		int type = is_file_lru(lru);
		bool active = is_active_lru(lru);
		struct list_head *head = &lruvec->lists[lru];

		if (!lruvec->evictable.enabled[type])
			continue;

		while (!list_empty(head)) {
			bool success;
			struct page *page = lru_to_page(head);

			VM_BUG_ON_PAGE(PageTail(page), page);
			VM_BUG_ON_PAGE(PageUnevictable(page), page);
			VM_BUG_ON_PAGE(PageActive(page) != active, page);
			VM_BUG_ON_PAGE(page_is_file_cache(page) != type, page);
			VM_BUG_ON_PAGE(page_lru_gen(page) < MAX_NR_GENS, page);

			prefetchw_prev_lru_page(page, head, flags);

			del_page_from_lru_list(page, lruvec, page_lru(page));
			success = lru_gen_add_page(page, lruvec, false);
			VM_BUG_ON(!success);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static bool drain_lists(struct lruvec *lruvec)
{
	int gen, type, zone;
	int remaining = MAX_BATCH_SIZE;

	for_each_gen_type_zone(gen, type, zone) {
		struct list_head *head = &lruvec->evictable.lists[gen][type][zone];

		if (lruvec->evictable.enabled[type])
			continue;

		while (!list_empty(head)) {
			bool success;
			struct page *page = lru_to_page(head);

			VM_BUG_ON_PAGE(PageTail(page), page);
			VM_BUG_ON_PAGE(PageUnevictable(page), page);
			VM_BUG_ON_PAGE(PageActive(page), page);
			VM_BUG_ON_PAGE(page_is_file_cache(page) != type, page);
			VM_BUG_ON_PAGE(page_zonenum(page) != zone, page);

			prefetchw_prev_lru_page(page, head, flags);

			success = lru_gen_del_page(page, lruvec, false);
			VM_BUG_ON(!success);
			add_page_to_lru_list(page, lruvec, LRU_INACTIVE_FILE);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

/*
 * For file page tracking, we enable/disable it according to the main switch.
 * For anon page tracking, we only enabled it when the main switch is on and
 * there is at least one swapfile; we disable it when there are no swapfiles
 * regardless of the value of the main switch. Otherwise, we will eventually
 * reach the max size of the sliding window and have to call inc_min_seq().
 */
void lru_gen_change_state(bool enable, bool main, bool swap)
{
	static DEFINE_MUTEX(state_mutex);

	struct mem_cgroup *memcg;

	mem_hotplug_begin();
	cgroup_lock();
	mutex_lock(&state_mutex);

	if (swap) {
		if (enable)
			swap = !lru_gen_nr_swapfiles++;
		else
			swap = !--lru_gen_nr_swapfiles;
	}

	if (main && enable != lru_gen_enabled()) {
		if (enable)
			static_branch_enable(&lru_gen_static_key);
		else
			static_branch_disable(&lru_gen_static_key);
	} else if (!swap || !lru_gen_enabled())
		goto unlock;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node(nid) {
			struct lruvec *lruvec = get_lruvec(nid, memcg);

			if (!lruvec)
				continue;

			spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);

			VM_BUG_ON(!seq_is_valid(lruvec));
			VM_BUG_ON(!state_is_valid(lruvec));

			lruvec->evictable.enabled[0] = lru_gen_enabled() && lru_gen_nr_swapfiles;
			lruvec->evictable.enabled[1] = lru_gen_enabled();

			while (!(enable ? fill_lists(lruvec) : drain_lists(lruvec))) {
				spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);
				cond_resched();
				spin_lock_irq(&lruvec_pgdat(lruvec)->lru_lock);
			}

			spin_unlock_irq(&lruvec_pgdat(lruvec)->lru_lock);
		}

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));
unlock:
	mutex_unlock(&state_mutex);
	cgroup_unlock();
	mem_hotplug_done();
}

/******************************************************************************
 *                          sysfs interface
 ******************************************************************************/

static ssize_t show_min_ttl(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", jiffies_to_msecs(READ_ONCE(lru_gen_min_ttl)));
}

static ssize_t store_min_ttl(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	unsigned int msecs;

	if (kstrtouint(buf, 10, &msecs))
		return -EINVAL;

	WRITE_ONCE(lru_gen_min_ttl, msecs_to_jiffies(msecs));

	return len;
}

static struct kobj_attribute lru_gen_min_ttl_attr = __ATTR(
	min_ttl_ms, 0644, show_min_ttl, store_min_ttl
);

static ssize_t show_enable(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", lru_gen_enabled());
}

static ssize_t store_enable(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t len)
{
	bool enable;

	if (kstrtobool(buf, &enable))
		return -EINVAL;

	lru_gen_change_state(enable, true, false);

	return len;
}

static struct kobj_attribute lru_gen_enabled_attr = __ATTR(
	enabled, 0644, show_enable, store_enable
);

static struct attribute *lru_gen_attrs[] = {
	&lru_gen_min_ttl_attr.attr,
	&lru_gen_enabled_attr.attr,
	NULL
};

static struct attribute_group lru_gen_attr_group = {
	.name = "lru_gen",
	.attrs = lru_gen_attrs,
};

/******************************************************************************
 *                          debugfs interface
 ******************************************************************************/

static void *lru_gen_seq_start(struct seq_file *m, loff_t *pos)
{
	struct mem_cgroup *memcg;
	loff_t nr_to_skip = *pos;

	m->private = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!m->private)
		return ERR_PTR(-ENOMEM);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node_state(nid, N_MEMORY) {
			if (!nr_to_skip--)
				return get_lruvec(nid, memcg);
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	return NULL;
}

static void lru_gen_seq_stop(struct seq_file *m, void *v)
{
	if (!IS_ERR_OR_NULL(v))
		mem_cgroup_iter_break(NULL, lruvec_memcg_4_9(v));

	kfree(m->private);
	m->private = NULL;
}

static void *lru_gen_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	int nid = lruvec_pgdat(v)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg_4_9(v);

	++*pos;

	nid = next_memory_node(nid);
	if (nid == MAX_NUMNODES) {
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
		if (!memcg)
			return NULL;

		nid = first_memory_node;
	}

	return get_lruvec(nid, memcg);
}

static void lru_gen_seq_show_full(struct seq_file *m, struct lruvec *lruvec,
				  unsigned long max_seq, unsigned long *min_seq,
				  unsigned long seq)
{
	int i;
	int type, tier;
	int hist = lru_hist_from_seq(seq);
	struct lrugen *lrugen = &lruvec->evictable;

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		seq_printf(m, "            %10d", tier);
		for (type = 0; type < ANON_AND_FILE; type++) {
			unsigned long n[3] = {};

			if (seq == max_seq) {
				n[0] = READ_ONCE(lrugen->avg_refaulted[type][tier]);
				n[1] = READ_ONCE(lrugen->avg_total[type][tier]);

				seq_printf(m, " %10luR %10luT %10lu ", n[0], n[1], n[2]);
			} else if (seq == min_seq[type] || NR_HIST_GENS > 1) {
				n[0] = atomic_long_read(&lrugen->refaulted[hist][type][tier]);
				n[1] = atomic_long_read(&lrugen->evicted[hist][type][tier]);
				if (tier)
					n[2] = READ_ONCE(lrugen->protected[hist][type][tier - 1]);

				seq_printf(m, " %10lur %10lue %10lup", n[0], n[1], n[2]);
			} else
				seq_puts(m, "          0           0           0 ");
		}
		seq_putc(m, '\n');
	}

	seq_puts(m, "                      ");
	for (i = 0; i < NR_MM_STATS; i++) {
		if (seq == max_seq && NR_HIST_GENS == 1)
			seq_printf(m, " %10lu%c", READ_ONCE(lruvec->mm_walk.stats[hist][i]),
				   toupper(MM_STAT_CODES[i]));
		else if (seq != max_seq && NR_HIST_GENS > 1)
			seq_printf(m, " %10lu%c", READ_ONCE(lruvec->mm_walk.stats[hist][i]),
				   MM_STAT_CODES[i]);
		else
			seq_puts(m, "          0 ");
	}
	seq_putc(m, '\n');
}

/*
 * 4.4 has no debugfs proxy fops, so we cannot ask the real file_operations.
 * The two files are distinct: lru_gen_full is created read-only (0444).
 */
static bool lru_gen_debugfs_is_full(void)
{
	return false;
}

static int lru_gen_seq_show(struct seq_file *m, void *v)
{
	unsigned long seq;
	bool full = lru_gen_debugfs_is_full();
	struct lruvec *lruvec = v;
	struct lrugen *lrugen = &lruvec->evictable;
	int nid = lruvec_pgdat(lruvec)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg_4_9(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (nid == first_memory_node) {
		const char *path = memcg ? m->private : "";

#ifdef CONFIG_MEMCG
		if (memcg)
			cgroup_path(memcg->css.cgroup, m->private, PATH_MAX);
#endif
		seq_printf(m, "memcg %5hu %s\n", mem_cgroup_id(memcg), path);
	}

	seq_printf(m, " node %5d\n", nid);

	if (!full)
		seq = min_seq[0];
	else if (max_seq >= MAX_NR_GENS)
		seq = max_seq - MAX_NR_GENS + 1;
	else
		seq = 0;

	for (; seq <= max_seq; seq++) {
		int gen, type, zone;
		unsigned int msecs;

		gen = lru_gen_from_seq(seq);
		msecs = jiffies_to_msecs(jiffies - READ_ONCE(lrugen->timestamps[gen]));

		seq_printf(m, " %10lu %10u", seq, msecs);

		for (type = 0; type < ANON_AND_FILE; type++) {
			long size = 0;

			if (seq < min_seq[type]) {
				seq_puts(m, "         -0 ");
				continue;
			}

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += READ_ONCE(lrugen->sizes[gen][type][zone]);

			seq_printf(m, " %10lu ", max(size, 0L));
		}

		seq_putc(m, '\n');

		if (full)
			lru_gen_seq_show_full(m, lruvec, max_seq, min_seq, seq);
	}

	return 0;
}

static const struct seq_operations lru_gen_seq_ops = {
	.start = lru_gen_seq_start,
	.stop = lru_gen_seq_stop,
	.next = lru_gen_seq_next,
	.show = lru_gen_seq_show,
};

static int run_aging(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
		     unsigned long seq, bool use_filter)
{
	DEFINE_MAX_SEQ(lruvec);

	if (seq == max_seq)
		try_to_inc_max_seq(lruvec, sc, swappiness, max_seq, use_filter,
				   lruvec_memcg_4_9(lruvec));

	return seq > max_seq ? -EINVAL : 0;
}

static int run_eviction(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
			unsigned long seq, unsigned long nr_to_reclaim)
{
	struct blk_plug plug;
	int err = -EINTR;
	DEFINE_MAX_SEQ(lruvec);

	if (seq >= max_seq - 1)
		return -EINVAL;

	sc->nr_reclaimed = 0;

	blk_start_plug(&plug);

	while (!signal_pending(current)) {
		DEFINE_MIN_SEQ(lruvec);

		if (seq < min_seq[!swappiness] || sc->nr_reclaimed >= nr_to_reclaim ||
		    !evict_pages(lruvec, sc, swappiness)) {
			err = 0;
			break;
		}

		cond_resched();
	}

	blk_finish_plug(&plug);

	return err;
}

static int run_cmd(char cmd, int memcg_id, int nid, struct scan_control *sc,
		   int swappiness, unsigned long seq, unsigned long opt)
{
	struct lruvec *lruvec;
	int err = -EINVAL;
	struct mem_cgroup *memcg = NULL;

	if (!mem_cgroup_disabled()) {
		rcu_read_lock();
		memcg = mem_cgroup_from_id(memcg_id);
#ifdef CONFIG_MEMCG
		if (memcg && !css_tryget(&memcg->css))
			memcg = NULL;
#endif
		rcu_read_unlock();

		if (!memcg)
			goto done;
	}
	if (memcg_id != mem_cgroup_id(memcg))
		goto done;

	if (nid < 0 || nid >= MAX_NUMNODES || !node_state(nid, N_MEMORY))
		goto done;

	lruvec = get_lruvec(nid, memcg);

	if (swappiness < 0)
		swappiness = get_swappiness(memcg);
	else if (swappiness > 200)
		goto done;

	switch (cmd) {
	case '+':
		err = run_aging(lruvec, sc, swappiness, seq, opt);
		break;
	case '-':
		err = run_eviction(lruvec, sc, swappiness, seq, opt);
		break;
	}
done:
	css_put(&memcg->css);

	return err;
}

static ssize_t lru_gen_seq_write(struct file *file, const char __user *src,
				 size_t len, loff_t *pos)
{
	void *buf;
	char *cur, *next;
	unsigned int flags;
	int err = 0;
	struct scan_control sc = {
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.gfp_mask = GFP_KERNEL,
	};
	struct reclaim_state reclaim_state = {};

	buf = kzalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, src, len)) {
		kfree(buf);
		return -EFAULT;
	}

	next = buf;
	next[len] = '\0';

	reclaim_state.mm_walk_args = alloc_mm_walk_args();
	if (!reclaim_state.mm_walk_args) {
		kfree(buf);
		return -ENOMEM;
	}

	flags = current->flags & PF_MEMALLOC;
	current->flags |= PF_MEMALLOC;
	current->reclaim_state = &reclaim_state;

	while ((cur = strsep(&next, ",;\n"))) {
		int n;
		int end;
		char cmd;
		unsigned int memcg_id;
		unsigned int nid;
		unsigned long seq;
		unsigned int swappiness = -1;
		unsigned long opt = -1;

		cur = skip_spaces(cur);
		if (!*cur)
			continue;

		n = sscanf(cur, "%c %u %u %lu %n %u %n %lu %n", &cmd, &memcg_id, &nid,
			   &seq, &end, &swappiness, &end, &opt, &end);
		if (n < 4 || cur[end]) {
			err = -EINVAL;
			break;
		}

		err = run_cmd(cmd, memcg_id, nid, &sc, swappiness, seq, opt);
		if (err)
			break;
	}

	current->reclaim_state = NULL;
	current->flags &= ~PF_MEMALLOC;
	current->flags |= flags;

	free_mm_walk_args(reclaim_state.mm_walk_args);
	kfree(buf);

	return err ? : len;
}

static int lru_gen_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &lru_gen_seq_ops);
}

static const struct file_operations lru_gen_rw_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.write = lru_gen_seq_write,
	.llseek = seq_lseek,
	.release = seq_release,
};

static const struct file_operations lru_gen_ro_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

/******************************************************************************
 *                          initialization
 ******************************************************************************/

void lru_gen_init_state(struct mem_cgroup *memcg, struct lruvec *lruvec)
{
	int i;
	int gen, type, zone;
	struct lrugen *lrugen = &lruvec->evictable;
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	lrugen->max_seq = MIN_NR_GENS + 1;
	lrugen->enabled[0] = lru_gen_enabled() && lru_gen_nr_swapfiles;
	lrugen->enabled[1] = lru_gen_enabled();

	for (i = 0; i <= MIN_NR_GENS + 1; i++)
		lrugen->timestamps[i] = jiffies;

	for_each_gen_type_zone(gen, type, zone)
		INIT_LIST_HEAD(&lrugen->lists[gen][type][zone]);

	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG) && !memcg)
		spin_lock(&mm_list->lock);

	lruvec->mm_walk.seq = MIN_NR_GENS;
	lruvec->mm_walk.head = &mm_list->fifo;
	lruvec->mm_walk.tail = &mm_list->fifo;
	init_waitqueue_head(&lruvec->mm_walk.wait);

	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG) && !memcg)
		spin_unlock(&mm_list->lock);
}

#ifdef CONFIG_MEMCG
void lru_gen_init_memcg(struct mem_cgroup *memcg)
{
	int nid;

	INIT_LIST_HEAD(&memcg->mm_list.fifo);
	spin_lock_init(&memcg->mm_list.lock);

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(nid, memcg);

		lru_gen_init_state(memcg, lruvec);
	}
}

void lru_gen_free_memcg(struct mem_cgroup *memcg)
{
	int nid;

	for_each_node(nid) {
		int i;
		struct lruvec *lruvec = get_lruvec(nid, memcg);

		for (i = 0; i < NR_BLOOM_FILTERS; i++) {
			bitmap_free(lruvec->mm_walk.filters[i]);
			lruvec->mm_walk.filters[i] = NULL;
		}
	}
}
#endif

static int __init init_lru_gen(void)
{
	BUILD_BUG_ON(MIN_NR_GENS + 1 >= MAX_NR_GENS);
	BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS);
	BUILD_BUG_ON(sizeof(MM_STAT_CODES) != NR_MM_STATS + 1);

	if (sysfs_create_group(mm_kobj, &lru_gen_attr_group))
		pr_err("lru_gen: failed to create sysfs group\n");

	debugfs_create_file("lru_gen", 0644, NULL, NULL, &lru_gen_rw_fops);
	debugfs_create_file("lru_gen_full", 0444, NULL, NULL, &lru_gen_ro_fops);

	return 0;
};
late_initcall(init_lru_gen);

#else

static void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
}

static unsigned long lru_gen_shrink_lruvec(struct lruvec *lruvec,
					   struct scan_control *sc)
{
	return 0;
}

#endif /* CONFIG_LRU_GEN */

/*
 * This is a basic per-node page freer.  Used by both kswapd and direct reclaim.
 */
static void shrink_node_memcg(struct pglist_data *pgdat, struct mem_cgroup *memcg,
			      struct scan_control *sc, unsigned long *lru_pages)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	int swappiness = mem_cgroup_swappiness(memcg);
	unsigned long nr[NR_LRU_LISTS];
	unsigned long targets[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	struct blk_plug plug;
	bool scan_adjusted;

#ifdef CONFIG_LRU_GEN
	/*
	 * get_scan_count() is what normally fills *lru_pages (the denominator
	 * of shrink_slab()'s pressure model). MGLRU does its own scan
	 * accounting and does not reach it, so hand back what it did scan --
	 * leaving *lru_pages unset would read uninitialised stack here.
	 */
	if (lru_gen_enabled()) {
		*lru_pages = lru_gen_shrink_lruvec(lruvec, sc, memcg);
		return;
	}
#endif

	get_scan_count(lruvec, swappiness, sc, nr, lru_pages);

	/* Record the original scan target for proportional adjustments later */
	memcpy(targets, nr, sizeof(nr));

	/*
	 * Global reclaiming within direct reclaim at DEF_PRIORITY is a normal
	 * event that can occur when there is little memory pressure e.g.
	 * multiple streaming readers/writers. Hence, we do not abort scanning
	 * when the requested number of pages are reclaimed when scanning at
	 * DEF_PRIORITY on the assumption that the fact we are direct
	 * reclaiming implies that kswapd is not keeping up and it is best to
	 * do a batch of work at once. For memcg reclaim one check is made to
	 * abort proportional reclaim if either the file or anon lru has already
	 * dropped to zero at the first pass.
	 */
	scan_adjusted = (global_reclaim(sc) && !current_is_kswapd() &&
			 sc->priority == DEF_PRIORITY);

	blk_start_plug(&plug);
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		unsigned long nr_anon, nr_file, percentage;
		unsigned long nr_scanned;

		for_each_evictable_lru(lru) {
			if (nr[lru]) {
				nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;

				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
			}
		}

		if (nr_reclaimed < nr_to_reclaim || scan_adjusted)
			continue;

		/*
		 * For kswapd and memcg, reclaim at least the number of pages
		 * requested. Ensure that the anon and file LRUs are scanned
		 * proportionally what was requested by get_scan_count(). We
		 * stop reclaiming one LRU and reduce the amount scanning
		 * proportional to the original scan target.
		 */
		nr_file = nr[LRU_INACTIVE_FILE] + nr[LRU_ACTIVE_FILE];
		nr_anon = nr[LRU_INACTIVE_ANON] + nr[LRU_ACTIVE_ANON];

		/*
		 * It's just vindictive to attack the larger once the smaller
		 * has gone to zero.  And given the way we stop scanning the
		 * smaller below, this makes sure that we only make one nudge
		 * towards proportionality once we've got nr_to_reclaim.
		 */
		if (!nr_file || !nr_anon)
			break;

		if (nr_file > nr_anon) {
			unsigned long scan_target = targets[LRU_INACTIVE_ANON] +
						targets[LRU_ACTIVE_ANON] + 1;
			lru = LRU_BASE;
			percentage = nr_anon * 100 / scan_target;
		} else {
			unsigned long scan_target = targets[LRU_INACTIVE_FILE] +
						targets[LRU_ACTIVE_FILE] + 1;
			lru = LRU_FILE;
			percentage = nr_file * 100 / scan_target;
		}

		/* Stop scanning the smaller of the LRU */
		nr[lru] = 0;
		nr[lru + LRU_ACTIVE] = 0;

		/*
		 * Recalculate the other LRU scan count based on its original
		 * scan target and the percentage scanning already complete
		 */
		lru = (lru == LRU_FILE) ? LRU_BASE : LRU_FILE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		lru += LRU_ACTIVE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		scan_adjusted = true;
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	if (inactive_anon_is_low(lruvec))
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);

	throttle_vm_writeout(sc->gfp_mask);
}

/* Use reclaim/compaction for costly allocs or under memory pressure */
static bool in_reclaim_compaction(struct scan_control *sc)
{
	if (IS_ENABLED(CONFIG_COMPACTION) && sc->order &&
			(sc->order > PAGE_ALLOC_COSTLY_ORDER ||
			 sc->priority < DEF_PRIORITY - 2))
		return true;

	return false;
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_zone() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
static inline bool should_continue_reclaim(struct pglist_data *pgdat,
					unsigned long nr_reclaimed,
					unsigned long nr_scanned,
					struct scan_control *sc)
{
	unsigned long pages_for_compaction;
	unsigned long inactive_lru_pages;
	int z;

	/* If not in reclaim/compaction mode, stop */
	if (!in_reclaim_compaction(sc))
		return false;

	/* Consider stopping depending on scan and reclaim activity */
	if (sc->gfp_mask & __GFP_REPEAT) {
		/*
		 * For __GFP_REPEAT allocations, stop reclaiming if the
		 * full LRU list has been scanned and we are still failing
		 * to reclaim pages. This full LRU scan is potentially
		 * expensive but a __GFP_REPEAT caller really wants to succeed
		 */
		if (!nr_reclaimed && !nr_scanned)
			return false;
	} else {
		/*
		 * For non-__GFP_REPEAT allocations which can presumably
		 * fail without consequence, stop if we failed to reclaim
		 * any pages from the last SWAP_CLUSTER_MAX number of
		 * pages that were scanned. This will return to the
		 * caller faster at the risk reclaim/compaction and
		 * the resulting allocation attempt fails
		 */
		if (!nr_reclaimed)
			return false;
	}

	/*
	 * If we have not reclaimed enough pages for compaction and the
	 * inactive lists are large enough, continue reclaiming
	 */
	pages_for_compaction = (2UL << sc->order);
	inactive_lru_pages = node_page_state_pgdat(pgdat, NR_INACTIVE_FILE);
	if (get_nr_swap_pages() > 0)
		inactive_lru_pages += node_page_state_pgdat(pgdat, NR_INACTIVE_ANON);
	if (sc->nr_reclaimed < pages_for_compaction &&
			inactive_lru_pages > pages_for_compaction)
		return true;

	/* If compaction would go ahead or the allocation would succeed, stop */
	for (z = 0; z <= sc->reclaim_idx; z++) {
		struct zone *zone = &pgdat->node_zones[z];
		if (!populated_zone(zone))
			continue;

		switch (compaction_suitable(zone, sc->order, 0, sc->reclaim_idx)) {
		case COMPACT_PARTIAL:
		case COMPACT_CONTINUE:
			return false;
		default:
			/* check next zone */
			;
		}
	}
	return true;
}

static bool shrink_node(pg_data_t *pgdat, struct scan_control *sc,
			enum zone_type classzone_idx)
{
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_reclaimed, nr_scanned;
	bool reclaimable = false;

	do {
		struct mem_cgroup *root = sc->target_mem_cgroup;
		struct mem_cgroup_reclaim_cookie reclaim = {
			.zone = &pgdat->node_zones[classzone_idx],
			.priority = sc->priority,
		};
		unsigned long node_lru_pages = 0;
		struct mem_cgroup *memcg;

		nr_reclaimed = sc->nr_reclaimed;
		nr_scanned = sc->nr_scanned;

		memcg = mem_cgroup_iter(root, NULL, &reclaim);
		do {
			unsigned long lru_pages;
			unsigned long scanned;
			if (mem_cgroup_low(root, memcg)) {
				if (!sc->may_thrash)
					continue;
				mem_cgroup_events(memcg, MEMCG_LOW, 1);
			}

			scanned = sc->nr_scanned;

			shrink_node_memcg(pgdat, memcg, sc, &lru_pages);
			node_lru_pages += lru_pages;

			if (!global_reclaim(sc))
				shrink_slab(sc->gfp_mask, pgdat->node_id,
					    memcg, sc->nr_scanned - scanned,
					    lru_pages);

			/*
			 * Direct reclaim and kswapd have to scan all memory
			 * cgroups to fulfill the overall scan target for the
			 * node.
			 *
			 * Limit reclaim, on the other hand, only cares about
			 * nr_to_reclaim pages to be reclaimed and it will
			 * retry with decreasing priority if one round over the
			 * whole hierarchy is not sufficient.
			 */
			if (!global_reclaim(sc) &&
					sc->nr_reclaimed >= sc->nr_to_reclaim) {
				mem_cgroup_iter_break(root, memcg);
				break;
			}
		} while ((memcg = mem_cgroup_iter(root, memcg, &reclaim)));

		/*
		 * Shrink the slab caches in the same proportion that
		 * the eligible LRU pages were scanned.
		 */
		if (global_reclaim(sc))
			shrink_slab(sc->gfp_mask, pgdat->node_id, NULL,
				    sc->nr_scanned - nr_scanned,
				    node_lru_pages);

		/*
		 * Record the subtree's reclaim efficiency. The reclaimed
		 * pages from slab is excluded here because the corresponding
		 * scanned pages is not accounted. Moreover, freeing a page
		 * by slab shrinking depends on each slab's object population,
		 * making the cost model (i.e. scan:free) different from that
		 * of LRU.
		 */
		vmpressure(sc->gfp_mask, sc->target_mem_cgroup,
			   sc->nr_scanned - nr_scanned,
			   sc->nr_reclaimed - nr_reclaimed);

		if (reclaim_state) {
			sc->nr_reclaimed += reclaim_state->reclaimed_slab;
			reclaim_state->reclaimed_slab = 0;
		}

		if (sc->nr_reclaimed - nr_reclaimed)
			reclaimable = true;

	} while (should_continue_reclaim(pgdat, sc->nr_reclaimed - nr_reclaimed,
					 sc->nr_scanned - nr_scanned, sc));

	return reclaimable;
}

/*
 * Returns true if compaction should go ahead for a high-order request, or
 * the high-order allocation would succeed without compaction.
 */
static inline bool compaction_ready(struct zone *zone, int order)
{
	unsigned long watermark;
	bool watermark_ok;

	/*
	 * Compaction takes time to run and there are potentially other
	 * callers using the pages just freed. Continue reclaiming until
	 * there is a buffer of free pages available to give compaction
	 * a reasonable chance of completing and allocating the page
	 */
	watermark = high_wmark_pages(zone) + (2UL << order);
	watermark_ok = zone_watermark_ok_safe(zone, 0, watermark,
					      zone_idx(zone));

	/*
	 * If compaction is deferred, reclaim up to a point where
	 * compaction will have a chance of success when re-enabled
	 */
	if (compaction_deferred(zone, order))
		return watermark_ok;

	/*
	 * If compaction is not ready to start and allocation is not likely
	 * to succeed without it, then keep reclaiming.
	 */
	if (compaction_suitable(zone, order, 0, 0) == COMPACT_SKIPPED)
		return false;

	return watermark_ok;
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 *
 * Returns true if a zone was reclaimable.
 */
static bool shrink_zones(struct zonelist *zonelist, struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	unsigned long lru_pages = 0;
	bool reclaimable = false;
	gfp_t orig_mask;
	enum zone_type classzone_idx;
	pg_data_t *last_pgdat = NULL;

	/*
	 * If the number of buffer_heads in the machine exceeds the maximum
	 * allowed level, force direct reclaim to scan the highmem zone as
	 * highmem pages could be pinning lowmem pages storing buffer_heads
	 */
	orig_mask = sc->gfp_mask;
	if (buffer_heads_over_limit) {
		sc->gfp_mask |= __GFP_HIGHMEM;
		sc->reclaim_idx = classzone_idx = gfp_zone(sc->gfp_mask);
	}

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					sc->reclaim_idx, sc->nodemask) {
		if (!populated_zone(zone))
			continue;

		/*
		 * Note that reclaim_idx does not change as it is the highest
		 * zone reclaimed from which for empty zones is a no-op but
		 * classzone_idx is used by shrink_node to test if the slabs
		 * should be shrunk on a given node.
		 */
		classzone_idx = sc->reclaim_idx;
		while (!populated_zone(zone->zone_pgdat->node_zones +
							classzone_idx))
			classzone_idx--;

		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		if (global_reclaim(sc)) {
			lru_pages += zone_reclaimable_pages(zone);
			if (!cpuset_zone_allowed(zone,
						 GFP_KERNEL | __GFP_HARDWALL))
				continue;

			if (sc->priority != DEF_PRIORITY &&
			    !pgdat_reclaimable(zone->zone_pgdat))
				continue;	/* Let kswapd poll it */

			/*
			 * If we already have plenty of memory free for
			 * compaction in this zone, don't free any more.
			 * Even though compaction is invoked for any
			 * non-zero order, only frequent costly order
			 * reclamation is disruptive enough to become a
			 * noticeable problem, like transparent huge
			 * page allocations.
			 */
			if (IS_ENABLED(CONFIG_COMPACTION) &&
			    sc->order > PAGE_ALLOC_COSTLY_ORDER &&
			    zonelist_zone_idx(z) <= classzone_idx &&
			    compaction_ready(zone, sc->order)) {
				sc->compaction_ready = true;
				continue;
			}

			/*
			 * Shrink each node in the zonelist once. If the
			 * zonelist is ordered by zone (not the default) then a
			 * node may be shrunk multiple times but in that case
			 * the user prefers lower zones being preserved.
			 */
			if (zone->zone_pgdat == last_pgdat)
				continue;

			/*
			 * This steals pages from memory cgroups over softlimit
			 * and returns the number of reclaimed pages and
			 * scanned pages. This works for global memory pressure
			 * and balancing, not for a memcg's limit.
			 */
			nr_soft_scanned = 0;
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone,
						sc->order, sc->gfp_mask,
						&nr_soft_scanned);
			sc->nr_reclaimed += nr_soft_reclaimed;
			sc->nr_scanned += nr_soft_scanned;
			if (nr_soft_reclaimed)
				reclaimable = true;
			/* need some check for avoid more shrink_zone() */
		}

		/* See comment about same check for global reclaim above */
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;
		shrink_node(zone->zone_pgdat, sc, classzone_idx);
	}

	if (global_reclaim(sc))
		shrink_slab_lmk(sc->gfp_mask, 0, NULL,
				sc->nr_scanned, lru_pages);
	/*
	 * Restore to original mask to avoid the impact on the caller if we
	 * promoted it to __GFP_HIGHMEM.
	 */
	sc->gfp_mask = orig_mask;

	return reclaimable;
}

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					  struct scan_control *sc)
{
	int initial_priority = sc->priority;
	unsigned long total_scanned = 0;
	unsigned long writeback_threshold;
	bool zones_reclaimable;
retry:
	delayacct_freepages_start();

	if (global_reclaim(sc))
		count_vm_event(ALLOCSTALL);

	do {
		vmpressure_prio(sc->gfp_mask, sc->target_mem_cgroup,
				sc->priority);
		sc->nr_scanned = 0;
		zones_reclaimable = shrink_zones(zonelist, sc);

		total_scanned += sc->nr_scanned;
		if (sc->nr_reclaimed >= sc->nr_to_reclaim)
			break;

		if (sc->compaction_ready)
			break;

		/*
		 * If we're getting trouble reclaiming, start doing
		 * writepage even in laptop mode.
		 */
		if (sc->priority < DEF_PRIORITY - 2)
			sc->may_writepage = 1;

		/*
		 * Try to write back as many pages as we just scanned.  This
		 * tends to cause slow streaming writers to write data to the
		 * disk smoothly, at the dirtying rate, which is nice.   But
		 * that's undesirable in laptop mode, where we *want* lumpy
		 * writeout.  So in laptop mode, write out the whole world.
		 */
		writeback_threshold = sc->nr_to_reclaim + sc->nr_to_reclaim / 2;
		if (total_scanned > writeback_threshold) {
			wakeup_flusher_threads(laptop_mode ? 0 : total_scanned,
						WB_REASON_TRY_TO_FREE_PAGES);
			sc->may_writepage = 1;
		}
	} while (--sc->priority >= 0);

	delayacct_freepages_end();

	if (sc->nr_reclaimed)
		return sc->nr_reclaimed;

	/* Aborted reclaim to try compaction? don't OOM, then */
	if (sc->compaction_ready)
		return 1;

	/* Untapped cgroup reserves?  Don't OOM, retry. */
	if (!sc->may_thrash) {
		sc->priority = initial_priority;
		sc->may_thrash = 1;
		goto retry;
	}

	/* Any of the zones still reclaimable?  Don't OOM. */
	if (zones_reclaimable)
		return 1;

	return 0;
}

static bool pfmemalloc_watermark_ok(pg_data_t *pgdat)
{
	struct zone *zone;
	unsigned long pfmemalloc_reserve = 0;
	unsigned long free_pages = 0;
	int i;
	bool wmark_ok;

	for (i = 0; i <= ZONE_NORMAL; i++) {
		zone = &pgdat->node_zones[i];
		if (!populated_zone(zone) ||
		    pgdat_reclaimable_pages(pgdat) == 0)
			continue;

		pfmemalloc_reserve += min_wmark_pages(zone);
		free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}

	/* If there are no reserves (unexpected config) then do not throttle */
	if (!pfmemalloc_reserve)
		return true;

	wmark_ok = free_pages > pfmemalloc_reserve / 2;

	/* kswapd must be awake if processes are being throttled */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
		pgdat->kswapd_classzone_idx = min(pgdat->kswapd_classzone_idx,
						(enum zone_type)ZONE_NORMAL);
		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

/*
 * Throttle direct reclaimers if backing storage is backed by the network
 * and the PFMEMALLOC reserve for the preferred node is getting dangerously
 * depleted. kswapd will continue to make progress and wake the processes
 * when the low watermark is reached.
 *
 * Returns true if a fatal signal was delivered during throttling. If this
 * happens, the page allocator should not consider triggering the OOM killer.
 */
static bool throttle_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
					nodemask_t *nodemask)
{
	struct zoneref *z;
	struct zone *zone;
	pg_data_t *pgdat = NULL;

	/*
	 * Kernel threads should not be throttled as they may be indirectly
	 * responsible for cleaning pages necessary for reclaim to make forward
	 * progress. kjournald for example may enter direct reclaim while
	 * committing a transaction where throttling it could forcing other
	 * processes to block on log_wait_commit().
	 */
	if (current->flags & PF_KTHREAD)
		goto out;

	/*
	 * If a fatal signal is pending, this process should not throttle.
	 * It should return quickly so it can exit and free its memory
	 */
	if (fatal_signal_pending(current))
		goto out;

	/*
	 * Check if the pfmemalloc reserves are ok by finding the first node
	 * with a usable ZONE_NORMAL or lower zone. The expectation is that
	 * GFP_KERNEL will be required for allocating network buffers when
	 * swapping over the network so ZONE_HIGHMEM is unusable.
	 *
	 * Throttling is based on the first usable node and throttled processes
	 * wait on a queue until kswapd makes progress and wakes them. There
	 * is an affinity then between processes waking up and where reclaim
	 * progress has been made assuming the process wakes on the same node.
	 * More importantly, processes running on remote nodes will not compete
	 * for remote pfmemalloc reserves and processes on different nodes
	 * should make reasonable progress.
	 */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(gfp_mask), nodemask) {
		if (zone_idx(zone) > ZONE_NORMAL)
			continue;

		/* Throttle based on the first usable node */
		pgdat = zone->zone_pgdat;
		if (pfmemalloc_watermark_ok(pgdat))
			goto out;
		break;
	}

	/* If no zone was usable by the allocation flags then do not throttle */
	if (!pgdat)
		goto out;

	/* Account for the throttling */
	count_vm_event(PGSCAN_DIRECT_THROTTLE);

	/*
	 * If the caller cannot enter the filesystem, it's possible that it
	 * is due to the caller holding an FS lock or performing a journal
	 * transaction in the case of a filesystem like ext[3|4]. In this case,
	 * it is not safe to block on pfmemalloc_wait as kswapd could be
	 * blocked waiting on the same lock. Instead, throttle for up to a
	 * second before continuing.
	 */
	if (!(gfp_mask & __GFP_FS)) {
		wait_event_interruptible_timeout(pgdat->pfmemalloc_wait,
			pfmemalloc_watermark_ok(pgdat), HZ);

		goto check_pending;
	}

	/* Throttle until kswapd wakes the process */
	wait_event_killable(zone->zone_pgdat->pfmemalloc_wait,
		pfmemalloc_watermark_ok(pgdat));

check_pending:
	if (fatal_signal_pending(current))
		return true;

out:
	return false;
}

unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned long nr_reclaimed;
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.gfp_mask = (gfp_mask = memalloc_noio_flags(gfp_mask)),
		.reclaim_idx = gfp_zone(gfp_mask),
		.order = order,
		.nodemask = nodemask,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
	};

	/*
	 * Do not enter reclaim if fatal signal was delivered while throttled.
	 * 1 is returned so that the page allocator does not OOM kill at this
	 * point.
	 */
	if (throttle_direct_reclaim(gfp_mask, zonelist, nodemask))
		return 1;

	trace_mm_vmscan_direct_reclaim_begin(order,
				sc.may_writepage,
				gfp_mask);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	trace_mm_vmscan_direct_reclaim_end(nr_reclaimed);

	return nr_reclaimed;
}

#ifdef CONFIG_MEMCG

unsigned long mem_cgroup_shrink_node(struct mem_cgroup *memcg,
						gfp_t gfp_mask, bool noswap,
						struct zone *zone,
						unsigned long *nr_scanned)
{
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.target_mem_cgroup = memcg,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.may_swap = !noswap,
	};
	int swappiness = mem_cgroup_swappiness(memcg);
	unsigned long lru_pages;

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);

	trace_mm_vmscan_memcg_softlimit_reclaim_begin(sc.order,
						      sc.may_writepage,
						      sc.gfp_mask);

	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_node from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_node_memcg(zone->zone_pgdat, memcg, &sc, &lru_pages);

	trace_mm_vmscan_memcg_softlimit_reclaim_end(sc.nr_reclaimed);

	*nr_scanned = sc.nr_scanned;
	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   unsigned long nr_pages,
					   gfp_t gfp_mask,
					   bool may_swap)
{
	struct zonelist *zonelist;
	unsigned long nr_reclaimed;
	int nid;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
				(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK),
		.reclaim_idx = MAX_NR_ZONES - 1,
		.target_mem_cgroup = memcg,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = may_swap,
	};

	/*
	 * Unlike direct reclaim via alloc_pages(), memcg's reclaim doesn't
	 * take care of from where we get pages. So the node where we start the
	 * scan does not need to be the current node.
	 */
	nid = mem_cgroup_select_victim_node(memcg);

	zonelist = NODE_DATA(nid)->node_zonelists;

	trace_mm_vmscan_memcg_reclaim_begin(0,
					    sc.may_writepage,
					    sc.gfp_mask);

	current->flags |= PF_MEMALLOC;
	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);
	current->flags &= ~PF_MEMALLOC;

	trace_mm_vmscan_memcg_reclaim_end(nr_reclaimed);

	return nr_reclaimed;
}
#endif

static void age_active_anon(struct pglist_data *pgdat,
				struct zone *zone, struct scan_control *sc)
{
	struct mem_cgroup *memcg;

#ifdef CONFIG_LRU_GEN
	if (lru_gen_enabled()) {
		lru_gen_age_node(pgdat, sc);
		return;
	}
#endif

	if (!total_swap_pages)
		return;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);

		if (inactive_anon_is_low(lruvec))
			shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
					   sc, LRU_ACTIVE_ANON);

		memcg = mem_cgroup_iter(NULL, memcg, NULL);
	} while (memcg);
}

static bool zone_balanced(struct zone *zone, int order, int classzone_idx)
{
	unsigned long mark = high_wmark_pages(zone);

	if (!zone_watermark_ok_safe(zone, order, mark, classzone_idx))
		return false;

	/*
	 * If any eligible zone is balanced then the node is not considered
	 * to be congested or dirty
	 */
	clear_bit(PGDAT_CONGESTED, &zone->zone_pgdat->flags);
	clear_bit(PGDAT_DIRTY, &zone->zone_pgdat->flags);

	return true;
}

/*
 * Prepare kswapd for sleeping. This verifies that there are no processes
 * waiting in throttle_direct_reclaim() and that watermarks have been met.
 *
 * Returns true if kswapd is ready to sleep
 */
static bool prepare_kswapd_sleep(pg_data_t *pgdat, int order, long remaining,
					int classzone_idx)
{
	int i;

	/* If a direct reclaimer woke kswapd within HZ/10, it's premature */
	if (remaining)
		return false;

	/*
	 * The throttled processes are normally woken up in balance_pgdat() as
	 * soon as pfmemalloc_watermark_ok() is true. But there is a potential
	 * race between when kswapd checks the watermarks and a process gets
	 * throttled. There is also a potential race if processes get
	 * throttled, kswapd wakes, a large process exits thereby balancing the
	 * zones, which causes kswapd to exit balance_pgdat() before reaching
	 * the wake up checks. If kswapd is going to sleep, no process should
	 * be sleeping on pfmemalloc_wait, so wake them now if necessary. If
	 * the wake up is premature, processes will wake kswapd and get
	 * throttled again. The difference from wake ups in balance_pgdat() is
	 * that here we are under prepare_to_wait().
	 */
	if (waitqueue_active(&pgdat->pfmemalloc_wait))
		wake_up_all(&pgdat->pfmemalloc_wait);

	for (i = 0; i <= classzone_idx; i++) {
		struct zone *zone = pgdat->node_zones + i;

		if (!populated_zone(zone))
			continue;

		if (!zone_balanced(zone, order, classzone_idx))
			return false;
	}

	return true;
}

/*
 * kswapd shrinks a node of pages that are at or below the highest usable
 * zone that is currently unbalanced.
 *
 * Returns true if kswapd scanned at least the requested number of pages to
 * reclaim or if the lack of progress was due to pages under writeback.
 * This is used to determine if the scanning priority needs to be raised.
 */
static bool kswapd_shrink_node(pg_data_t *pgdat,
			       int classzone_idx,
			       struct scan_control *sc)
{
	struct zone *zone;
	int z;

	/* Reclaim a number of pages proportional to the number of zones */
	sc->nr_to_reclaim = 0;
	for (z = 0; z <= classzone_idx; z++) {
		zone = pgdat->node_zones + z;
		if (!populated_zone(zone))
			continue;

		sc->nr_to_reclaim += max(high_wmark_pages(zone), SWAP_CLUSTER_MAX);
	}

	/*
	 * Historically care was taken to put equal pressure on all zones but
	 * now pressure is applied based on node LRU order.
	 */
	shrink_node(pgdat, sc, classzone_idx);

	/*
	 * Fragmentation may mean that the system cannot be rebalanced for
	 * high-order allocations. If twice the allocation size has been
	 * reclaimed then recheck watermarks only at order-0 to prevent
	 * excessive reclaim. Assume that a process requested a high-order
	 * can direct reclaim/compact.
	 */
	if (sc->order && sc->nr_reclaimed >= 2UL << sc->order)
		sc->order = 0;

	return sc->nr_scanned >= sc->nr_to_reclaim;
}

/*
 * For kswapd, balance_pgdat() will reclaim pages across a node from zones
 * that are eligible for use by the caller until at least one zone is
 * balanced.
 *
 * Returns the order kswapd finished reclaiming at.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), any page is that zone
 * or lower is eligible for reclaim until at least one usable zone is
 * balanced.
 */
static int balance_pgdat(pg_data_t *pgdat, int order, int classzone_idx)
{
	int i;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	struct zone *zone = NULL;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
		.reclaim_idx = classzone_idx,
	};
	count_vm_event(PAGEOUTRUN);

	do {
		bool raise_priority = true;
		unsigned long lru_pages = 0;

		sc.nr_reclaimed = 0;

		/*
		 * If the number of buffer_heads in the machine exceeds the
		 * maximum allowed level then reclaim from all zones. This is
		 * not specific to highmem as highmem may not exist but it is
		 * it is expected that buffer_heads are stripped in writeback.
		 */
		if (buffer_heads_over_limit) {
			for (i = MAX_NR_ZONES - 1; i >= 0; i--) {
				zone = pgdat->node_zones + i;
				if (!populated_zone(zone))
					continue;

				classzone_idx = i;
				break;
			}
		}

		/*
		 * Only reclaim if there are no eligible zones. Check from
		 * high to low zone as allocations prefer higher zones.
		 * Scanning from low to high zone would allow congestion to be
		 * cleared during a very small window when a small low
		 * zone was balanced even under extreme pressure when the
		 * overall node may be congested.
		 */
		for (i = classzone_idx; i >= 0; i--) {
			zone = pgdat->node_zones + i;
			if (!populated_zone(zone))
				continue;

			if (zone_balanced(zone, sc.order, classzone_idx))
				goto out;
		}

		/*
		 * Do some background aging of the anon list, to give
		 * pages a chance to be referenced before reclaiming. All
		 * pages are rotated regardless of classzone as this is
		 * about consistent aging.
		 */
		age_active_anon(pgdat, &pgdat->node_zones[MAX_NR_ZONES - 1], &sc);

		/*
		 * If we're getting trouble reclaiming, start doing writepage
		 * even in laptop mode.
		 */
		if (sc.priority < DEF_PRIORITY - 2 || !pgdat_reclaimable(pgdat))
			sc.may_writepage = 1;

		/*
		 * Call soft limit reclaim before calling shrink_node.
		 * zone is NULL when the loop above found no populated zone
		 * up to classzone_idx; mem_cgroup_soft_limit_reclaim()
		 * dereferences it via zone_to_nid()/zone_idx(), so it must
		 * not be entered in that case.
		 */
		sc.nr_scanned = 0;
		nr_soft_scanned = 0;
		if (zone)
			nr_soft_reclaimed =
				mem_cgroup_soft_limit_reclaim(zone, sc.order,
						sc.gfp_mask, &nr_soft_scanned);
		else
			nr_soft_reclaimed = 0;
		sc.nr_reclaimed += nr_soft_reclaimed;

		/*
		 * There should be no need to raise the scanning priority if
		 * enough pages are already being scanned that that high
		 * watermark would be met at 100% efficiency.
		 */
		if (kswapd_shrink_node(pgdat, classzone_idx, &sc))
			raise_priority = false;

		/*
		 * If the low watermark is met there is no need for processes
		 * to be throttled on pfmemalloc_wait as they should not be
		 * able to safely make forward progress. Wake them
		 */
		if (waitqueue_active(&pgdat->pfmemalloc_wait) &&
				pfmemalloc_watermark_ok(pgdat))
			wake_up_all(&pgdat->pfmemalloc_wait);

		/* Check if kswapd should be suspending */
		if (try_to_freeze() || kthread_should_stop())
			break;

		/*
		 * Raise priority if scanning rate is too low or there was no
		 * progress in reclaiming pages
		 */
		if (raise_priority || !sc.nr_reclaimed)
			sc.priority--;
	} while (sc.priority >= 1);

out:
	/*
	 * Return the order kswapd stopped reclaiming at as
	 * prepare_kswapd_sleep() takes it into account. If another caller
	 * entered the allocator slow path while kswapd was awake, order will
	 * remain at the higher level.
	 */
	return sc.order;
}

static void kswapd_try_to_sleep(pg_data_t *pgdat, int alloc_order, int reclaim_order,
				unsigned int classzone_idx)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);

	/* Try to sleep for a short interval */
	if (prepare_kswapd_sleep(pgdat, reclaim_order, remaining, classzone_idx)) {
		/*
		 * Compaction records what page blocks it recently failed to
		 * isolate pages from and skips them in the future scanning.
		 * When kswapd is going to sleep, it is reasonable to assume
		 * that pages and compaction may succeed so reset the cache.
		 */
		reset_isolation_suitable(pgdat);

		/*
		 * We have freed the memory, now we should compact it to make
		 * allocation of the requested order possible.
		 */
		wakeup_kcompactd(pgdat, alloc_order, classzone_idx);

		remaining = schedule_timeout(HZ/10);

		/*
		 * If woken prematurely then reset kswapd_classzone_idx and
		 * order. The values will either be from a wakeup request or
		 * the previous request that slept prematurely.
		 */
		if (remaining) {
			pgdat->kswapd_classzone_idx = max(pgdat->kswapd_classzone_idx, classzone_idx);
			pgdat->kswapd_order = max(pgdat->kswapd_order, reclaim_order);
		}

		finish_wait(&pgdat->kswapd_wait, &wait);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
	}

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (prepare_kswapd_sleep(pgdat, reclaim_order, remaining, classzone_idx)) {
		trace_mm_vmscan_kswapd_sleep(pgdat->node_id);

		/*
		 * vmstat counters are not perfectly accurate and the estimated
		 * value for counters such as NR_FREE_PAGES can deviate from the
		 * true value by nr_online_cpus * threshold. To avoid the zone
		 * watermarks being breached while under pressure, we reduce the
		 * per-cpu vmstat threshold while kswapd is awake and restore
		 * them before going back to sleep.
		 */
		set_pgdat_percpu_threshold(pgdat, calculate_normal_threshold);

		if (!kthread_should_stop())
			schedule();

		set_pgdat_percpu_threshold(pgdat, calculate_pressure_threshold);
	} else {
		if (remaining)
			count_vm_event(KSWAPD_LOW_WMARK_HIT_QUICKLY);
		else
			count_vm_event(KSWAPD_HIGH_WMARK_HIT_QUICKLY);
	}
	finish_wait(&pgdat->kswapd_wait, &wait);
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int kswapd(void *p)
{
	unsigned int alloc_order, reclaim_order, classzone_idx;
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;

	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	lockdep_set_current_reclaim_state(GFP_KERNEL);

	if (kswapd_cpu_mask == NULL && !cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);
	current->reclaim_state = &reclaim_state;

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	pgdat->kswapd_order = alloc_order = reclaim_order = 0;
	pgdat->kswapd_classzone_idx = classzone_idx = 0;
	for ( ; ; ) {
		bool ret;

kswapd_try_sleep:
		kswapd_try_to_sleep(pgdat, alloc_order, reclaim_order,
					classzone_idx);

		/* Read the new order and classzone_idx */
		alloc_order = reclaim_order = pgdat->kswapd_order;
		classzone_idx = pgdat->kswapd_classzone_idx;
		pgdat->kswapd_order = 0;
		pgdat->kswapd_classzone_idx = 0;

		ret = try_to_freeze();
		if (kthread_should_stop())
			break;

		/*
		 * We can speed up thawing tasks if we don't call balance_pgdat
		 * after returning from the refrigerator
		 */
		if (ret)
			continue;

		/*
		 * Reclaim begins at the requested order but if a high-order
		 * reclaim fails then kswapd falls back to reclaiming for
		 * order-0. If that happens, kswapd will consider sleeping
		 * for the order it finished reclaiming at (reclaim_order)
		 * but kcompactd is woken to compact for the original
		 * request (alloc_order).
		 */
		trace_mm_vmscan_kswapd_wake(pgdat->node_id, alloc_order);
		reclaim_order = balance_pgdat(pgdat, alloc_order, classzone_idx);
		if (reclaim_order < alloc_order)
			goto kswapd_try_sleep;

		alloc_order = reclaim_order = pgdat->kswapd_order;
		classzone_idx = pgdat->kswapd_classzone_idx;
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD);
	current->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();

	return 0;
}

/*
 * A zone is low on free memory, so wake its kswapd task to service it.
 */
void wakeup_kswapd(struct zone *zone, int order, enum zone_type classzone_idx)
{
	pg_data_t *pgdat;

	if (!populated_zone(zone))
		return;

	if (!cpuset_zone_allowed(zone, GFP_KERNEL | __GFP_HARDWALL))
		return;
	pgdat = zone->zone_pgdat;
	pgdat->kswapd_classzone_idx = max(pgdat->kswapd_classzone_idx, classzone_idx);
	pgdat->kswapd_order = max(pgdat->kswapd_order, order);
	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;
	if (zone_balanced(zone, order, 0))
		return;

	trace_mm_vmscan_wakeup_kswapd(pgdat->node_id, zone_idx(zone), order);
	wake_up_interruptible(&pgdat->kswapd_wait);
}

#ifdef CONFIG_HIBERNATION
/*
 * Try to free `nr_to_reclaim' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_to_reclaim)
{
	struct reclaim_state reclaim_state = { 0 };
	struct scan_control sc = {
		.nr_to_reclaim = nr_to_reclaim,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.hibernation_mode = 1,
	};
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);
	struct task_struct *p = current;
	unsigned long nr_reclaimed;

	p->flags |= PF_MEMALLOC;
	lockdep_set_current_reclaim_state(sc.gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	p->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();
	p->flags &= ~PF_MEMALLOC;

	return nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/* It's optimal to keep kswapds on the same CPUs as their memory, but
   not required for correctness.  So if the last cpu in a node goes
   away, we get changed to run anywhere: as the first one comes back,
   restore their cpu bindings. */
static int cpu_callback(struct notifier_block *nfb, unsigned long action,
			void *hcpu)
{
	int nid;

	if (action == CPU_ONLINE || action == CPU_ONLINE_FROZEN) {
		for_each_node_state(nid, N_MEMORY) {
			pg_data_t *pgdat = NODE_DATA(nid);
			const struct cpumask *mask;

			mask = cpumask_of_node(pgdat->node_id);

			if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
				/* One of our CPUs online: restore mask */
				set_cpus_allowed_ptr(pgdat->kswapd, mask);
		}
	}
	return NOTIFY_OK;
}

static int set_kswapd_cpu_mask(pg_data_t *pgdat)
{
	int ret = 0;
	cpumask_t tmask;

	if (!kswapd_cpu_mask)
		return 0;

	cpumask_clear(&tmask);
	ret = cpumask_parse(kswapd_cpu_mask, &tmask);
	if (ret)
		return ret;

	return set_cpus_allowed_ptr(pgdat->kswapd, &tmask);
}

/*
 * This kswapd start function will be called by init and node-hot-add.
 * On node-hot-add, kswapd will moved to proper cpus if cpus are hot-added.
 */
int kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kswapd)
		return 0;

	pgdat->kswapd = kthread_run(kswapd, pgdat, "kswapd%d", nid);
	if (IS_ERR(pgdat->kswapd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state == SYSTEM_BOOTING);
		pr_err("Failed to start kswapd on node %d\n", nid);
		ret = PTR_ERR(pgdat->kswapd);
		pgdat->kswapd = NULL;
	} else if (kswapd_cpu_mask) {
		if (set_kswapd_cpu_mask(pgdat))
			pr_warn("error setting kswapd cpu affinity mask\n");
	}
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
void kswapd_stop(int nid)
{
	struct task_struct *kswapd = NODE_DATA(nid)->kswapd;

	if (kswapd) {
		kthread_stop(kswapd);
		NODE_DATA(nid)->kswapd = NULL;
	}
}

static int __init kswapd_init(void)
{
	int nid;

	swap_setup();
	for_each_node_state(nid, N_MEMORY)
 		kswapd_run(nid);
	if (kswapd_cpu_mask == NULL)
		hotcpu_notifier(cpu_callback, 0);
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Zone reclaim mode
 *
 * If non-zero call zone_reclaim when the number of free pages falls below
 * the watermarks.
 */
int zone_reclaim_mode __read_mostly;

#define RECLAIM_OFF 0
#define RECLAIM_ZONE (1<<0)	/* Run shrink_inactive_list on the zone */
#define RECLAIM_WRITE (1<<1)	/* Writeout pages during reclaim */
#define RECLAIM_UNMAP (1<<2)	/* Unmap pages during reclaim */

/*
 * Priority for ZONE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define ZONE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for zone_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long zone_unmapped_file_pages(struct zone *zone)
{
	unsigned long file_mapped = zone_page_state(zone, NR_FILE_MAPPED);
	unsigned long file_lru = node_page_state_pgdat(zone->zone_pgdat, NR_INACTIVE_FILE) +
		node_page_state_pgdat(zone->zone_pgdat, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static unsigned long zone_pagecache_reclaimable(struct zone *zone)
{
	unsigned long nr_pagecache_reclaimable;
	unsigned long delta = 0;

	/*
	 * If RECLAIM_UNMAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and zone_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (zone_reclaim_mode & RECLAIM_UNMAP)
		nr_pagecache_reclaimable = zone_page_state(zone, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = zone_unmapped_file_pages(zone);

	/* If we can't clean pages, remove dirty pages from consideration */
	if (!(zone_reclaim_mode & RECLAIM_WRITE))
		delta += zone_page_state(zone, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this zone through reclaim.
 */
static int __zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct task_struct *p = current;
	struct reclaim_state reclaim_state = { 0 };
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = (gfp_mask = memalloc_noio_flags(gfp_mask)),
		.order = order,
		.priority = ZONE_RECLAIM_PRIORITY,
		.may_writepage = !!(zone_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(zone_reclaim_mode & RECLAIM_UNMAP),
		.may_swap = 1,
		.reclaim_idx = zone_idx(zone),
	};

	cond_resched();
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_UNMAP
	 * and we also need to be able to write out pages for RECLAIM_WRITE
	 * and RECLAIM_UNMAP.
	 */
	p->flags |= PF_MEMALLOC | PF_SWAPWRITE;
	lockdep_set_current_reclaim_state(gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	if (zone_pagecache_reclaimable(zone) > zone->min_unmapped_pages) {
		/*
		 * Free memory by calling shrink zone with increasing
		 * priorities until we have enough memory freed.
		 */
		do {
			shrink_node(zone->zone_pgdat, &sc, zone_idx(zone));
		} while (sc.nr_reclaimed < nr_pages && --sc.priority >= 0);
	}

	p->reclaim_state = NULL;
	current->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE);
	lockdep_clear_current_reclaim_state();
	return sc.nr_reclaimed >= nr_pages;
}

int zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	int node_id;
	int ret;

	/*
	 * Zone reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the zone is overallocated. So we do not reclaim
	 * if less than a specified percentage of the zone is used by
	 * unmapped file backed pages.
	 */
	if (zone_pagecache_reclaimable(zone) <= zone->min_unmapped_pages &&
	    zone_page_state(zone, NR_SLAB_RECLAIMABLE) <= zone->min_slab_pages)
		return ZONE_RECLAIM_FULL;

	if (!pgdat_reclaimable(zone->zone_pgdat))
		return ZONE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!gfpflags_allow_blocking(gfp_mask) || (current->flags & PF_MEMALLOC))
		return ZONE_RECLAIM_NOSCAN;

	/*
	 * Only run zone reclaim on the local zone or on zones that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
	node_id = zone_to_nid(zone);
	if (node_state(node_id, N_CPU) && node_id != numa_node_id())
		return ZONE_RECLAIM_NOSCAN;

	if (test_and_set_bit(ZONE_RECLAIM_LOCKED, &zone->flags))
		return ZONE_RECLAIM_NOSCAN;

	ret = __zone_reclaim(zone, gfp_mask, order);
	clear_bit(ZONE_RECLAIM_LOCKED, &zone->flags);

	if (!ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}
#endif

/*
 * page_evictable - test whether a page is evictable
 * @page: the page to test
 *
 * Test whether page is evictable--i.e., should be placed on active/inactive
 * lists vs unevictable list.
 *
 * Reasons page might not be evictable:
 * (1) page's mapping marked unevictable
 * (2) page is part of an mlocked VMA
 *
 */
int page_evictable(struct page *page)
{
	int ret;

	/* Prevent address_space of inode and swap cache from being freed */
	rcu_read_lock();
	ret = !mapping_unevictable(page_mapping(page)) && !PageMlocked(page);
	rcu_read_unlock();
	return ret;
}

#ifdef CONFIG_SHMEM
/**
 * check_move_unevictable_pages - check pages for evictability and move to appropriate zone lru list
 * @pages:	array of pages to check
 * @nr_pages:	number of pages to check
 *
 * Checks pages for evictability and moves them to the appropriate lru list.
 *
 * This function is only used for SysV IPC SHM_UNLOCK.
 */
void check_move_unevictable_pages(struct page **pages, int nr_pages)
{
	struct lruvec *lruvec;
	struct pglist_data *pgdat = NULL;
	int pgscanned = 0;
	int pgrescued = 0;
	int i;

	for (i = 0; i < nr_pages; i++) {
		struct page *page = pages[i];
		struct pglist_data *pagepgdat = page_pgdat(page);

		pgscanned++;
		if (pagepgdat != pgdat) {
			if (pgdat)
				spin_unlock_irq(&pgdat->lru_lock);
			pgdat = pagepgdat;
			spin_lock_irq(&pgdat->lru_lock);
		}
		lruvec = mem_cgroup_page_lruvec(page, pgdat);

		if (!PageLRU(page) || !PageUnevictable(page))
			continue;

		if (page_evictable(page)) {
			enum lru_list lru = page_lru_base_type(page);

			VM_BUG_ON_PAGE(PageActive(page), page);
			ClearPageUnevictable(page);
			del_page_from_lru_list(page, lruvec, LRU_UNEVICTABLE);
			add_page_to_lru_list(page, lruvec, lru);
			pgrescued++;
		}
	}

	if (pgdat) {
		__count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
		__count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
		spin_unlock_irq(&pgdat->lru_lock);
	}
}
#endif /* CONFIG_SHMEM */
