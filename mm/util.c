#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mman.h>
#include <linux/hugetlb.h>
#include <linux/vmalloc.h>
#include <linux/sysctl.h>

#include <asm/sections.h>
#include <asm/uaccess.h>

#include "internal.h"

static inline int is_kernel_rodata(unsigned long addr)
{
	return addr >= (unsigned long)__start_rodata &&
		addr < (unsigned long)__end_rodata;
}

/**
 * kfree_const - conditionally free memory
 * @x: pointer to the memory
 *
 * Function calls kfree only if @x is not in .rodata section.
 */
void kfree_const(const void *x)
{
	if (!is_kernel_rodata((unsigned long)x))
		kfree(x);
}
EXPORT_SYMBOL(kfree_const);

/**
 * kstrdup - allocate space for and copy an existing string
 * @s: the string to duplicate
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 */
char *kstrdup(const char *s, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (!s)
		return NULL;

	len = strlen(s) + 1;
	buf = kmalloc_track_caller(len, gfp);
	if (buf)
		memcpy(buf, s, len);
	return buf;
}
EXPORT_SYMBOL(kstrdup);

/**
 * kstrdup_const - conditionally duplicate an existing const string
 * @s: the string to duplicate
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Function returns source string if it is in .rodata section otherwise it
 * fallbacks to kstrdup.
 * Strings allocated by kstrdup_const should be freed by kfree_const.
 */
const char *kstrdup_const(const char *s, gfp_t gfp)
{
	if (is_kernel_rodata((unsigned long)s))
		return s;

	return kstrdup(s, gfp);
}
EXPORT_SYMBOL(kstrdup_const);

/**
 * kstrndup - allocate space for and copy an existing string
 * @s: the string to duplicate
 * @max: read at most @max chars from @s
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Note: Use kmemdup_nul() instead if the size is known exactly.
 */
char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (!s)
		return NULL;

	len = strnlen(s, max);
	buf = kmalloc_track_caller(len+1, gfp);
	if (buf) {
		memcpy(buf, s, len);
		buf[len] = '\0';
	}
	return buf;
}
EXPORT_SYMBOL(kstrndup);

/**
 * kmemdup - duplicate region of memory
 *
 * @src: memory region to duplicate
 * @len: memory region length
 * @gfp: GFP mask to use
 */
void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p;

	p = kmalloc_track_caller(len, gfp);
	if (p)
		memcpy(p, src, len);
	return p;
}
EXPORT_SYMBOL(kmemdup);

/**
 * kmemdup_nul - Create a NUL-terminated string from unterminated data
 * @s: The data to stringify
 * @len: The size of the data
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 */
char *kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
	char *buf;

	if (!s)
		return NULL;

	buf = kmalloc_track_caller(len + 1, gfp);
	if (buf) {
		memcpy(buf, s, len);
		buf[len] = '\0';
	}
	return buf;
}
EXPORT_SYMBOL(kmemdup_nul);

/**
 * memdup_user - duplicate memory region from user space
 *
 * @src: source address in user space
 * @len: number of bytes to copy
 *
 * Returns an ERR_PTR() on failure.
 */
void *memdup_user(const void __user *src, size_t len)
{
	void *p;

	/*
	 * Always use GFP_KERNEL, since copy_from_user() can sleep and
	 * cause pagefault, which makes it pointless to use GFP_NOFS
	 * or GFP_ATOMIC.
	 */
	p = kmalloc_track_caller(len, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}

	return p;
}
EXPORT_SYMBOL(memdup_user);

/*
 * strndup_user - duplicate an existing string from user space
 * @s: The string to duplicate
 * @n: Maximum number of bytes to copy, including the trailing NUL.
 */
char *strndup_user(const char __user *s, long n)
{
	char *p;
	long length;

	length = strnlen_user(s, n);

	if (!length)
		return ERR_PTR(-EFAULT);

	if (length > n)
		return ERR_PTR(-EINVAL);

	p = memdup_user(s, length);

	if (IS_ERR(p))
		return p;

	p[length - 1] = '\0';

	return p;
}
EXPORT_SYMBOL(strndup_user);

