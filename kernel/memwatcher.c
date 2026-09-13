// SPDX-License-Identifier: GPL-2.0-only
/*
 * memwatcher.c - safely lend isolated physical pageblocks to a userspace
 * memory tester. This module never tests memory itself.
 */

#include <linux/bitmap.h>
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/hugetlb.h>
#include <linux/list.h>
#include <linux/memory.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pageblock-flags.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "../include/memwatcher_uapi.h"

struct mw_session {
	struct mutex lock;
	unsigned long start_pfn;
	unsigned long nr_pages;
	atomic_t mappings;
	bool claimed;
	bool mapped_once;
};

struct mw_quarantine {
	struct list_head node;
	unsigned long start_pfn;
	unsigned long nr_pages;
	unsigned long flags;
	unsigned long bad[];
};

static LIST_HEAD(mw_quarantines);
static DEFINE_MUTEX(mw_global_lock);
static atomic_t mw_opened = ATOMIC_INIT(0);
static unsigned long mw_active_claims;
static bool mw_hotplug_busy;
static bool mw_module_pinned;

static atomic64_t mw_successful_claims;
static atomic64_t mw_failed_claims;
static atomic64_t mw_tested_pages;
static atomic64_t mw_quarantined_pages;
static atomic64_t mw_orphaned_pages;
static atomic64_t mw_preloaded_pages;

static bool enabled;
module_param(enabled, bool, 0600);
MODULE_PARM_DESC(enabled,
	"Allow destructive pageblock claims (must be set explicitly)");

static bool mw_range_valid(unsigned long start, unsigned long nr)
{
	struct page *first;
	struct zone *zone;
	unsigned long pfn;

	if (!nr || nr != pageblock_nr_pages ||
	    !pageblock_aligned(start) || start + nr < start)
		return false;

	first = pfn_to_online_page(start);
	if (!first)
		return false;
	zone = page_zone(first);

	for (pfn = start; pfn < start + nr; pfn++) {
		struct page *page = pfn_to_online_page(pfn);

		if (!page || page_zone(page) != zone || PageReserved(page) ||
		    PageHWPoison(page) || PageHuge(page))
			return false;
	}

	return true;
}

static int mw_alloc_range(unsigned long start, unsigned long nr)
{
#ifdef ACR_FLAGS_NONE
	return alloc_contig_range(start, start + nr, ACR_FLAGS_NONE,
				  GFP_KERNEL | __GFP_NOWARN);
#else
	return alloc_contig_range(start, start + nr, MIGRATE_MOVABLE,
				  GFP_KERNEL | __GFP_NOWARN);
#endif
}

static void mw_pin_for_quarantine(void)
{
	if (!mw_module_pinned && try_module_get(THIS_MODULE)) {
		mw_module_pinned = true;
		pr_warn("memwatcher: module pinned until reboot because bad/uncertain pages are quarantined\n");
	}
}

/* Caller holds session->lock and has verified there are no VMAs. */
static int mw_finish_claim(struct mw_session *session,
			   const unsigned long *bad_bitmap,
			   unsigned long qflags, bool allow_alloc_failure,
			   bool count_as_test, bool count_as_preload)
{
	struct mw_quarantine *q = NULL;
	unsigned long pfn, bad_count = 0;
	size_t bitmap_bytes = BITS_TO_LONGS(session->nr_pages) * sizeof(long);

	if (bad_bitmap)
		bad_count = bitmap_weight(bad_bitmap, session->nr_pages);

	if (bad_count) {
		q = kzalloc(struct_size(q, bad, BITS_TO_LONGS(session->nr_pages)),
			    GFP_KERNEL);
		if (!q && !allow_alloc_failure)
			return -ENOMEM;
		mw_pin_for_quarantine();
	}

	if (q) {
		q->start_pfn = session->start_pfn;
		q->nr_pages = session->nr_pages;
		q->flags = qflags;
		memcpy(q->bad, bad_bitmap, bitmap_bytes);
	}

	for (pfn = 0; pfn < session->nr_pages; pfn++) {
		if (!bad_bitmap || !test_bit(pfn, bad_bitmap))
			__free_page(pfn_to_page(session->start_pfn + pfn));
	}

	mutex_lock(&mw_global_lock);
	if (q)
		list_add_tail(&q->node, &mw_quarantines);
	else if (bad_count)
		atomic64_add(bad_count, &mw_orphaned_pages);
	atomic64_add(bad_count, &mw_quarantined_pages);
	if (count_as_test)
		atomic64_add(session->nr_pages, &mw_tested_pages);
	if (count_as_preload)
		atomic64_add(bad_count, &mw_preloaded_pages);
	if (mw_active_claims)
		mw_active_claims--;
	mutex_unlock(&mw_global_lock);

	session->claimed = false;
	session->start_pfn = 0;
	session->nr_pages = 0;
	session->mapped_once = false;
	return 0;
}