void __vma_link_list(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev, struct rb_node *rb_parent)
{
	struct vm_area_struct *next;

	vma->vm_prev = prev;
	if (prev) {
		next = prev->vm_next;
		prev->vm_next = vma;
	} else {
		mm->mmap = vma;
		if (rb_parent)
			next = rb_entry(rb_parent,
					struct vm_area_struct, vm_rb);
		else
			next = NULL;
	}
	vma->vm_next = next;
	if (next)
		next->vm_prev = vma;
}

/* Check if the vma is being used as a stack by this task */
int vma_is_stack_for_task(struct vm_area_struct *vma, struct task_struct *t)
{
	return (vma->vm_start <= KSTK_ESP(t) && vma->vm_end >= KSTK_ESP(t));
}

#if defined(CONFIG_MMU) && !defined(HAVE_ARCH_PICK_MMAP_LAYOUT)
void arch_pick_mmap_layout(struct mm_struct *mm)
{
	mm->mmap_base = TASK_UNMAPPED_BASE;
	mm->get_unmapped_area = arch_get_unmapped_area;
}
#endif

/*
 * Like get_user_pages_fast() except its IRQ-safe in that it won't fall
 * back to the regular GUP.
 * If the architecture not support this function, simply return with no
 * page pinned
 */
int __weak __get_user_pages_fast(unsigned long start,
				 int nr_pages, int write, struct page **pages)
{
	return 0;
}
EXPORT_SYMBOL_GPL(__get_user_pages_fast);

/**
 * get_user_pages_fast() - pin user pages in memory
 * @start:	starting user address
 * @nr_pages:	number of pages from start to pin
 * @write:	whether pages will be written to
 * @pages:	array that receives pointers to the pages pinned.
 *		Should be at least nr_pages long.
 *
 * Returns number of pages pinned. This may be fewer than the number
 * requested. If nr_pages is 0 or negative, returns 0. If no pages
 * were pinned, returns -errno.
 *
 * get_user_pages_fast provides equivalent functionality to get_user_pages,
 * operating on current and current->mm, with force=0 and vma=NULL. However
 * unlike get_user_pages, it must be called without mmap_sem held.
 *
 * get_user_pages_fast may take mmap_sem and page table locks, so no
 * assumptions can be made about lack of locking. get_user_pages_fast is to be
 * implemented in a way that is advantageous (vs get_user_pages()) when the
 * user memory area is already faulted in and present in ptes. However if the
 * pages have to be faulted in, it may turn out to be slightly slower so
 * callers need to carefully consider what to use. On many architectures,
 * get_user_pages_fast simply falls back to get_user_pages.
 */
int __weak get_user_pages_fast(unsigned long start,
				int nr_pages, int write, struct page **pages)
{
	struct mm_struct *mm = current->mm;
	return get_user_pages_unlocked(current, mm, start, nr_pages,
				       pages, write ? FOLL_WRITE : 0);
}
EXPORT_SYMBOL_GPL(get_user_pages_fast);

unsigned long vm_mmap_pgoff(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long pgoff)
{
	unsigned long ret;
	struct mm_struct *mm = current->mm;
	unsigned long populate;

	ret = security_mmap_file(file, prot, flag);
	if (!ret) {
		down_write(&mm->mmap_sem);
		ret = do_mmap_pgoff(file, addr, len, prot, flag, pgoff,
				    &populate);
		up_write(&mm->mmap_sem);
		if (populate)
			mm_populate(ret, populate);
	}
	return ret;
}

unsigned long vm_mmap(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long offset)
{
	if (unlikely(offset + PAGE_ALIGN(len) < offset))
		return -EINVAL;
	if (unlikely(offset_in_page(offset)))
		return -EINVAL;

	return vm_mmap_pgoff(file, addr, len, prot, flag, offset >> PAGE_SHIFT);
}
EXPORT_SYMBOL(vm_mmap);

void kvfree(const void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);
}
EXPORT_SYMBOL(kvfree);

static inline void *__page_rmapping(struct page *page)
{
	unsigned long mapping;

	mapping = (unsigned long)page->mapping;
	mapping &= ~PAGE_MAPPING_FLAGS;

	return (void *)mapping;
}

/* Neutral page->mapping pointer to address_space or anon_vma or other */
void *page_rmapping(struct page *page)
{
	page = compound_head(page);
	return __page_rmapping(page);
}

struct anon_vma *page_anon_vma(struct page *page)
{
	unsigned long mapping;

	page = compound_head(page);
	mapping = (unsigned long)page->mapping;
	if ((mapping & PAGE_MAPPING_FLAGS) != PAGE_MAPPING_ANON)
		return NULL;
	return __page_rmapping(page);
}

struct address_space *page_mapping(struct page *page)
{
	unsigned long mapping;

	/* This happens if someone calls flush_dcache_page on slab page */
	if (unlikely(PageSlab(page)))
		return NULL;

	if (unlikely(PageSwapCache(page))) {
		swp_entry_t entry;

		entry.val = page_private(page);
		return swap_address_space(entry);
	}

	mapping = (unsigned long)page->mapping;
	if ((unsigned long)mapping & PAGE_MAPPING_ANON)
		return NULL;

	return (void *)((unsigned long)mapping & ~PAGE_MAPPING_FLAGS);
}
EXPORT_SYMBOL(page_mapping);

int overcommit_ratio_handler(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp,
			     loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret == 0 && write)
		sysctl_overcommit_kbytes = 0;
	return ret;
}

int overcommit_kbytes_handler(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp,
			     loff_t *ppos)
{
	int ret;

	ret = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
	if (ret == 0 && write)
		sysctl_overcommit_ratio = 0;
	return ret;
}

/*
 * Committed memory limit enforced when OVERCOMMIT_NEVER policy is used
 */
unsigned long vm_commit_limit(void)
{
	unsigned long allowed;

	if (sysctl_overcommit_kbytes)
		allowed = sysctl_overcommit_kbytes >> (PAGE_SHIFT - 10);
	else
		allowed = ((totalram_pages - hugetlb_total_pages())
			   * sysctl_overcommit_ratio / 100);
	allowed += total_swap_pages;

	return allowed;
}

/**
 * get_cmdline() - copy the cmdline value to a buffer.
 * @task:     the task whose cmdline value to copy.
 * @buffer:   the buffer to copy to.
 * @buflen:   the length of the buffer. Larger cmdline values are truncated
 *            to this length.
 * Returns the size of the cmdline field copied. Note that the copy does
 * not guarantee an ending NULL byte.
 */
int get_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	int res = 0;
	unsigned int len;
	struct mm_struct *mm = get_task_mm(task);
	unsigned long arg_start, arg_end, env_start, env_end;
	if (!mm)
		goto out;
	if (!mm->arg_end)
		goto out_mm;	/* Shh! No looking before we're done */

	down_read(&mm->mmap_sem);
	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	env_start = mm->env_start;
	env_end = mm->env_end;
	up_read(&mm->mmap_sem);

	len = arg_end - arg_start;

	if (len > buflen)
		len = buflen;

	res = access_process_vm(task, arg_start, buffer, len, 0);

	/*
	 * If the nul at the end of args has been overwritten, then
	 * assume application is using setproctitle(3).
	 */
	if (res > 0 && buffer[res-1] != '\0' && len < buflen) {
		len = strnlen(buffer, res);
		if (len < res) {
			res = len;
		} else {
			len = env_end - env_start;
			if (len > buflen - res)
				len = buflen - res;
			res += access_process_vm(task, env_start,
						 buffer+res, len, 0);
			res = strnlen(buffer, res);
		}
	}
out_mm:
	mmput(mm);
out:
	return res;
}

#ifdef CONFIG_SYSCTL
/*
 * le9uo (le9 Unofficial working set protection 1.15) sysctl surface.
 *
 * Backport notes for 4.4 -- upstream (6.15) puts these entries in the
 * util_sysctl_table that already lives here; 4.4 mm/util.c has no sysctl
 * table at all (vm knobs live in kernel/sysctl.c's vm_table), so the table
 * is registered separately under the existing "vm" directory.
 *
 * Two upstream helpers do not exist in 4.4 and are substituted:
 *   - SYSCTL_ONE_HUNDRED / SYSCTL_ZERO macros (added to
 *     include/linux/sysctl.h in 4.11) -> local file-static ints below,
 *     same convention kernel/sysctl.c uses with its own zero/one_hundred.
 *   - proc_dou8vec_minmax -> not used here; bounds are enforced through
 *     extra1/extra2 and vm_workingset_protection_update_handler()
 *     in mm/vmscan.c owns the actual write handling.
 *   - proc_dobool -> not present in 4.4 at all (tree-wide grep: 0 hits),
 *     so the bool knob gets a small int-shadow wrapper below.
 */