static int mw_quarantine_all(struct mw_session *session, unsigned long flags,
			     bool allow_alloc_failure, bool count_as_test,
			     bool count_as_preload)
{
	unsigned long *bad;
	int ret;

	bad = bitmap_zalloc(session->nr_pages, GFP_KERNEL);
	if (!bad) {
		if (!allow_alloc_failure)
			return -ENOMEM;
		mw_pin_for_quarantine();
		mutex_lock(&mw_global_lock);
		atomic64_add(session->nr_pages, &mw_quarantined_pages);
		atomic64_add(session->nr_pages, &mw_orphaned_pages);
		if (count_as_test)
			atomic64_add(session->nr_pages, &mw_tested_pages);
		if (count_as_preload)
			atomic64_add(session->nr_pages, &mw_preloaded_pages);
		if (mw_active_claims)
			mw_active_claims--;
		mutex_unlock(&mw_global_lock);
		session->claimed = false;
		session->start_pfn = 0;
		session->nr_pages = 0;
		session->mapped_once = false;
		return 0;
	}

	bitmap_fill(bad, session->nr_pages);
	ret = mw_finish_claim(session, bad, flags | MW_QUARANTINE_F_WHOLE_CLAIM,
			      allow_alloc_failure, count_as_test,
			      count_as_preload);
	bitmap_free(bad);
	return ret;
}

static long mw_get_info(void __user *arg)
{
	struct mw_info info = {
		.abi_version = MW_ABI_VERSION,
		.page_size = PAGE_SIZE,
		.pageblock_pages = pageblock_nr_pages,
		.max_bad_pages = MW_MAX_BAD_PAGES,
		.successful_claims = atomic64_read(&mw_successful_claims),
		.failed_claims = atomic64_read(&mw_failed_claims),
		.tested_pages = atomic64_read(&mw_tested_pages),
		.quarantined_pages = atomic64_read(&mw_quarantined_pages),
		.orphaned_quarantine_pages = atomic64_read(&mw_orphaned_pages),
		.preloaded_pages = atomic64_read(&mw_preloaded_pages),
	};

	return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long mw_claim(struct mw_session *session, void __user *arg)
{
	struct mw_claim request;
	unsigned long start, nr;
	int ret;

	if (!enabled)
		return -EPERM;
	if (copy_from_user(&request, arg, sizeof(request)))
		return -EFAULT;
	if (request.flags || request.start_pfn > ULONG_MAX)
		return -EINVAL;

	start = request.start_pfn;
	nr = request.nr_pages;

	mutex_lock(&session->lock);
	if (session->claimed || atomic_read(&session->mappings)) {
		ret = -EBUSY;
		goto out_session;
	}

	mutex_lock(&mw_global_lock);
	if (mw_hotplug_busy) {
		mutex_unlock(&mw_global_lock);
		ret = -EBUSY;
		goto out_session;
	}
	mw_active_claims++;
	mutex_unlock(&mw_global_lock);

	if (!mw_range_valid(start, nr)) {
		ret = -EINVAL;
		goto out_active;
	}

	ret = mw_alloc_range(start, nr);
	if (ret)
		goto out_active;

	session->start_pfn = start;
	session->nr_pages = nr;
	session->claimed = true;
	session->mapped_once = false;
	atomic64_inc(&mw_successful_claims);
	mutex_unlock(&session->lock);
	return 0;

out_active:
	mutex_lock(&mw_global_lock);
	mw_active_claims--;
	mutex_unlock(&mw_global_lock);
	atomic64_inc(&mw_failed_claims);
out_session:
	mutex_unlock(&session->lock);
	return ret;
}

static long mw_complete(struct mw_session *session, void __user *arg)
{
	struct mw_result result;
	unsigned long *bad = NULL;
	unsigned long quarantine_flags;
	unsigned int i;
	bool count_as_preload, count_as_test;
	int ret = 0;

	if (copy_from_user(&result, arg, sizeof(result)))
		return -EFAULT;
	if (result.flags & ~(MW_RESULT_F_QUARANTINE_ALL |
			     MW_RESULT_F_PRELOAD | MW_RESULT_F_INCOMPLETE))
		return -EINVAL;

	mutex_lock(&session->lock);
	if (!session->claimed) {
		ret = -EINVAL;
		goto out;
	}
	if (atomic_read(&session->mappings)) {
		ret = -EBUSY;
		goto out;
	}
	count_as_preload = result.flags & MW_RESULT_F_PRELOAD;
	count_as_test = !(result.flags & (MW_RESULT_F_PRELOAD |
					 MW_RESULT_F_INCOMPLETE));
	quarantine_flags = count_as_preload ? MW_QUARANTINE_F_PRELOADED : 0;
	if (result.flags & MW_RESULT_F_INCOMPLETE)
		quarantine_flags |= MW_QUARANTINE_F_INCOMPLETE;
	if ((count_as_preload && session->mapped_once) ||
	    ((result.flags & MW_RESULT_F_INCOMPLETE) &&
	     (!(result.flags & MW_RESULT_F_QUARANTINE_ALL) || count_as_preload))) {
		ret = -EINVAL;
		goto out;
	}

	if (result.flags & MW_RESULT_F_QUARANTINE_ALL) {
		ret = mw_quarantine_all(session, quarantine_flags, false, count_as_test,
					count_as_preload);
		goto out;
	}
	if (result.bad_count > MW_MAX_BAD_PAGES) {
		ret = -EINVAL;
		goto out;
	}
	if (!result.bad_count) {
		if (count_as_preload) {
			ret = -EINVAL;
			goto out;
		}
		ret = mw_finish_claim(session, NULL, 0, false, count_as_test,
				      false);
		goto out;
	}

	bad = bitmap_zalloc(session->nr_pages, GFP_KERNEL);
	if (!bad) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < result.bad_count; i++) {
		unsigned int offset = result.bad_page_offsets[i];

		if (offset >= session->nr_pages || test_and_set_bit(offset, bad)) {
			ret = -EINVAL;
			goto out;
		}
	}
	ret = mw_finish_claim(session, bad, quarantine_flags, false, count_as_test,
			      count_as_preload);
out:
	bitmap_free(bad);
	mutex_unlock(&session->lock);
	return ret;
}

static long mw_get_quarantine(void __user *arg)
{
	struct mw_quarantine_query query;
	struct mw_quarantine *q;
	unsigned long index = 0, bit;
	bool found = false;

	if (copy_from_user(&query, arg, sizeof(query)))
		return -EFAULT;

	mutex_lock(&mw_global_lock);
	list_for_each_entry(q, &mw_quarantines, node) {
		for_each_set_bit(bit, q->bad, q->nr_pages) {
			if (index++ != query.index)
				continue;
			query.pfn = q->start_pfn + bit;
			query.flags = q->flags;
			query.reserved = 0;
			found = true;
			goto done;
		}
	}
done:
	mutex_unlock(&mw_global_lock);
	if (!found)
		return -ENOENT;
	return copy_to_user(arg, &query, sizeof(query)) ? -EFAULT : 0;
}

static long mw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mw_session *session = file->private_data;
	void __user *user_arg = (void __user *)arg;
	long ret;

	if (_IOC_TYPE(cmd) != MW_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case MW_IOC_GET_INFO:
		return mw_get_info(user_arg);
	case MW_IOC_CLAIM:
		return mw_claim(session, user_arg);
	case MW_IOC_COMPLETE:
		return mw_complete(session, user_arg);
	case MW_IOC_QUARANTINE:
		mutex_lock(&session->lock);
		if (!session->claimed)
			ret = -EINVAL;
		else if (atomic_read(&session->mappings))
			ret = -EBUSY;
		else
			ret = mw_quarantine_all(session, MW_QUARANTINE_F_ABANDONED,
						true, false, false);
		mutex_unlock(&session->lock);
		return ret;
	case MW_IOC_GET_QUARANTINE:
		return mw_get_quarantine(user_arg);
	default:
		return -ENOTTY;
	}
}

static void mw_vma_open(struct vm_area_struct *vma)
{
	struct mw_session *session = vma->vm_private_data;

	atomic_inc(&session->mappings);
}

static void mw_vma_close(struct vm_area_struct *vma)
{
	struct mw_session *session = vma->vm_private_data;

	atomic_dec(&session->mappings);
}

static const struct vm_operations_struct mw_vm_ops = {
	.open = mw_vma_open,
	.close = mw_vma_close,
};

static int mw_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mw_session *session = file->private_data;
	unsigned long length = vma->vm_end - vma->vm_start;
	int ret = 0;

	mutex_lock(&session->lock);
	if (!session->claimed || atomic_read(&session->mappings)) {
		ret = -EBUSY;
		goto out;
	}
	if (vma->vm_pgoff || length != session->nr_pages * PAGE_SIZE ||
	    !(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_WRITE) ||
	    (vma->vm_flags & VM_EXEC)) {
		ret = -EINVAL;
		goto out;
	}

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP |
			  VM_DONTCOPY);
	vma->vm_ops = &mw_vm_ops;
	vma->vm_private_data = session;
	ret = remap_pfn_range(vma, vma->vm_start, session->start_pfn,
			      length, vma->vm_page_prot);
	if (!ret)
		session->mapped_once = true;
	if (!ret)
		mw_vma_open(vma);