static int zero;
static int one_hundred = 100;

/*
 * Declared in include/linux/mm.h by the same backport. Repeated here
 * deliberately: identical extern re-declarations are legal C and this keeps
 * mm/util.c compiling on its own if mm.h is patched separately.
 *
 * NOTE: the three ratio knobs are `int`, not `u8`, on purpose. 4.4 has no
 * proc_dou8vec_minmax(); vm_workingset_protection_update_handler() in
 * mm/vmscan.c goes through proc_dointvec_minmax(), which reads and writes a
 * whole int through .data. A u8 variable with .maxlen == sizeof(int) would
 * be overrun by the sysctl core.
 */
extern bool sysctl_workingset_protection;
extern int sysctl_anon_min_ratio;
extern int sysctl_clean_low_ratio;
extern int sysctl_clean_min_ratio;

int vm_workingset_protection_update_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos);

/*
 * mm.h declares vm_workingset_protection_update_handler() with a non-const
 * `struct ctl_table *` to match 4.4's proc_handler typedef, so the
 * assignments below need no cast.
 */

/*
 * 4.4 has no proc_dobool and sysctl_workingset_protection is a one-byte
 * bool while proc_dointvec() works on int-sized data. Read/write through an
 * int shadow, then mirror the value back into the bool.
 */
static int le9uo_workingset_protection_proc(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table shadow = *table;
	int tmp = sysctl_workingset_protection ? 1 : 0;
	int ret;

	shadow.data = &tmp;
	shadow.maxlen = sizeof(tmp);

	ret = proc_dointvec(&shadow, write, buffer, lenp, ppos);
	if (!ret && write)
		sysctl_workingset_protection = tmp ? true : false;

	return ret;
}

static struct ctl_table util_sysctl_table[] = {
	{
		.procname	= "workingset_protection",
		.data		= &sysctl_workingset_protection,
		.maxlen		= sizeof(bool),
		.mode		= 0644,
		.proc_handler	= le9uo_workingset_protection_proc,
	},
	{
		.procname	= "anon_min_ratio",
		.data		= &sysctl_anon_min_ratio,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= vm_workingset_protection_update_handler,
		.extra1		= &zero,
		.extra2		= &one_hundred,
	},
	{
		.procname	= "clean_low_ratio",
		.data		= &sysctl_clean_low_ratio,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= vm_workingset_protection_update_handler,
		.extra1		= &zero,
		.extra2		= &one_hundred,
	},
	{
		.procname	= "clean_min_ratio",
		.data		= &sysctl_clean_min_ratio,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= vm_workingset_protection_update_handler,
		.extra1		= &zero,
		.extra2		= &one_hundred,
	},
	{ }
};

/*
 * 4.4 has NO register_sysctl(path, table): it is declared in
 * include/linux/sysctl.h but never implemented in kernel/sysctl.c, so
 * calling it is a link error. The 4.4 way to place a table under an
 * existing directory is a leading directory entry plus
 * register_sysctl_table() -- the same pattern kernel/sched/core.c and
 * kernel/utsname_sysctl.c use. This attaches the le9uo knobs to the
 * already-existing /proc/sys/vm rather than creating a second "vm".
 */
static struct ctl_table vm_le9uo_root_table[] = {
	{
		.procname = "vm",
		.mode = 0555,
		.child = util_sysctl_table,
	},
	{ }
};

static int __init init_vm_util_sysctls(void)
{
	register_sysctl_table(vm_le9uo_root_table);
	return 0;
}

/* device_initcall: runs after proc_sys_init()/sysctl_init(), so /proc/sys/vm
 * already exists and the entries attach to the existing directory.
 */
device_initcall(init_vm_util_sysctls);
#endif /* CONFIG_SYSCTL */