out:
	mutex_unlock(&session->lock);
	return ret;
}

static int mw_open(struct inode *inode, struct file *file)
{
	struct mw_session *session;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (atomic_cmpxchg(&mw_opened, 0, 1))
		return -EBUSY;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		atomic_set(&mw_opened, 0);
		return -ENOMEM;
	}
	mutex_init(&session->lock);
	atomic_set(&session->mappings, 0);
	file->private_data = session;
	return 0;
}

static int mw_release(struct inode *inode, struct file *file)
{
	struct mw_session *session = file->private_data;

	mutex_lock(&session->lock);
	if (session->claimed)
		mw_quarantine_all(session, MW_QUARANTINE_F_ABANDONED, true,
				  false, false);
	mutex_unlock(&session->lock);
	kfree(session);
	atomic_set(&mw_opened, 0);
	return 0;
}

static const struct file_operations mw_fops = {
	.owner = THIS_MODULE,
	.open = mw_open,
	.release = mw_release,
	.unlocked_ioctl = mw_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mw_ioctl,
#endif
	.mmap = mw_mmap,
	.llseek = no_llseek,
};

static struct miscdevice mw_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "memwatcher",
	.fops = &mw_fops,
	.mode = 0600,
};

static int mw_memory_notify(struct notifier_block *nb,
			    unsigned long action, void *data)
{
	int ret = NOTIFY_OK;

	mutex_lock(&mw_global_lock);
	switch (action) {
	case MEM_GOING_OFFLINE:
	case MEM_GOING_ONLINE:
		if (mw_active_claims)
			ret = notifier_from_errno(-EBUSY);
		else
			mw_hotplug_busy = true;
		break;
	case MEM_OFFLINE:
	case MEM_ONLINE:
	case MEM_CANCEL_OFFLINE:
	case MEM_CANCEL_ONLINE:
		mw_hotplug_busy = false;
		break;
	default:
		break;
	}
	mutex_unlock(&mw_global_lock);
	return ret;
}

static struct notifier_block mw_memory_nb = {
	.notifier_call = mw_memory_notify,
};

static int mw_pm_notify(struct notifier_block *nb,
			unsigned long action, void *data)
{
	int ret = NOTIFY_OK;

	if (action != PM_SUSPEND_PREPARE && action != PM_HIBERNATION_PREPARE &&
	    action != PM_RESTORE_PREPARE)
		return NOTIFY_OK;

	mutex_lock(&mw_global_lock);
	if (mw_active_claims || atomic64_read(&mw_quarantined_pages))
		ret = notifier_from_errno(-EBUSY);
	mutex_unlock(&mw_global_lock);
	return ret;
}

static struct notifier_block mw_pm_nb = {
	.notifier_call = mw_pm_notify,
};

static int __init mw_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct mw_info) != 64);
	BUILD_BUG_ON(sizeof(struct mw_claim) != 16);
	BUILD_BUG_ON(sizeof(struct mw_result) != 296);
	BUILD_BUG_ON(sizeof(struct mw_quarantine_query) != 24);

	if (!IS_ENABLED(CONFIG_CONTIG_ALLOC) || !IS_ENABLED(CONFIG_MEMORY_ISOLATION))
		return -EOPNOTSUPP;

	ret = register_memory_notifier(&mw_memory_nb);
	if (ret)
		return ret;
	ret = register_pm_notifier(&mw_pm_nb);
	if (ret)
		goto unregister_memory;
	ret = misc_register(&mw_miscdev);
	if (ret)
		goto unregister_pm;

	pr_info("memwatcher 1.1.0-dev loaded (enabled=%d, pageblock=%lu pages)\n",
		enabled, pageblock_nr_pages);
	return 0;

unregister_pm:
	unregister_pm_notifier(&mw_pm_nb);
unregister_memory:
	unregister_memory_notifier(&mw_memory_nb);
	return ret;
}

static void __exit mw_exit(void)
{
	misc_deregister(&mw_miscdev);
	unregister_pm_notifier(&mw_pm_nb);
	unregister_memory_notifier(&mw_memory_nb);
	pr_info("memwatcher unloaded\n");
}

module_init(mw_init);
module_exit(mw_exit);

MODULE_AUTHOR("Memwatcher contributors");
MODULE_DESCRIPTION("Continuous online physical memory test page isolator");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1.0-dev");
