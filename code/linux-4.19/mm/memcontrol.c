/* memcontrol.c - Memory Controller
 *
 * Copyright IBM Corporation, 2007
 * Author Balbir Singh <balbir@linux.vnet.ibm.com>
 *
 * Copyright 2007 OpenVZ SWsoft Inc
 * Author: Pavel Emelianov <xemul@openvz.org>
 *
 * Memory thresholds
 * Copyright (C) 2009 Nokia Corporation
 * Author: Kirill A. Shutemov
 *
 * Kernel Memory Controller
 * Copyright (C) 2012 Parallels Inc. and Google Inc.
 * Authors: Glauber Costa and Suleiman Souhlal
 *
 * Native page reclaim
 * Charge lifetime sanitation
 * Lockless page tracking & accounting
 * Unified hierarchy configuration model
 * Copyright (C) 2015 Red Hat, Inc., Johannes Weiner
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/page_counter.h>
#include <linux/memcontrol.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/smp.h>
#include <linux/page-flags.h>
#include <linux/backing-dev.h>
#include <linux/bit_spinlock.h>
#include <linux/rcupdate.h>
#include <linux/limits.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/spinlock.h>
#include <linux/eventfd.h>
#include <linux/poll.h>
#include <linux/sort.h>
#include <linux/fs.h>
#include <linux/psi.h>
#include <linux/seq_file.h>
#include <linux/vmpressure.h>
#include <linux/mm_inline.h>
#include <linux/swap_cgroup.h>
#include <linux/cpu.h>
#include <linux/oom.h>
#include <linux/lockdep.h>
#include <linux/file.h>
#include <linux/tracehook.h>
#include "internal.h"
#include <net/sock.h>
#include <net/ip.h>
#include "slab.h"
#include <linux/proc_fs.h>
#include <linux/parser.h>

#include <linux/uaccess.h>

#include <trace/events/vmscan.h>

struct cgroup_subsys memory_cgrp_subsys __read_mostly;
EXPORT_SYMBOL(memory_cgrp_subsys);

struct mem_cgroup *root_mem_cgroup __read_mostly;

/* Socket memory accounting disabled? */
static bool cgroup_memory_nosocket;

/* Kernel memory accounting disabled? */
static bool cgroup_memory_nokmem;
unsigned long sysctl_penalty_extra_delay_jiffies;

#ifdef CONFIG_MEMSLI
/* Cgroup memory SLI disabled? */
static DEFINE_STATIC_KEY_FALSE(cgroup_memory_nosli);
#endif /* CONFIG_MEMSLI */

/* Whether the swap controller is active */
#ifdef CONFIG_MEMCG_SWAP
bool cgroup_memory_noswap __read_mostly;
#else
#define cgroup_memory_noswap		1
#endif

static struct workqueue_struct *memcg_wmark_wq;

/* Whether legacy memory+swap accounting is active */
static bool do_memsw_account(void)
{
	return !cgroup_subsys_on_dfl(memory_cgrp_subsys) && !cgroup_memory_noswap;
}

static const char *const mem_cgroup_lru_names[] = {
	"inactive_anon",
	"active_anon",
	"inactive_file",
	"active_file",
	"unevictable",
};

#define THRESHOLDS_EVENTS_TARGET 128
#define SOFTLIMIT_EVENTS_TARGET 1024
#define NUMAINFO_EVENTS_TARGET	1024

/*
 * Cgroups above their limits are maintained in a RB-Tree, independent of
 * their hierarchy representation
 */

struct mem_cgroup_tree_per_node {
	struct rb_root rb_root;
	struct rb_node *rb_rightmost;
	spinlock_t lock;
};

struct mem_cgroup_tree {
	struct mem_cgroup_tree_per_node *rb_tree_per_node[MAX_NUMNODES];
};

static struct mem_cgroup_tree soft_limit_tree __read_mostly;

/* for OOM */
struct mem_cgroup_eventfd_list {
	struct list_head list;
	struct eventfd_ctx *eventfd;
};

/*
 * cgroup_event represents events which userspace want to receive.
 */
struct mem_cgroup_event {
	/*
	 * memcg which the event belongs to.
	 */
	struct mem_cgroup *memcg;
	/*
	 * eventfd to signal userspace about the event.
	 */
	struct eventfd_ctx *eventfd;
	/*
	 * Each of these stored in a list by the cgroup.
	 */
	struct list_head list;
	/*
	 * register_event() callback will be used to add new userspace
	 * waiter for changes related to this event.  Use eventfd_signal()
	 * on eventfd to send notification to userspace.
	 */
	int (*register_event)(struct mem_cgroup *memcg,
			      struct eventfd_ctx *eventfd, const char *args);
	/*
	 * unregister_event() callback will be called when userspace closes
	 * the eventfd or on cgroup removing.  This callback must be set,
	 * if you want provide notification functionality.
	 */
	void (*unregister_event)(struct mem_cgroup *memcg,
				 struct eventfd_ctx *eventfd);
	/*
	 * All fields below needed to unregister event when
	 * userspace closes eventfd.
	 */
	poll_table pt;
	wait_queue_head_t *wqh;
	wait_queue_entry_t wait;
	struct work_struct remove;
};

static void mem_cgroup_threshold(struct mem_cgroup *memcg);
static void mem_cgroup_oom_notify(struct mem_cgroup *memcg);

/* Stuffs for move charges at task migration. */
/*
 * Types of charges to be moved.
 */
#define MOVE_ANON	0x1U
#define MOVE_FILE	0x2U
#define MOVE_MASK	(MOVE_ANON | MOVE_FILE)

/* "mc" and its members are protected by cgroup_mutex */
static struct move_charge_struct {
	spinlock_t	  lock; /* for from, to */
	struct mm_struct  *mm;
	struct mem_cgroup *from;
	struct mem_cgroup *to;
	unsigned long flags;
	unsigned long precharge;
	unsigned long moved_charge;
	unsigned long moved_swap;
	struct task_struct *moving_task;	/* a task moving charges */
	wait_queue_head_t waitq;		/* a waitq for other context */
} mc = {
	.lock = __SPIN_LOCK_UNLOCKED(mc.lock),
	.waitq = __WAIT_QUEUE_HEAD_INITIALIZER(mc.waitq),
};

/*
 * Maximum loops in mem_cgroup_hierarchical_reclaim(), used for soft
 * limit reclaim to prevent infinite loops, if they ever occur.
 */
#define	MEM_CGROUP_MAX_RECLAIM_LOOPS		100
#define	MEM_CGROUP_MAX_SOFT_LIMIT_RECLAIM_LOOPS	2

enum charge_type {
	MEM_CGROUP_CHARGE_TYPE_CACHE = 0,
	MEM_CGROUP_CHARGE_TYPE_ANON,
	MEM_CGROUP_CHARGE_TYPE_SWAPOUT,	/* for accounting swapcache */
	MEM_CGROUP_CHARGE_TYPE_DROP,	/* a page was unused swap cache */
	NR_CHARGE_TYPE,
};

/* for encoding cft->private value on file */
enum res_type {
	_MEM,
	_MEMSWAP,
	_OOM_TYPE,
	_KMEM,
	_TCP,
};

#define MEMFILE_PRIVATE(x, val)	((x) << 16 | (val))
#define MEMFILE_TYPE(val)	((val) >> 16 & 0xffff)
#define MEMFILE_ATTR(val)	((val) & 0xffff)
/* Used for OOM nofiier */
#define OOM_CONTROL		(0)

/*
 * Iteration constructs for visiting all cgroups (under a tree).  If
 * loops are exited prematurely (break), mem_cgroup_iter_break() must
 * be used for reference counting.
 */
#define for_each_mem_cgroup_tree(iter, root)		\
	for (iter = mem_cgroup_iter(root, NULL, NULL);	\
	     iter != NULL;				\
	     iter = mem_cgroup_iter(root, iter, NULL))

#define for_each_mem_cgroup(iter)			\
	for (iter = mem_cgroup_iter(NULL, NULL, NULL);	\
	     iter != NULL;				\
	     iter = mem_cgroup_iter(NULL, iter, NULL))

static inline bool should_force_charge(void)
{
	return tsk_is_oom_victim(current) || fatal_signal_pending(current) ||
		(current->flags & PF_EXITING);
}

/* Some nice accessors for the vmpressure. */
struct vmpressure *memcg_to_vmpressure(struct mem_cgroup *memcg)
{
	if (!memcg)
		memcg = root_mem_cgroup;
	return &memcg->vmpressure;
}

struct cgroup_subsys_state *vmpressure_to_css(struct vmpressure *vmpr)
{
	return &container_of(vmpr, struct mem_cgroup, vmpressure)->css;
}

#ifdef CONFIG_MEMCG_KMEM
/*
 * This will be the memcg's index in each cache's ->memcg_params.memcg_caches.
 * The main reason for not using cgroup id for this:
 *  this works better in sparse environments, where we have a lot of memcgs,
 *  but only a few kmem-limited. Or also, if we have, for instance, 200
 *  memcgs, and none but the 200th is kmem-limited, we'd have to have a
 *  200 entry array for that.
 *
 * The current size of the caches array is stored in memcg_nr_cache_ids. It
 * will double each time we have to increase it.
 */
static DEFINE_IDA(memcg_cache_ida);
int memcg_nr_cache_ids;

/* Protects memcg_nr_cache_ids */
static DECLARE_RWSEM(memcg_cache_ids_sem);
static int swap_high_show(struct seq_file *m, void *v);
static ssize_t swap_high_write(struct kernfs_open_file *of,
					char *buf, size_t nbytes, loff_t off);
static int swap_events_show(struct seq_file *m, void *v);

void memcg_get_cache_ids(void)
{
	down_read(&memcg_cache_ids_sem);
}

void memcg_put_cache_ids(void)
{
	up_read(&memcg_cache_ids_sem);
}

/*
 * MIN_SIZE is different than 1, because we would like to avoid going through
 * the alloc/free process all the time. In a small machine, 4 kmem-limited
 * cgroups is a reasonable guess. In the future, it could be a parameter or
 * tunable, but that is strictly not necessary.
 *
 * MAX_SIZE should be as large as the number of cgrp_ids. Ideally, we could get
 * this constant directly from cgroup, but it is understandable that this is
 * better kept as an internal representation in cgroup.c. In any case, the
 * cgrp_id space is not getting any smaller, and we don't have to necessarily
 * increase ours as well if it increases.
 */
#define MEMCG_CACHES_MIN_SIZE 4
#define MEMCG_CACHES_MAX_SIZE MEM_CGROUP_ID_MAX

/*
 * A lot of the calls to the cache allocation functions are expected to be
 * inlined by the compiler. Since the calls to memcg_kmem_get_cache are
 * conditional to this static branch, we'll have to allow modules that does
 * kmem_cache_alloc and the such to see this symbol as well
 */
DEFINE_STATIC_KEY_FALSE(memcg_kmem_enabled_key);
EXPORT_SYMBOL(memcg_kmem_enabled_key);

struct workqueue_struct *memcg_kmem_cache_wq;
#endif

static int memcg_shrinker_map_size;
static DEFINE_MUTEX(memcg_shrinker_map_mutex);

static void memcg_free_shrinker_map_rcu(struct rcu_head *head)
{
	kvfree(container_of(head, struct memcg_shrinker_map, rcu));
}

static int memcg_expand_one_shrinker_map(struct mem_cgroup *memcg,
					 int size, int old_size)
{
	struct memcg_shrinker_map *new, *old;
	int nid;

	lockdep_assert_held(&memcg_shrinker_map_mutex);

	for_each_node(nid) {
		old = rcu_dereference_protected(
			mem_cgroup_nodeinfo(memcg, nid)->shrinker_map, true);
		/* Not yet online memcg */
		if (!old)
			return 0;

		new = kvmalloc(sizeof(*new) + size, GFP_KERNEL);
		if (!new)
			return -ENOMEM;

		/* Set all old bits, clear all new bits */
		memset(new->map, (int)0xff, old_size);
		memset((void *)new->map + old_size, 0, size - old_size);

		rcu_assign_pointer(memcg->nodeinfo[nid]->shrinker_map, new);
		call_rcu(&old->rcu, memcg_free_shrinker_map_rcu);
	}

	return 0;
}

static void memcg_free_shrinker_maps(struct mem_cgroup *memcg)
{
	struct mem_cgroup_per_node *pn;
	struct memcg_shrinker_map *map;
	int nid;

	if (mem_cgroup_is_root(memcg))
		return;

	for_each_node(nid) {
		pn = mem_cgroup_nodeinfo(memcg, nid);
		map = rcu_dereference_protected(pn->shrinker_map, true);
		if (map)
			kvfree(map);
		rcu_assign_pointer(pn->shrinker_map, NULL);
	}
}

static int memcg_alloc_shrinker_maps(struct mem_cgroup *memcg)
{
	struct memcg_shrinker_map *map;
	int nid, size, ret = 0;

	if (mem_cgroup_is_root(memcg))
		return 0;

	mutex_lock(&memcg_shrinker_map_mutex);
	size = memcg_shrinker_map_size;
	for_each_node(nid) {
		map = kvzalloc(sizeof(*map) + size, GFP_KERNEL);
		if (!map) {
			memcg_free_shrinker_maps(memcg);
			ret = -ENOMEM;
			break;
		}
		rcu_assign_pointer(memcg->nodeinfo[nid]->shrinker_map, map);
	}
	mutex_unlock(&memcg_shrinker_map_mutex);

	return ret;
}

int memcg_expand_shrinker_maps(int new_id)
{
	int size, old_size, ret = 0;
	struct mem_cgroup *memcg;

	size = DIV_ROUND_UP(new_id + 1, BITS_PER_LONG) * sizeof(unsigned long);
	old_size = memcg_shrinker_map_size;
	if (size <= old_size)
		return 0;

	mutex_lock(&memcg_shrinker_map_mutex);
	if (!root_mem_cgroup)
		goto unlock;

	for_each_mem_cgroup(memcg) {
		if (mem_cgroup_is_root(memcg))
			continue;
		ret = memcg_expand_one_shrinker_map(memcg, size, old_size);
		if (ret)
			goto unlock;
	}
unlock:
	if (!ret)
		memcg_shrinker_map_size = size;
	mutex_unlock(&memcg_shrinker_map_mutex);
	return ret;
}

void memcg_set_shrinker_bit(struct mem_cgroup *memcg, int nid, int shrinker_id)
{
	if (shrinker_id >= 0 && memcg && !mem_cgroup_is_root(memcg)) {
		struct memcg_shrinker_map *map;

		rcu_read_lock();
		map = rcu_dereference(memcg->nodeinfo[nid]->shrinker_map);
		/* Pairs with smp mb in shrink_slab() */
		smp_mb__before_atomic();
		set_bit(shrinker_id, map->map);
		rcu_read_unlock();
	}
}

/**
 * mem_cgroup_css_from_page - css of the memcg associated with a page
 * @page: page of interest
 *
 * If memcg is bound to the default hierarchy, css of the memcg associated
 * with @page is returned.  The returned css remains associated with @page
 * until it is released.
 *
 * If memcg is bound to a traditional hierarchy, the css of root_mem_cgroup
 * is returned.
 */
struct cgroup_subsys_state *mem_cgroup_css_from_page(struct page *page)
{
	struct mem_cgroup *memcg;

	memcg = page->mem_cgroup;

#ifdef CONFIG_CGROUP_WRITEBACK
	if (!memcg ||
	    (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && !cgwb_v1))
#else
	if (!memcg || !cgroup_subsys_on_dfl(memory_cgrp_subsys))
#endif
		memcg = root_mem_cgroup;

	return &memcg->css;
}

/**
 * page_cgroup_ino - return inode number of the memcg a page is charged to
 * @page: the page
 *
 * Look up the closest online ancestor of the memory cgroup @page is charged to
 * and return its inode number or 0 if @page is not charged to any cgroup. It
 * is safe to call this function without holding a reference to @page.
 *
 * Note, this function is inherently racy, because there is nothing to prevent
 * the cgroup inode from getting torn down and potentially reallocated a moment
 * after page_cgroup_ino() returns, so it only should be used by callers that
 * do not care (such as procfs interfaces).
 */
ino_t page_cgroup_ino(struct page *page)
{
	struct mem_cgroup *memcg;
	unsigned long ino = 0;

	rcu_read_lock();
	memcg = READ_ONCE(page->mem_cgroup);
	while (memcg && !(memcg->css.flags & CSS_ONLINE))
		memcg = parent_mem_cgroup(memcg);
	if (memcg)
		ino = cgroup_ino(memcg->css.cgroup);
	rcu_read_unlock();
	return ino;
}

static struct mem_cgroup_per_node *
mem_cgroup_page_nodeinfo(struct mem_cgroup *memcg, struct page *page)
{
	int nid = page_to_nid(page);

	return memcg->nodeinfo[nid];
}

static struct mem_cgroup_tree_per_node *
soft_limit_tree_node(int nid)
{
	return soft_limit_tree.rb_tree_per_node[nid];
}

static struct mem_cgroup_tree_per_node *
soft_limit_tree_from_page(struct page *page)
{
	int nid = page_to_nid(page);

	return soft_limit_tree.rb_tree_per_node[nid];
}

static void __mem_cgroup_insert_exceeded(struct mem_cgroup_per_node *mz,
					 struct mem_cgroup_tree_per_node *mctz,
					 unsigned long new_usage_in_excess)
{
	struct rb_node **p = &mctz->rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct mem_cgroup_per_node *mz_node;
	bool rightmost = true;

	if (mz->on_tree)
		return;

	mz->usage_in_excess = new_usage_in_excess;
	if (!mz->usage_in_excess)
		return;
	while (*p) {
		parent = *p;
		mz_node = rb_entry(parent, struct mem_cgroup_per_node,
					tree_node);
		if (mz->usage_in_excess < mz_node->usage_in_excess) {
			p = &(*p)->rb_left;
			rightmost = false;
		}

		/*
		 * We can't avoid mem cgroups that are over their soft
		 * limit by the same amount
		 */
		else if (mz->usage_in_excess >= mz_node->usage_in_excess)
			p = &(*p)->rb_right;
	}

	if (rightmost)
		mctz->rb_rightmost = &mz->tree_node;

	rb_link_node(&mz->tree_node, parent, p);
	rb_insert_color(&mz->tree_node, &mctz->rb_root);
	mz->on_tree = true;
}

static void __mem_cgroup_remove_exceeded(struct mem_cgroup_per_node *mz,
					 struct mem_cgroup_tree_per_node *mctz)
{
	if (!mz->on_tree)
		return;

	if (&mz->tree_node == mctz->rb_rightmost)
		mctz->rb_rightmost = rb_prev(&mz->tree_node);

	rb_erase(&mz->tree_node, &mctz->rb_root);
	mz->on_tree = false;
}

static void mem_cgroup_remove_exceeded(struct mem_cgroup_per_node *mz,
				       struct mem_cgroup_tree_per_node *mctz)
{
	unsigned long flags;

	spin_lock_irqsave(&mctz->lock, flags);
	__mem_cgroup_remove_exceeded(mz, mctz);
	spin_unlock_irqrestore(&mctz->lock, flags);
}

static unsigned long soft_limit_excess(struct mem_cgroup *memcg)
{
	unsigned long nr_pages = page_counter_read(&memcg->memory);
	unsigned long soft_limit = READ_ONCE(memcg->soft_limit);
	unsigned long excess = 0;

	if (nr_pages > soft_limit)
		excess = nr_pages - soft_limit;

	return excess;
}

static void mem_cgroup_update_tree(struct mem_cgroup *memcg, struct page *page)
{
	unsigned long excess;
	struct mem_cgroup_per_node *mz;
	struct mem_cgroup_tree_per_node *mctz;

	mctz = soft_limit_tree_from_page(page);
	if (!mctz)
		return;
	/*
	 * Necessary to update all ancestors when hierarchy is used.
	 * because their event counter is not touched.
	 */
	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		mz = mem_cgroup_page_nodeinfo(memcg, page);
		excess = soft_limit_excess(memcg);
		/*
		 * We have to update the tree if mz is on RB-tree or
		 * mem is over its softlimit.
		 */
		if (excess || mz->on_tree) {
			unsigned long flags;

			spin_lock_irqsave(&mctz->lock, flags);
			/* if on-tree, remove it */
			if (mz->on_tree)
				__mem_cgroup_remove_exceeded(mz, mctz);
			/*
			 * Insert again. mz->usage_in_excess will be updated.
			 * If excess is 0, no tree ops.
			 */
			__mem_cgroup_insert_exceeded(mz, mctz, excess);
			spin_unlock_irqrestore(&mctz->lock, flags);
		}
	}
}

static void mem_cgroup_remove_from_trees(struct mem_cgroup *memcg)
{
	struct mem_cgroup_tree_per_node *mctz;
	struct mem_cgroup_per_node *mz;
	int nid;

	for_each_node(nid) {
		mz = mem_cgroup_nodeinfo(memcg, nid);
		mctz = soft_limit_tree_node(nid);
		if (mctz)
			mem_cgroup_remove_exceeded(mz, mctz);
	}
}

static struct mem_cgroup_per_node *
__mem_cgroup_largest_soft_limit_node(struct mem_cgroup_tree_per_node *mctz)
{
	struct mem_cgroup_per_node *mz;

retry:
	mz = NULL;
	if (!mctz->rb_rightmost)
		goto done;		/* Nothing to reclaim from */

	mz = rb_entry(mctz->rb_rightmost,
		      struct mem_cgroup_per_node, tree_node);
	/*
	 * Remove the node now but someone else can add it back,
	 * we will to add it back at the end of reclaim to its correct
	 * position in the tree.
	 */
	__mem_cgroup_remove_exceeded(mz, mctz);
	if (!soft_limit_excess(mz->memcg) ||
	    !css_tryget_online(&mz->memcg->css))
		goto retry;
done:
	return mz;
}

static struct mem_cgroup_per_node *
mem_cgroup_largest_soft_limit_node(struct mem_cgroup_tree_per_node *mctz)
{
	struct mem_cgroup_per_node *mz;

	spin_lock_irq(&mctz->lock);
	mz = __mem_cgroup_largest_soft_limit_node(mctz);
	spin_unlock_irq(&mctz->lock);
	return mz;
}

/**
 * __mod_memcg_state - update cgroup memory statistics
 * @memcg: the memory cgroup
 * @idx: the stat item - can be enum memcg_stat_item or enum node_stat_item
 * @val: delta to add to the counter, can be negative
 */
void __mod_memcg_state(struct mem_cgroup *memcg, int idx, int val)
{
	long x;

	if (mem_cgroup_disabled())
		return;

	x = val + __this_cpu_read(memcg->vmstats_percpu->stat[idx]);
	if (unlikely(abs(x) > MEMCG_CHARGE_BATCH)) {
		struct mem_cgroup *mi;

		/*
		 * Batch local counters to keep them in sync with
		 * the hierarchical ones.
		 */
		__this_cpu_add(memcg->vmstats_local->stat[idx], x);
		for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
			atomic_long_add(x, &mi->vmstats[idx]);
		x = 0;
	}
	__this_cpu_write(memcg->vmstats_percpu->stat[idx], x);
}

static struct mem_cgroup_per_node *
parent_nodeinfo(struct mem_cgroup_per_node *pn, int nid)
{
	struct mem_cgroup *parent;

	parent = parent_mem_cgroup(pn->memcg);
	if (!parent)
		return NULL;
	return mem_cgroup_nodeinfo(parent, nid);
}

/**
 * __mod_lruvec_state - update lruvec memory statistics
 * @lruvec: the lruvec
 * @idx: the stat item
 * @val: delta to add to the counter, can be negative
 *
 * The lruvec is the intersection of the NUMA node and a cgroup. This
 * function updates the all three counters that are affected by a
 * change of state at this level: per-node, per-cgroup, per-lruvec.
 */
void __mod_lruvec_state(struct lruvec *lruvec, enum node_stat_item idx,
			int val)
{
	pg_data_t *pgdat = lruvec_pgdat(lruvec);
	struct mem_cgroup_per_node *pn;
	struct mem_cgroup *memcg;
	long x;

	/* Update node */
	__mod_node_page_state(pgdat, idx, val);

	if (mem_cgroup_disabled())
		return;

	pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	memcg = pn->memcg;

	/* Update memcg */
	__mod_memcg_state(memcg, idx, val);

	/* Update lruvec */
	__this_cpu_add(pn->lruvec_stat_local->count[idx], val);

	x = val + __this_cpu_read(pn->lruvec_stat_cpu->count[idx]);
	if (unlikely(abs(x) > MEMCG_CHARGE_BATCH)) {
		struct mem_cgroup_per_node *pi;

		for (pi = pn; pi; pi = parent_nodeinfo(pi, pgdat->node_id))
			atomic_long_add(x, &pi->lruvec_stat[idx]);
		x = 0;
	}
	__this_cpu_write(pn->lruvec_stat_cpu->count[idx], x);
}

/**
 * __count_memcg_events - account VM events in a cgroup
 * @memcg: the memory cgroup
 * @idx: the event item
 * @count: the number of events that occured
 */
void __count_memcg_events(struct mem_cgroup *memcg, enum vm_event_item idx,
			  unsigned long count)
{
	unsigned long x;

	if (mem_cgroup_disabled())
		return;

	x = count + __this_cpu_read(memcg->vmstats_percpu->events[idx]);
	if (unlikely(x > MEMCG_CHARGE_BATCH)) {
		struct mem_cgroup *mi;

		/*
		 * Batch local counters to keep them in sync with
		 * the hierarchical ones.
		 */
		__this_cpu_add(memcg->vmstats_local->events[idx], x);
		for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
			atomic_long_add(x, &mi->vmevents[idx]);
		x = 0;
	}
	__this_cpu_write(memcg->vmstats_percpu->events[idx], x);
}

static unsigned long memcg_events(struct mem_cgroup *memcg, int event)
{
	return atomic_long_read(&memcg->vmevents[event]);
}

static unsigned long memcg_events_local(struct mem_cgroup *memcg, int event)
{
	long x = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		x += per_cpu(memcg->vmstats_local->events[event], cpu);
	return x;
}

static void mem_cgroup_charge_statistics(struct mem_cgroup *memcg,
					 struct page *page,
					 int nr_pages)
{
	/* pagein of a big page is an event. So, ignore page size */
	if (nr_pages > 0)
		__count_memcg_events(memcg, PGPGIN, 1);
	else {
		__count_memcg_events(memcg, PGPGOUT, 1);
		nr_pages = -nr_pages; /* for event */
	}

	__this_cpu_add(memcg->vmstats_percpu->nr_page_events, nr_pages);
}

static bool mem_cgroup_event_ratelimit(struct mem_cgroup *memcg,
				       enum mem_cgroup_events_target target)
{
	unsigned long val, next;

	val = __this_cpu_read(memcg->vmstats_percpu->nr_page_events);
	next = __this_cpu_read(memcg->vmstats_percpu->targets[target]);
	/* from time_after() in jiffies.h */
	if ((long)(next - val) < 0) {
		switch (target) {
		case MEM_CGROUP_TARGET_THRESH:
			next = val + THRESHOLDS_EVENTS_TARGET;
			break;
		case MEM_CGROUP_TARGET_SOFTLIMIT:
			next = val + SOFTLIMIT_EVENTS_TARGET;
			break;
		case MEM_CGROUP_TARGET_NUMAINFO:
			next = val + NUMAINFO_EVENTS_TARGET;
			break;
		default:
			break;
		}
		__this_cpu_write(memcg->vmstats_percpu->targets[target], next);
		return true;
	}
	return false;
}

/*
 * Check events in order.
 *
 */
static void memcg_check_events(struct mem_cgroup *memcg, struct page *page)
{
	/* threshold event is triggered in finer grain than soft limit */
	if (unlikely(mem_cgroup_event_ratelimit(memcg,
						MEM_CGROUP_TARGET_THRESH))) {
		bool do_softlimit;
		bool do_numainfo __maybe_unused;

		do_softlimit = mem_cgroup_event_ratelimit(memcg,
						MEM_CGROUP_TARGET_SOFTLIMIT);
#if MAX_NUMNODES > 1
		do_numainfo = mem_cgroup_event_ratelimit(memcg,
						MEM_CGROUP_TARGET_NUMAINFO);
#endif
		mem_cgroup_threshold(memcg);
		if (unlikely(do_softlimit))
			mem_cgroup_update_tree(memcg, page);
#if MAX_NUMNODES > 1
		if (unlikely(do_numainfo))
			atomic_inc(&memcg->numainfo_events);
#endif
	}
}

struct mem_cgroup *mem_cgroup_from_task(struct task_struct *p)
{
	/*
	 * mm_update_next_owner() may clear mm->owner to NULL
	 * if it races with swapoff, page migration, etc.
	 * So this can be called with p == NULL.
	 */
	if (unlikely(!p))
		return NULL;

	return mem_cgroup_from_css(task_css(p, memory_cgrp_id));
}
EXPORT_SYMBOL(mem_cgroup_from_task);

/**
 * get_mem_cgroup_from_mm: Obtain a reference on given mm_struct's memcg.
 * @mm: mm from which memcg should be extracted. It can be NULL.
 *
 * Obtain a reference on mm->memcg and returns it if successful. Otherwise
 * root_mem_cgroup is returned. However if mem_cgroup is disabled, NULL is
 * returned.
 */
struct mem_cgroup *get_mem_cgroup_from_mm(struct mm_struct *mm)
{
	struct mem_cgroup *memcg;

	if (mem_cgroup_disabled())
		return NULL;

	rcu_read_lock();
	do {
		/*
		 * Page cache insertions can happen withou an
		 * actual mm context, e.g. during disk probing
		 * on boot, loopback IO, acct() writes etc.
		 */
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
}
EXPORT_SYMBOL(get_mem_cgroup_from_mm);

/**
 * get_mem_cgroup_from_page: Obtain a reference on given page's memcg.
 * @page: page from which memcg should be extracted.
 *
 * Obtain a reference on page->memcg and returns it if successful. Otherwise
 * root_mem_cgroup is returned.
 */
struct mem_cgroup *get_mem_cgroup_from_page(struct page *page)
{
	struct mem_cgroup *memcg = page->mem_cgroup;

	if (mem_cgroup_disabled())
		return NULL;

	rcu_read_lock();
	if (!memcg || !css_tryget_online(&memcg->css))
		memcg = root_mem_cgroup;
	rcu_read_unlock();
	return memcg;
}
EXPORT_SYMBOL(get_mem_cgroup_from_page);

/**
 * If current->active_memcg is non-NULL, do not fallback to current->mm->memcg.
 */
static __always_inline struct mem_cgroup *get_mem_cgroup_from_current(void)
{
	if (unlikely(current->active_memcg)) {
		struct mem_cgroup *memcg = root_mem_cgroup;

		rcu_read_lock();
		if (css_tryget_online(&current->active_memcg->css))
			memcg = current->active_memcg;
		rcu_read_unlock();
		return memcg;
	}
	return get_mem_cgroup_from_mm(current->mm);
}

/**
 * mem_cgroup_iter - iterate over memory cgroup hierarchy
 * @root: hierarchy root
 * @prev: previously returned memcg, NULL on first invocation
 * @reclaim: cookie for shared reclaim walks, NULL for full walks
 *
 * Returns references to children of the hierarchy below @root, or
 * @root itself, or %NULL after a full round-trip.
 *
 * Caller must pass the return value in @prev on subsequent
 * invocations for reference counting, or use mem_cgroup_iter_break()
 * to cancel a hierarchy walk before the round-trip is complete.
 *
 * Reclaimers can specify a node and a priority level in @reclaim to
 * divide up the memcgs in the hierarchy among all concurrent
 * reclaimers operating on the same node and priority.
 */
struct mem_cgroup *mem_cgroup_iter(struct mem_cgroup *root,
				   struct mem_cgroup *prev,
				   struct mem_cgroup_reclaim_cookie *reclaim)
{
	struct mem_cgroup_reclaim_iter *uninitialized_var(iter);
	struct cgroup_subsys_state *css = NULL;
	struct mem_cgroup *memcg = NULL;
	struct mem_cgroup *pos = NULL;

	if (mem_cgroup_disabled())
		return NULL;

	if (!root)
		root = root_mem_cgroup;

	if (prev && !reclaim)
		pos = prev;

	if (!root->use_hierarchy && root != root_mem_cgroup) {
		if (prev)
			goto out;
		return root;
	}

	rcu_read_lock();

	if (reclaim) {
		struct mem_cgroup_per_node *mz;

		mz = mem_cgroup_nodeinfo(root, reclaim->pgdat->node_id);
		iter = &mz->iter[reclaim->priority];

		if (prev && reclaim->generation != iter->generation)
			goto out_unlock;

		while (1) {
			pos = READ_ONCE(iter->position);
			if (!pos || css_tryget(&pos->css))
				break;
			/*
			 * css reference reached zero, so iter->position will
			 * be cleared by ->css_released. However, we should not
			 * rely on this happening soon, because ->css_released
			 * is called from a work queue, and by busy-waiting we
			 * might block it. So we clear iter->position right
			 * away.
			 */
			(void)cmpxchg(&iter->position, pos, NULL);
		}
	}

	if (pos)
		css = &pos->css;

	for (;;) {
		css = css_next_descendant_pre(css, &root->css);
		if (!css) {
			/*
			 * Reclaimers share the hierarchy walk, and a
			 * new one might jump in right at the end of
			 * the hierarchy - make sure they see at least
			 * one group and restart from the beginning.
			 */
			if (!prev)
				continue;
			break;
		}

		/*
		 * Verify the css and acquire a reference.  The root
		 * is provided by the caller, so we know it's alive
		 * and kicking, and don't take an extra reference.
		 */
		memcg = mem_cgroup_from_css(css);

		if (css == &root->css)
			break;

		if (css_tryget(css))
			break;

		memcg = NULL;
	}

	if (reclaim) {
		/*
		 * The position could have already been updated by a competing
		 * thread, so check that the value hasn't changed since we read
		 * it to avoid reclaiming from the same cgroup twice.
		 */
		(void)cmpxchg(&iter->position, pos, memcg);

		if (pos)
			css_put(&pos->css);

		if (!memcg)
			iter->generation++;
		else if (!prev)
			reclaim->generation = iter->generation;
	}

out_unlock:
	rcu_read_unlock();
out:
	if (prev && prev != root)
		css_put(&prev->css);

	return memcg;
}

/**
 * mem_cgroup_iter_break - abort a hierarchy walk prematurely
 * @root: hierarchy root
 * @prev: last visited hierarchy member as returned by mem_cgroup_iter()
 */
void mem_cgroup_iter_break(struct mem_cgroup *root,
			   struct mem_cgroup *prev)
{
	if (!root)
		root = root_mem_cgroup;
	if (prev && prev != root)
		css_put(&prev->css);
}

static void __invalidate_reclaim_iterators(struct mem_cgroup *from,
					struct mem_cgroup *dead_memcg)
{
	struct mem_cgroup_reclaim_iter *iter;
	struct mem_cgroup_per_node *mz;
	int nid;
	int i;

	for_each_node(nid) {
		mz = mem_cgroup_nodeinfo(from, nid);
		for (i = 0; i <= DEF_PRIORITY; i++) {
			iter = &mz->iter[i];
			cmpxchg(&iter->position,
				dead_memcg, NULL);
		}
	}
}

static void invalidate_reclaim_iterators(struct mem_cgroup *dead_memcg)
{
	struct mem_cgroup *memcg = dead_memcg;
	struct mem_cgroup *last;

	do {
		__invalidate_reclaim_iterators(memcg, dead_memcg);
		last = memcg;
	} while ((memcg = parent_mem_cgroup(memcg)));

	/*
	 * When cgruop1 non-hierarchy mode is used,
	 * parent_mem_cgroup() does not walk all the way up to the
	 * cgroup root (root_mem_cgroup). So we have to handle
	 * dead_memcg from cgroup root separately.
	 */
	if (last != root_mem_cgroup)
		__invalidate_reclaim_iterators(root_mem_cgroup,
						dead_memcg);
}

/* memcg oom priority */
/*
 * do_mem_cgroup_account_oom_skip - account the memcg with OOM-unkillable task
 * @memcg: mem_cgroup struct with OOM-unkillable task
 * @oc: oom_control struct
 *
 * Account OOM-unkillable task to its cgroup and up to the OOMing cgroup's
 * @num_oom_skip, if all the tasks of one cgroup hierarchy are OOM-unkillable
 * we skip this cgroup hierarchy when select the victim cgroup.
 *
 * The @num_oom_skip must be reset when bad process selection has finished,
 * since before the next round bad process selection, these OOM-unkillable
 * tasks might become killable.
 *
 */
static void do_mem_cgroup_account_oom_skip(struct mem_cgroup *memcg,
					   struct oom_control *oc)
{
	struct mem_cgroup *root;
	struct cgroup_subsys_state *css;

	if (!oc->use_priority_oom)
		return;
	if (unlikely(!memcg))
		return;
	root = oc->memcg;
	if (!root)
		root = root_mem_cgroup;

	css = &memcg->css;
	while (css) {
		struct mem_cgroup *tmp;

		tmp = mem_cgroup_from_css(css);
		tmp->num_oom_skip++;
		/*
		 * Put these cgroups into a list to
		 * reduce the iteration time when reset
		 * the @num_oom_skip.
		 */
		if (!tmp->next_reset) {
			css_get(&tmp->css);
			tmp->next_reset = oc->reset_list;
			oc->reset_list = tmp;
		}

		if (mem_cgroup_from_css(css) == root)
			break;

		css = css->parent;
	}
}

void mem_cgroup_account_oom_skip(struct task_struct *task,
		struct oom_control *oc)
{
	do_mem_cgroup_account_oom_skip(mem_cgroup_from_task(task), oc);
}

static struct mem_cgroup *
mem_cgroup_select_victim_cgroup(struct mem_cgroup *memcg)
{
	struct cgroup_subsys_state *chosen, *parent;
	struct cgroup_subsys_state *victim;
	int chosen_priority;

	if (!memcg->use_hierarchy) {
		css_get(&memcg->css);
		return memcg;
	}
again:
	victim = NULL;
	parent = &memcg->css;
	rcu_read_lock();
	while (parent) {
		struct cgroup_subsys_state *pos;
		struct mem_cgroup *parent_mem;

		parent_mem = mem_cgroup_from_css(parent);

		if (parent->nr_procs <= parent_mem->num_oom_skip)
			break;
		victim = parent;
		chosen = NULL;
		chosen_priority = DEF_PRIORITY + 1;
		list_for_each_entry_rcu(pos, &parent->children, sibling) {
			struct mem_cgroup *tmp, *chosen_mem;

			tmp = mem_cgroup_from_css(pos);

			if (pos->nr_procs <= tmp->num_oom_skip)
				continue;
			if (tmp->priority > chosen_priority)
				continue;
			if (tmp->priority < chosen_priority) {
				chosen_priority = tmp->priority;
				chosen = pos;
				continue;
			}

			chosen_mem = mem_cgroup_from_css(chosen);

			if (do_memsw_account()) {
				if (page_counter_read(&tmp->memsw) >
					page_counter_read(&chosen_mem->memsw))
					chosen = pos;
			} else if (page_counter_read(&tmp->memory) >
				page_counter_read(&chosen_mem->memory)) {
				chosen = pos;
			}
		}
		parent = chosen;
	}

	if (likely(victim)) {
		if (!css_tryget(victim)) {
			rcu_read_unlock();
			goto again;
		}
	}

	rcu_read_unlock();

	if (likely(victim))
		return mem_cgroup_from_css(victim);

	return NULL;
}

/**
 * mem_cgroup_scan_tasks - iterate over tasks of a memory cgroup hierarchy
 * @memcg: hierarchy root
 * @fn: function to call for each task
 * @arg: argument passed to @fn
 *
 * This function iterates over tasks attached to @memcg or to any of its
 * descendants and calls @fn for each task. If @fn returns a non-zero
 * value, the function breaks the iteration loop and returns the value.
 * Otherwise, it will iterate over all tasks and return 0.
 *
 */
int mem_cgroup_scan_tasks(struct mem_cgroup *memcg,
			  int (*fn)(struct task_struct *, void *), void *arg)
{
	struct mem_cgroup *iter;
	int ret = 0;

	for_each_mem_cgroup_tree(iter, memcg) {
		struct css_task_iter it;
		struct task_struct *task;

		css_task_iter_start(&iter->css, CSS_TASK_ITER_PROCS, &it);
		while (!ret && (task = css_task_iter_next(&it)))
			ret = fn(task, arg);
		css_task_iter_end(&it);
		if (ret) {
			mem_cgroup_iter_break(memcg, iter);
			break;
		}
	}
	return ret;
}

void mem_cgroup_select_bad_process(struct oom_control *oc)
{
	struct mem_cgroup *memcg, *victim, *iter;

	memcg  = oc->memcg;

	if (!memcg)
		memcg = root_mem_cgroup;

	oc->use_priority_oom = memcg->use_priority_oom;
	victim = memcg;

retry:
	if (oc->use_priority_oom) {
		victim = mem_cgroup_select_victim_cgroup(memcg);
		if (!victim) {
			if (mem_cgroup_is_root(memcg) && oc->num_skip)
				oc->chosen = (void *)-1UL;
			goto out;
		}
	}

	mem_cgroup_scan_tasks(victim, oom_evaluate_task, oc);
	if (oc->use_priority_oom) {
		css_put(&victim->css);
		if (oc->chosen == (void *)-1UL)
			goto out;
		if (!oc->chosen && victim != memcg) {
			do_mem_cgroup_account_oom_skip(victim, oc);
			goto retry;
		}
	}
out:
	/* See commets in mem_cgroup_account_oom_skip() */
	while (oc->reset_list) {
		iter = oc->reset_list;
		iter->num_oom_skip = 0;
		oc->reset_list = iter->next_reset;
		iter->next_reset = NULL;
		css_put(&iter->css);
	}
}
/**
 * mem_cgroup_page_lruvec - return lruvec for isolating/putting an LRU page
 * @page: the page
 * @pgdat: pgdat of the page
 *
 * This function relies on page->mem_cgroup being stable - see the
 * access rules in commit_charge().
 */
struct lruvec *mem_cgroup_page_lruvec(struct page *page, struct pglist_data *pgdat)
{
	struct mem_cgroup_per_node *mz;
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;

	if (mem_cgroup_disabled()) {
		lruvec = &pgdat->__lruvec;
		goto out;
	}

	memcg = page->mem_cgroup;
	/*
	 * Swapcache readahead pages are added to the LRU - and
	 * possibly migrated - before they are charged.
	 */
	if (!memcg)
		memcg = root_mem_cgroup;

	mz = mem_cgroup_page_nodeinfo(memcg, page);
	lruvec = &mz->lruvec;
out:
	/*
	 * Since a node can be onlined after the mem_cgroup was created,
	 * we have to be prepared to initialize lruvec->zone here;
	 * and if offlined then reonlined, we need to reinitialize it.
	 */
	if (unlikely(lruvec->pgdat != pgdat))
		lruvec->pgdat = pgdat;
	return lruvec;
}

/**
 * mem_cgroup_update_lru_size - account for adding or removing an lru page
 * @lruvec: mem_cgroup per zone lru vector
 * @lru: index of lru list the page is sitting on
 * @zid: zone id of the accounted pages
 * @nr_pages: positive when adding or negative when removing
 *
 * This function must be called under lru_lock, just before a page is added
 * to or just after a page is removed from an lru list (that ordering being
 * so as to allow it to check that lru_size 0 is consistent with list_empty).
 */
void mem_cgroup_update_lru_size(struct lruvec *lruvec, enum lru_list lru,
				int zid, int nr_pages)
{
	struct mem_cgroup_per_node *mz;
	unsigned long *lru_size;
	long size;

	if (mem_cgroup_disabled())
		return;

	mz = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	lru_size = &mz->lru_zone_size[zid][lru];

	if (nr_pages < 0)
		*lru_size += nr_pages;

	size = *lru_size;
	if (WARN_ONCE(size < 0,
		"%s(%p, %d, %d): lru_size %ld\n",
		__func__, lruvec, lru, nr_pages, size)) {
		VM_BUG_ON(1);
		*lru_size = 0;
	}

	if (nr_pages > 0)
		*lru_size += nr_pages;
}

bool task_in_mem_cgroup(struct task_struct *task, struct mem_cgroup *memcg)
{
	struct mem_cgroup *task_memcg;
	struct task_struct *p;
	bool ret;

	p = find_lock_task_mm(task);
	if (p) {
		task_memcg = get_mem_cgroup_from_mm(p->mm);
		task_unlock(p);
	} else {
		/*
		 * All threads may have already detached their mm's, but the oom
		 * killer still needs to detect if they have already been oom
		 * killed to prevent needlessly killing additional tasks.
		 */
		rcu_read_lock();
		task_memcg = mem_cgroup_from_task(task);
		css_get(&task_memcg->css);
		rcu_read_unlock();
	}
	ret = mem_cgroup_is_descendant(task_memcg, memcg);
	css_put(&task_memcg->css);
	return ret;
}

/**
 * mem_cgroup_margin - calculate chargeable space of a memory cgroup
 * @memcg: the memory cgroup
 *
 * Returns the maximum amount of memory @mem can be charged with, in
 * pages.
 */
static unsigned long mem_cgroup_margin(struct mem_cgroup *memcg)
{
	unsigned long margin = 0;
	unsigned long count;
	unsigned long limit;

	count = page_counter_read(&memcg->memory);
	limit = READ_ONCE(memcg->memory.max);
	if (count < limit)
		margin = limit - count;

	if (do_memsw_account()) {
		count = page_counter_read(&memcg->memsw);
		limit = READ_ONCE(memcg->memsw.max);
		if (count <= limit)
			margin = min(margin, limit - count);
		else
			margin = 0;
	}

	return margin;
}

/*
 * A routine for checking "mem" is under move_account() or not.
 *
 * Checking a cgroup is mc.from or mc.to or under hierarchy of
 * moving cgroups. This is for waiting at high-memory pressure
 * caused by "move".
 */
static bool mem_cgroup_under_move(struct mem_cgroup *memcg)
{
	struct mem_cgroup *from;
	struct mem_cgroup *to;
	bool ret = false;
	/*
	 * Unlike task_move routines, we access mc.to, mc.from not under
	 * mutual exclusion by cgroup_mutex. Here, we take spinlock instead.
	 */
	spin_lock(&mc.lock);
	from = mc.from;
	to = mc.to;
	if (!from)
		goto unlock;

	ret = mem_cgroup_is_descendant(from, memcg) ||
		mem_cgroup_is_descendant(to, memcg);
unlock:
	spin_unlock(&mc.lock);
	return ret;
}

static bool mem_cgroup_wait_acct_move(struct mem_cgroup *memcg)
{
	if (mc.moving_task && current != mc.moving_task) {
		if (mem_cgroup_under_move(memcg)) {
			DEFINE_WAIT(wait);
			prepare_to_wait(&mc.waitq, &wait, TASK_INTERRUPTIBLE);
			/* moving charge context might have finished. */
			if (mc.moving_task)
				schedule();
			finish_wait(&mc.waitq, &wait);
			return true;
		}
	}
	return false;
}

static const unsigned int memcg1_stats[] = {
	NR_FILE_PAGES,
	NR_ANON_MAPPED,
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	NR_ANON_THPS,
#endif
	NR_SHMEM,
	NR_FILE_MAPPED,
	NR_FILE_DIRTY,
	NR_WRITEBACK,
	MEMCG_SWAP,
	WORKINGSET_REFAULT_ANON,
	WORKINGSET_REFAULT_FILE,
	WORKINGSET_ACTIVATE_ANON,
	WORKINGSET_ACTIVATE_FILE,
	WORKINGSET_RESTORE_ANON,
	WORKINGSET_RESTORE_FILE,
	WORKINGSET_NODERECLAIM,
};

static const char *const memcg1_stat_names[] = {
	"cache",
	"rss",
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	"rss_huge",
#endif
	"shmem",
	"mapped_file",
	"dirty",
	"writeback",
	"swap",
	"workingset_refault_anon",
	"workingset_refault_file",
	"workingset_activate_anon",
	"workingset_activate_file",
	"workingset_restore_anon",
	"workingset_restore_file",
	"workingset_nodereclaim",
};

#define K(x) ((x) << (PAGE_SHIFT-10))
/**
 * mem_cgroup_print_oom_info: Print OOM information relevant to memory controller.
 * @memcg: The memory cgroup that went over limit
 * @p: Task that is going to be killed
 *
 * NOTE: @memcg and @p's mem_cgroup can be different when hierarchy is
 * enabled
 */
void mem_cgroup_print_oom_info(struct mem_cgroup *memcg, struct task_struct *p)
{
	struct mem_cgroup *iter;
	unsigned int i;

	rcu_read_lock();

	if (p) {
		pr_info("Task in ");
		pr_cont_cgroup_path(task_cgroup(p, memory_cgrp_id));
		pr_cont(" killed as a result of limit of ");
	} else {
		pr_info("Memory limit reached of cgroup ");
	}

	pr_cont_cgroup_path(memcg->css.cgroup);
	pr_cont("\n");

	rcu_read_unlock();

	pr_info("memory: usage %llukB, limit %llukB, failcnt %lu\n",
		K((u64)page_counter_read(&memcg->memory)),
		K((u64)READ_ONCE(memcg->memory.max)), memcg->memory.failcnt);
	pr_info("memory+swap: usage %llukB, limit %llukB, failcnt %lu\n",
		K((u64)page_counter_read(&memcg->memsw)),
		K((u64)READ_ONCE(memcg->memsw.max)), memcg->memsw.failcnt);
	pr_info("kmem: usage %llukB, limit %llukB, failcnt %lu\n",
		K((u64)page_counter_read(&memcg->kmem)),
		K((u64)memcg->kmem.max), memcg->kmem.failcnt);

	for_each_mem_cgroup_tree(iter, memcg) {
		pr_info("Memory cgroup stats for ");
		pr_cont_cgroup_path(iter->css.cgroup);
		pr_cont(":");

		for (i = 0; i < ARRAY_SIZE(memcg1_stats); i++) {
			unsigned long nr;

			if (memcg1_stats[i] == MEMCG_SWAP && cgroup_memory_noswap)
				continue;
			nr = memcg_page_state_local(iter, memcg1_stats[i]);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			if (memcg1_stats[i] == NR_ANON_THPS)
				nr *= HPAGE_PMD_NR;
#endif
			pr_cont(" %s:%luKB", memcg1_stat_names[i], K(nr));
		}

		for (i = 0; i < NR_LRU_LISTS; i++)
			pr_cont(" %s:%luKB", mem_cgroup_lru_names[i],
				K(memcg_page_state_local(iter, NR_LRU_BASE + i)));

		pr_cont("\n");
	}
}

/*
 * Return the memory (and swap, if configured) limit for a memcg.
 */
unsigned long mem_cgroup_get_max(struct mem_cgroup *memcg)
{
	unsigned long max;

	max = READ_ONCE(memcg->memory.max);
	if (mem_cgroup_swappiness(memcg) > 0) {
		unsigned long memsw_max;
		unsigned long swap_max;

		memsw_max = memcg->memsw.max;
		swap_max = READ_ONCE(memcg->swap.max);
		swap_max = min(swap_max, (unsigned long)total_swap_pages);
		max = min(max + swap_max, memsw_max);
	}
	return max;
}

static bool mem_cgroup_out_of_memory(struct mem_cgroup *memcg, gfp_t gfp_mask,
				     int order)
{
	struct oom_control oc = {
		.zonelist = NULL,
		.nodemask = NULL,
		.memcg = memcg,
		.gfp_mask = gfp_mask,
		.order = order,
	};
	bool ret = true;

	if (mutex_lock_killable(&oom_lock))
		return true;

	if (mem_cgroup_margin(memcg) >= (1 << order))
		goto unlock;

	/*
	 * A few threads which were not waiting at mutex_lock_killable() can
	 * fail to bail out. Therefore, check again after holding oom_lock.
	 */
	ret = should_force_charge() || out_of_memory(&oc);

unlock:
	mutex_unlock(&oom_lock);
	return ret;
}

#if MAX_NUMNODES > 1

/**
 * test_mem_cgroup_node_reclaimable
 * @memcg: the target memcg
 * @nid: the node ID to be checked.
 * @noswap : specify true here if the user wants flle only information.
 *
 * This function returns whether the specified memcg contains any
 * reclaimable pages on a node. Returns true if there are any reclaimable
 * pages in the node.
 */
static bool test_mem_cgroup_node_reclaimable(struct mem_cgroup *memcg,
		int nid, bool noswap)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));

	if (lruvec_page_state(lruvec, NR_INACTIVE_FILE) ||
	    lruvec_page_state(lruvec, NR_ACTIVE_FILE))
		return true;
	if (noswap || !total_swap_pages)
		return false;
	if (lruvec_page_state(lruvec, NR_INACTIVE_ANON) ||
	    lruvec_page_state(lruvec, NR_ACTIVE_ANON))
		return true;
	return false;

}

/*
 * Always updating the nodemask is not very good - even if we have an empty
 * list or the wrong list here, we can start from some node and traverse all
 * nodes based on the zonelist. So update the list loosely once per 10 secs.
 *
 */
static void mem_cgroup_may_update_nodemask(struct mem_cgroup *memcg)
{
	int nid;
	/*
	 * numainfo_events > 0 means there was at least NUMAINFO_EVENTS_TARGET
	 * pagein/pageout changes since the last update.
	 */
	if (!atomic_read(&memcg->numainfo_events))
		return;
	if (atomic_inc_return(&memcg->numainfo_updating) > 1)
		return;

	/* make a nodemask where this memcg uses memory from */
	memcg->scan_nodes = node_states[N_MEMORY];

	for_each_node_mask(nid, node_states[N_MEMORY]) {

		if (!test_mem_cgroup_node_reclaimable(memcg, nid, false))
			node_clear(nid, memcg->scan_nodes);
	}

	atomic_set(&memcg->numainfo_events, 0);
	atomic_set(&memcg->numainfo_updating, 0);
}

/*
 * Selecting a node where we start reclaim from. Because what we need is just
 * reducing usage counter, start from anywhere is O,K. Considering
 * memory reclaim from current node, there are pros. and cons.
 *
 * Freeing memory from current node means freeing memory from a node which
 * we'll use or we've used. So, it may make LRU bad. And if several threads
 * hit limits, it will see a contention on a node. But freeing from remote
 * node means more costs for memory reclaim because of memory latency.
 *
 * Now, we use round-robin. Better algorithm is welcomed.
 */
int mem_cgroup_select_victim_node(struct mem_cgroup *memcg)
{
	int node;

	mem_cgroup_may_update_nodemask(memcg);
	node = memcg->last_scanned_node;

	node = next_node_in(node, memcg->scan_nodes);
	/*
	 * mem_cgroup_may_update_nodemask might have seen no reclaimmable pages
	 * last time it really checked all the LRUs due to rate limiting.
	 * Fallback to the current node in that case for simplicity.
	 */
	if (unlikely(node == MAX_NUMNODES))
		node = numa_node_id();

	memcg->last_scanned_node = node;
	return node;
}
#else
int mem_cgroup_select_victim_node(struct mem_cgroup *memcg)
{
	return 0;
}
#endif

static int mem_cgroup_soft_reclaim(struct mem_cgroup *root_memcg,
				   pg_data_t *pgdat,
				   gfp_t gfp_mask,
				   unsigned long *total_scanned)
{
	struct mem_cgroup *victim = NULL;
	int total = 0;
	int loop = 0;
	unsigned long excess;
	unsigned long nr_scanned;
	struct mem_cgroup_reclaim_cookie reclaim = {
		.pgdat = pgdat,
		.priority = 0,
	};

	excess = soft_limit_excess(root_memcg);

	while (1) {
		victim = mem_cgroup_iter(root_memcg, victim, &reclaim);
		if (!victim) {
			loop++;
			if (loop >= 2) {
				/*
				 * If we have not been able to reclaim
				 * anything, it might because there are
				 * no reclaimable pages under this hierarchy
				 */
				if (!total)
					break;
				/*
				 * We want to do more targeted reclaim.
				 * excess >> 2 is not to excessive so as to
				 * reclaim too much, nor too less that we keep
				 * coming back to reclaim from this cgroup
				 */
				if (total >= (excess >> 2) ||
					(loop > MEM_CGROUP_MAX_RECLAIM_LOOPS))
					break;
			}
			continue;
		}
		total += mem_cgroup_shrink_node(victim, gfp_mask, false,
					pgdat, &nr_scanned);
		*total_scanned += nr_scanned;
		if (!soft_limit_excess(root_memcg))
			break;
	}
	mem_cgroup_iter_break(root_memcg, victim);
	return total;
}

#ifdef CONFIG_LOCKDEP
static struct lockdep_map memcg_oom_lock_dep_map = {
	.name = "memcg_oom_lock",
};
#endif

static DEFINE_SPINLOCK(memcg_oom_lock);

/*
 * Check OOM-Killer is already running under our hierarchy.
 * If someone is running, return false.
 */
static bool mem_cgroup_oom_trylock(struct mem_cgroup *memcg)
{
	struct mem_cgroup *iter, *failed = NULL;

	spin_lock(&memcg_oom_lock);

	for_each_mem_cgroup_tree(iter, memcg) {
		if (iter->oom_lock) {
			/*
			 * this subtree of our hierarchy is already locked
			 * so we cannot give a lock.
			 */
			failed = iter;
			mem_cgroup_iter_break(memcg, iter);
			break;
		} else
			iter->oom_lock = true;
	}

	if (failed) {
		/*
		 * OK, we failed to lock the whole subtree so we have
		 * to clean up what we set up to the failing subtree
		 */
		for_each_mem_cgroup_tree(iter, memcg) {
			if (iter == failed) {
				mem_cgroup_iter_break(memcg, iter);
				break;
			}
			iter->oom_lock = false;
		}
	} else
		mutex_acquire(&memcg_oom_lock_dep_map, 0, 1, _RET_IP_);

	spin_unlock(&memcg_oom_lock);

	return !failed;
}

static void mem_cgroup_oom_unlock(struct mem_cgroup *memcg)
{
	struct mem_cgroup *iter;

	spin_lock(&memcg_oom_lock);
	mutex_release(&memcg_oom_lock_dep_map, 1, _RET_IP_);
	for_each_mem_cgroup_tree(iter, memcg)
		iter->oom_lock = false;
	spin_unlock(&memcg_oom_lock);
}

static void mem_cgroup_mark_under_oom(struct mem_cgroup *memcg)
{
	struct mem_cgroup *iter;

	spin_lock(&memcg_oom_lock);
	for_each_mem_cgroup_tree(iter, memcg)
		iter->under_oom++;
	spin_unlock(&memcg_oom_lock);
}

static void mem_cgroup_unmark_under_oom(struct mem_cgroup *memcg)
{
	struct mem_cgroup *iter;

	/*
	 * When a new child is created while the hierarchy is under oom,
	 * mem_cgroup_oom_lock() may not be called. Watch for underflow.
	 */
	spin_lock(&memcg_oom_lock);
	for_each_mem_cgroup_tree(iter, memcg)
		if (iter->under_oom > 0)
			iter->under_oom--;
	spin_unlock(&memcg_oom_lock);
}

static DECLARE_WAIT_QUEUE_HEAD(memcg_oom_waitq);

struct oom_wait_info {
	struct mem_cgroup *memcg;
	wait_queue_entry_t	wait;
};

static int memcg_oom_wake_function(wait_queue_entry_t *wait,
	unsigned mode, int sync, void *arg)
{
	struct mem_cgroup *wake_memcg = (struct mem_cgroup *)arg;
	struct mem_cgroup *oom_wait_memcg;
	struct oom_wait_info *oom_wait_info;

	oom_wait_info = container_of(wait, struct oom_wait_info, wait);
	oom_wait_memcg = oom_wait_info->memcg;

	if (!mem_cgroup_is_descendant(wake_memcg, oom_wait_memcg) &&
	    !mem_cgroup_is_descendant(oom_wait_memcg, wake_memcg))
		return 0;
	return autoremove_wake_function(wait, mode, sync, arg);
}

static void memcg_oom_recover(struct mem_cgroup *memcg)
{
	/*
	 * For the following lockless ->under_oom test, the only required
	 * guarantee is that it must see the state asserted by an OOM when
	 * this function is called as a result of userland actions
	 * triggered by the notification of the OOM.  This is trivially
	 * achieved by invoking mem_cgroup_mark_under_oom() before
	 * triggering notification.
	 */
	if (memcg && memcg->under_oom)
		__wake_up(&memcg_oom_waitq, TASK_NORMAL, 0, memcg);
}

enum oom_status {
	OOM_SUCCESS,
	OOM_FAILED,
	OOM_ASYNC,
	OOM_SKIPPED
};

static enum oom_status mem_cgroup_oom(struct mem_cgroup *memcg, gfp_t mask, int order)
{
	enum oom_status ret;
	bool locked;

	if (order > PAGE_ALLOC_COSTLY_ORDER)
		return OOM_SKIPPED;

	memcg_memory_event(memcg, MEMCG_OOM);

	/*
	 * We are in the middle of the charge context here, so we
	 * don't want to block when potentially sitting on a callstack
	 * that holds all kinds of filesystem and mm locks.
	 *
	 * cgroup1 allows disabling the OOM killer and waiting for outside
	 * handling until the charge can succeed; remember the context and put
	 * the task to sleep at the end of the page fault when all locks are
	 * released.
	 *
	 * On the other hand, in-kernel OOM killer allows for an async victim
	 * memory reclaim (oom_reaper) and that means that we are not solely
	 * relying on the oom victim to make a forward progress and we can
	 * invoke the oom killer here.
	 *
	 * Please note that mem_cgroup_out_of_memory might fail to find a
	 * victim and then we have to bail out from the charge path.
	 */
	if (memcg->oom_kill_disable) {
		if (!current->in_user_fault)
			return OOM_SKIPPED;
		css_get(&memcg->css);
		current->memcg_in_oom = memcg;
		current->memcg_oom_gfp_mask = mask;
		current->memcg_oom_order = order;

		return OOM_ASYNC;
	}

	mem_cgroup_mark_under_oom(memcg);

	locked = mem_cgroup_oom_trylock(memcg);

	if (locked)
		mem_cgroup_oom_notify(memcg);

	mem_cgroup_unmark_under_oom(memcg);
	if (mem_cgroup_out_of_memory(memcg, mask, order))
		ret = OOM_SUCCESS;
	else
		ret = OOM_FAILED;

	if (locked)
		mem_cgroup_oom_unlock(memcg);

	return ret;
}

/**
 * mem_cgroup_oom_synchronize - complete memcg OOM handling
 * @handle: actually kill/wait or just clean up the OOM state
 *
 * This has to be called at the end of a page fault if the memcg OOM
 * handler was enabled.
 *
 * Memcg supports userspace OOM handling where failed allocations must
 * sleep on a waitqueue until the userspace task resolves the
 * situation.  Sleeping directly in the charge context with all kinds
 * of locks held is not a good idea, instead we remember an OOM state
 * in the task and mem_cgroup_oom_synchronize() has to be called at
 * the end of the page fault to complete the OOM handling.
 *
 * Returns %true if an ongoing memcg OOM situation was detected and
 * completed, %false otherwise.
 */
bool mem_cgroup_oom_synchronize(bool handle)
{
	struct mem_cgroup *memcg = current->memcg_in_oom;
	struct oom_wait_info owait;
	bool locked;

	/* OOM is global, do not handle */
	if (!memcg)
		return false;

	if (!handle)
		goto cleanup;

	owait.memcg = memcg;
	owait.wait.flags = 0;
	owait.wait.func = memcg_oom_wake_function;
	owait.wait.private = current;
	INIT_LIST_HEAD(&owait.wait.entry);

	prepare_to_wait(&memcg_oom_waitq, &owait.wait, TASK_KILLABLE);
	mem_cgroup_mark_under_oom(memcg);

	locked = mem_cgroup_oom_trylock(memcg);

	if (locked)
		mem_cgroup_oom_notify(memcg);

	if (locked && !memcg->oom_kill_disable) {
		mem_cgroup_unmark_under_oom(memcg);
		finish_wait(&memcg_oom_waitq, &owait.wait);
		mem_cgroup_out_of_memory(memcg, current->memcg_oom_gfp_mask,
					 current->memcg_oom_order);
	} else {
		schedule();
		mem_cgroup_unmark_under_oom(memcg);
		finish_wait(&memcg_oom_waitq, &owait.wait);
	}

	if (locked) {
		mem_cgroup_oom_unlock(memcg);
		/*
		 * There is no guarantee that an OOM-lock contender
		 * sees the wakeups triggered by the OOM kill
		 * uncharges.  Wake any sleepers explicitely.
		 */
		memcg_oom_recover(memcg);
	}
cleanup:
	current->memcg_in_oom = NULL;
	css_put(&memcg->css);
	return true;
}

/**
 * mem_cgroup_get_oom_group - get a memory cgroup to clean up after OOM
 * @victim: task to be killed by the OOM killer
 * @oom_domain: memcg in case of memcg OOM, NULL in case of system-wide OOM
 *
 * Returns a pointer to a memory cgroup, which has to be cleaned up
 * by killing all belonging OOM-killable tasks.
 *
 * Caller has to call mem_cgroup_put() on the returned non-NULL memcg.
 */
struct mem_cgroup *mem_cgroup_get_oom_group(struct task_struct *victim,
					    struct mem_cgroup *oom_domain)
{
	struct mem_cgroup *oom_group = NULL;
	struct mem_cgroup *memcg;

	if (!oom_domain)
		oom_domain = root_mem_cgroup;

	rcu_read_lock();

	memcg = mem_cgroup_from_task(victim);
	if (memcg == root_mem_cgroup)
		goto out;

	/*
	 * Traverse the memory cgroup hierarchy from the victim task's
	 * cgroup up to the OOMing cgroup (or root) to find the
	 * highest-level memory cgroup with oom.group set.
	 */
	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		if (memcg->oom_group)
			oom_group = memcg;

		if (memcg == oom_domain)
			break;
	}

	if (oom_group)
		css_get(&oom_group->css);
out:
	rcu_read_unlock();

	return oom_group;
}

void mem_cgroup_print_oom_group(struct mem_cgroup *memcg)
{
	pr_info("Tasks in ");
	pr_cont_cgroup_path(memcg->css.cgroup);
	pr_cont(" are going to be killed due to memory.oom.group set\n");
}

/**
 * lock_page_memcg - lock a page->mem_cgroup binding
 * @page: the page
 *
 * This function protects unlocked LRU pages from being moved to
 * another cgroup.
 *
 * It ensures lifetime of the returned memcg. Caller is responsible
 * for the lifetime of the page; __unlock_page_memcg() is available
 * when @page might get freed inside the locked section.
 */
struct mem_cgroup *lock_page_memcg(struct page *page)
{
	struct page *head = compound_head(page); /* rmap on tail pages */
	struct mem_cgroup *memcg;
	unsigned long flags;

	/*
	 * The RCU lock is held throughout the transaction.  The fast
	 * path can get away without acquiring the memcg->move_lock
	 * because page moving starts with an RCU grace period.
	 *
	 * The RCU lock also protects the memcg from being freed when
	 * the page state that is going to change is the only thing
	 * preventing the page itself from being freed. E.g. writeback
	 * doesn't hold a page reference and relies on PG_writeback to
	 * keep off truncation, migration and so forth.
         */
	rcu_read_lock();

	if (mem_cgroup_disabled())
		return NULL;
again:
	memcg = head->mem_cgroup;
	if (unlikely(!memcg))
		return NULL;

	if (atomic_read(&memcg->moving_account) <= 0)
		return memcg;

	spin_lock_irqsave(&memcg->move_lock, flags);
	if (memcg != head->mem_cgroup) {
		spin_unlock_irqrestore(&memcg->move_lock, flags);
		goto again;
	}

	/*
	 * When charge migration first begins, we can have locked and
	 * unlocked page stat updates happening concurrently.  Track
	 * the task who has the lock for unlock_page_memcg().
	 */
	memcg->move_lock_task = current;
	memcg->move_lock_flags = flags;

	return memcg;
}
EXPORT_SYMBOL(lock_page_memcg);

/**
 * __unlock_page_memcg - unlock and unpin a memcg
 * @memcg: the memcg
 *
 * Unlock and unpin a memcg returned by lock_page_memcg().
 */
void __unlock_page_memcg(struct mem_cgroup *memcg)
{
	if (memcg && memcg->move_lock_task == current) {
		unsigned long flags = memcg->move_lock_flags;

		memcg->move_lock_task = NULL;
		memcg->move_lock_flags = 0;

		spin_unlock_irqrestore(&memcg->move_lock, flags);
	}

	rcu_read_unlock();
}

/**
 * unlock_page_memcg - unlock a page->mem_cgroup binding
 * @page: the page
 */
void unlock_page_memcg(struct page *page)
{
	struct page *head = compound_head(page);

	__unlock_page_memcg(head->mem_cgroup);
}
EXPORT_SYMBOL(unlock_page_memcg);

struct memcg_stock_pcp {
	struct mem_cgroup *cached; /* this never be root cgroup */
	unsigned int nr_pages;
	struct work_struct work;
	unsigned long flags;
#define FLUSHING_CACHED_CHARGE	0
};
static DEFINE_PER_CPU(struct memcg_stock_pcp, memcg_stock);
static DEFINE_MUTEX(percpu_charge_mutex);

/**
 * consume_stock: Try to consume stocked charge on this cpu.
 * @memcg: memcg to consume from.
 * @nr_pages: how many pages to charge.
 *
 * The charges will only happen if @memcg matches the current cpu's memcg
 * stock, and at least @nr_pages are available in that stock.  Failure to
 * service an allocation will refill the stock.
 *
 * returns true if successful, false otherwise.
 */
static bool consume_stock(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	struct memcg_stock_pcp *stock;
	unsigned long flags;
	bool ret = false;

	if (nr_pages > MEMCG_CHARGE_BATCH)
		return ret;

	local_irq_save(flags);

	stock = this_cpu_ptr(&memcg_stock);
	if (memcg == stock->cached && stock->nr_pages >= nr_pages) {
		stock->nr_pages -= nr_pages;
		ret = true;
	}

	local_irq_restore(flags);

	return ret;
}

/*
 * Returns stocks cached in percpu and reset cached information.
 */
static void drain_stock(struct memcg_stock_pcp *stock)
{
	struct mem_cgroup *old = stock->cached;

	if (stock->nr_pages) {
		page_counter_uncharge(&old->memory, stock->nr_pages);
		if (do_memsw_account())
			page_counter_uncharge(&old->memsw, stock->nr_pages);
		css_put_many(&old->css, stock->nr_pages);
		stock->nr_pages = 0;
	}
	stock->cached = NULL;
}

static void drain_local_stock(struct work_struct *dummy)
{
	struct memcg_stock_pcp *stock;
	unsigned long flags;

	/*
	 * The only protection from memory hotplug vs. drain_stock races is
	 * that we always operate on local CPU stock here with IRQ disabled
	 */
	local_irq_save(flags);

	stock = this_cpu_ptr(&memcg_stock);
	drain_stock(stock);
	clear_bit(FLUSHING_CACHED_CHARGE, &stock->flags);

	local_irq_restore(flags);
}

/*
 * Cache charges(val) to local per_cpu area.
 * This will be consumed by consume_stock() function, later.
 */
static void refill_stock(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	struct memcg_stock_pcp *stock;
	unsigned long flags;

	local_irq_save(flags);

	stock = this_cpu_ptr(&memcg_stock);
	if (stock->cached != memcg) { /* reset if necessary */
		drain_stock(stock);
		stock->cached = memcg;
	}
	stock->nr_pages += nr_pages;

	if (stock->nr_pages > MEMCG_CHARGE_BATCH)
		drain_stock(stock);

	local_irq_restore(flags);
}

/*
 * Drains all per-CPU charge caches for given root_memcg resp. subtree
 * of the hierarchy under it.
 */
void drain_all_stock(struct mem_cgroup *root_memcg)
{
	int cpu, curcpu;

	/* If someone's already draining, avoid adding running more workers. */
	if (!mutex_trylock(&percpu_charge_mutex))
		return;
	/*
	 * Notify other cpus that system-wide "drain" is running
	 * We do not care about races with the cpu hotplug because cpu down
	 * as well as workers from this path always operate on the local
	 * per-cpu data. CPU up doesn't touch memcg_stock at all.
	 */
	curcpu = get_cpu();
	for_each_online_cpu(cpu) {
		struct memcg_stock_pcp *stock = &per_cpu(memcg_stock, cpu);
		struct mem_cgroup *memcg;

		memcg = stock->cached;
		if (!memcg || !stock->nr_pages || !css_tryget(&memcg->css))
			continue;
		if (!mem_cgroup_is_descendant(memcg, root_memcg)) {
			css_put(&memcg->css);
			continue;
		}
		if (!test_and_set_bit(FLUSHING_CACHED_CHARGE, &stock->flags)) {
			if (cpu == curcpu)
				drain_local_stock(&stock->work);
			else
				schedule_work_on(cpu, &stock->work);
		}
		css_put(&memcg->css);
	}
	put_cpu();
	mutex_unlock(&percpu_charge_mutex);
}

static int memcg_hotplug_cpu_dead(unsigned int cpu)
{
	struct memcg_stock_pcp *stock;
	struct mem_cgroup *memcg, *mi;

	stock = &per_cpu(memcg_stock, cpu);
	drain_stock(stock);

	for_each_mem_cgroup(memcg) {
		int i;

		for (i = 0; i < MEMCG_NR_STAT; i++) {
			int nid;
			long x;

			x = this_cpu_xchg(memcg->vmstats_percpu->stat[i], 0);
			if (x)
				for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
					atomic_long_add(x, &memcg->vmstats[i]);

			if (i >= NR_VM_NODE_STAT_ITEMS)
				continue;

			for_each_node(nid) {
				struct mem_cgroup_per_node *pn;

				pn = mem_cgroup_nodeinfo(memcg, nid);
				x = this_cpu_xchg(pn->lruvec_stat_cpu->count[i], 0);
				if (x)
					do {
						atomic_long_add(x, &pn->lruvec_stat[i]);
					} while ((pn = parent_nodeinfo(pn, nid)));
			}
		}

		for (i = 0; i < NR_VM_EVENT_ITEMS; i++) {
			long x;

			x = this_cpu_xchg(memcg->vmstats_percpu->events[i], 0);
			if (x)
				for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
					atomic_long_add(x, &memcg->vmevents[i]);
		}
	}

	return 0;
}

static void reclaim_wmark(struct mem_cgroup *memcg)
{
	long nr_pages;
	struct mem_cgroup *iter;
	u64 start, duration;
	unsigned long pflags;

	if (is_wmark_ok(memcg, false))
		return;

	nr_pages = page_counter_read(&memcg->memory) -
		   memcg->memory.wmark_low;
	if (nr_pages <= 0)
		return;

	nr_pages = max(SWAP_CLUSTER_MAX, (unsigned long)nr_pages);

	/*
	 * Typically, we would like to record the actual cpu% of reclaim_wmark
	 * work, excluding any sleep/resched time.  However, currently we just
	 * simply record the whole duration of reclaim_wmark work for the
	 * overhead-accuracy trade-off.
	 */
	start = ktime_get_ns();
	psi_memstall_enter(&pflags);
	try_to_free_mem_cgroup_pages(memcg, nr_pages, GFP_KERNEL, true);
	psi_memstall_leave(&pflags);
	duration = ktime_get_ns() - start;

	css_get(&memcg->css);
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter))
		this_cpu_add(iter->exstat_cpu->item[MEMCG_WMARK_RECLAIM],
			     duration);
	css_put(&memcg->css);
}

static void wmark_work_func(struct work_struct *work)
{
	struct mem_cgroup *memcg;

	memcg = container_of(work, struct mem_cgroup, wmark_work);

	current->flags |= PF_SWAPWRITE | PF_MEMALLOC | PF_KSWAPD;
	reclaim_wmark(memcg);
	current->flags &= ~(PF_SWAPWRITE | PF_MEMALLOC | PF_KSWAPD);
}

static unsigned long reclaim_high(struct mem_cgroup *memcg,
				  unsigned int nr_pages,
				  gfp_t gfp_mask)
{
	unsigned long nr_reclaimed = 0;

	do {
		unsigned long pflags;

		if (page_counter_read(&memcg->memory) <=
		    READ_ONCE(memcg->memory.high))
			continue;

		memcg_memory_event(memcg, MEMCG_HIGH);

		psi_memstall_enter(&pflags);
		nr_reclaimed += try_to_free_mem_cgroup_pages(memcg, nr_pages,
							     gfp_mask, true);
		psi_memstall_leave(&pflags);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return nr_reclaimed;
}

static void high_work_func(struct work_struct *work)
{
	struct mem_cgroup *memcg;

	memcg = container_of(work, struct mem_cgroup, high_work);
	reclaim_high(memcg, MEMCG_CHARGE_BATCH, GFP_KERNEL);
}

/*
 * Clamp the maximum sleep time per allocation batch to 2 seconds. This is
 * enough to still cause a significant slowdown in most cases, while still
 * allowing diagnostics and tracing to proceed without becoming stuck.
 */
#define MEMCG_MAX_HIGH_DELAY_JIFFIES (2UL*HZ)

/*
 * When calculating the delay, we use these either side of the exponentiation to
 * maintain precision and scale to a reasonable number of jiffies (see the table
 * below.
 *
 * - MEMCG_DELAY_PRECISION_SHIFT: Extra precision bits while translating the
 *   overage ratio to a delay.
 * - MEMCG_DELAY_SCALING_SHIFT: The number of bits to scale down down the
 *   proposed penalty in order to reduce to a reasonable number of jiffies, and
 *   to produce a reasonable delay curve.
 *
 * MEMCG_DELAY_SCALING_SHIFT just happens to be a number that produces a
 * reasonable delay curve compared to precision-adjusted overage, not
 * penalising heavily at first, but still making sure that growth beyond the
 * limit penalises misbehaviour cgroups by slowing them down exponentially. For
 * example, with a high of 100 megabytes:
 *
 *  +-------+------------------------+
 *  | usage | time to allocate in ms |
 *  +-------+------------------------+
 *  | 100M  |                      0 |
 *  | 101M  |                      6 |
 *  | 102M  |                     25 |
 *  | 103M  |                     57 |
 *  | 104M  |                    102 |
 *  | 105M  |                    159 |
 *  | 106M  |                    230 |
 *  | 107M  |                    313 |
 *  | 108M  |                    409 |
 *  | 109M  |                    518 |
 *  | 110M  |                    639 |
 *  | 111M  |                    774 |
 *  | 112M  |                    921 |
 *  | 113M  |                   1081 |
 *  | 114M  |                   1254 |
 *  | 115M  |                   1439 |
 *  | 116M  |                   1638 |
 *  | 117M  |                   1849 |
 *  | 118M  |                   2000 |
 *  | 119M  |                   2000 |
 *  | 120M  |                   2000 |
 *  +-------+------------------------+
 */
 #define MEMCG_DELAY_PRECISION_SHIFT 20
 #define MEMCG_DELAY_SCALING_SHIFT 14

static inline unsigned long mem_cgroup_v1_swap_usage(struct mem_cgroup *memcg)
{
	unsigned long swap_usage = 0;

	if (do_memsw_account()) {
		unsigned long memsw_usage = page_counter_read(&memcg->memsw);
		unsigned long memcg_usage = page_counter_read(&memcg->memory);

		if (memsw_usage > memcg_usage)
			swap_usage = memsw_usage - memcg_usage;
	}

	return swap_usage;
}

static u64 calculate_overage(unsigned long usage, unsigned long high)
{
	u64 overage;

	if (usage <= high)
		return 0;
	/*
	 * Prevent division by 0 in overage calculation by acting as if
	 * it was a threshold of 1 page
	 */
	high = max(high, 1UL);

	overage = usage - high;
	overage <<= MEMCG_DELAY_PRECISION_SHIFT;
	return div64_u64(overage, high);
}

static u64 mem_find_max_overage(struct mem_cgroup *memcg)
{
	u64 overage, max_overage = 0;

	do {
		overage = calculate_overage(page_counter_read(&memcg->memory),
					    READ_ONCE(memcg->memory.high));
		max_overage = max(overage, max_overage);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return max_overage;
}

static u64 swap_find_max_overage(struct mem_cgroup *memcg)
{
	u64 overage, max_overage = 0;

	do {
		if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
			overage = calculate_overage(page_counter_read(&memcg->swap),
						READ_ONCE(memcg->swap.high));
		else {
			unsigned long swap_usage;

			swap_usage = mem_cgroup_v1_swap_usage(memcg);
			overage = calculate_overage(swap_usage,
						READ_ONCE(memcg->memsw.high));
		}

		if (overage)
			memcg_memory_event(memcg, MEMCG_SWAP_HIGH);
		max_overage = max(overage, max_overage);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return max_overage;
}

/*
 * Get the number of jiffies that we should penalise a mischievous cgroup which
 * is exceeding its memory.high by checking both it and its ancestors.
 */
static unsigned long calculate_high_delay(struct mem_cgroup *memcg,
					  unsigned int nr_pages,
					  u64 max_overage)
{
	unsigned long penalty_jiffies;

	if (!max_overage)
		return 0;

	/*
	 * We use overage compared to memory.high to calculate the number of
	 * jiffies to sleep (penalty_jiffies). Ideally this value should be
	 * fairly lenient on small overages, and increasingly harsh when the
	 * memcg in question makes it clear that it has no intention of stopping
	 * its crazy behaviour, so we exponentially increase the delay based on
	 * overage amount.
	 */
	penalty_jiffies = max_overage * max_overage * HZ;
	penalty_jiffies >>= MEMCG_DELAY_PRECISION_SHIFT;
	penalty_jiffies >>= MEMCG_DELAY_SCALING_SHIFT;

	/*
	 * Factor in the task's own contribution to the overage, such that four
	 * N-sized allocations are throttled approximately the same as one
	 * 4N-sized allocation.
	 *
	 * MEMCG_CHARGE_BATCH pages is nominal, so work out how much smaller or
	 * larger the current charge patch is than that.
	 */
	return penalty_jiffies * nr_pages / MEMCG_CHARGE_BATCH;
}

/*
 * Scheduled by try_charge() to be executed from the userland return path
 * and reclaims memory over the high limit.
 */
void mem_cgroup_handle_over_high(void)
{
	unsigned long penalty_jiffies;
	unsigned long pflags;
	unsigned long nr_reclaimed;
	unsigned int nr_pages = current->memcg_nr_pages_over_high;
	int nr_retries = MAX_RECLAIM_RETRIES;
	struct mem_cgroup *memcg;
	u64 start;
	bool in_retry = false;

	if (likely(!nr_pages))
		return;

	memcg = get_mem_cgroup_from_mm(current->mm);
	memcg_lat_stat_start(&start);
	current->memcg_nr_pages_over_high = 0;

retry_reclaim:
	/*
	 * The allocating task should reclaim at least the batch size, but for
	 * subsequent retries we only want to do what's necessary to prevent oom
	 * or breaching resource isolation.
	 *
	 * This is distinct from memory.max or page allocator behaviour because
	 * memory.high is currently batched, whereas memory.max and the page
	 * allocator run every time an allocation is made.
	 */
	nr_reclaimed = reclaim_high(memcg,
				    in_retry ? SWAP_CLUSTER_MAX : nr_pages,
				    GFP_KERNEL);

	/*
	 * memory.high is breached and reclaim is unable to keep up. Throttle
	 * allocators proactively to slow down excessive growth.
	 */
	penalty_jiffies = calculate_high_delay(memcg, nr_pages,
					       mem_find_max_overage(memcg));

	penalty_jiffies += calculate_high_delay(memcg, nr_pages,
						swap_find_max_overage(memcg));

	/*
	 * Clamp the max delay per usermode return so as to still keep the
	 * application moving forwards and also permit diagnostics, albeit
	 * extremely slowly.
	 */
	penalty_jiffies = min(penalty_jiffies, MEMCG_MAX_HIGH_DELAY_JIFFIES);
	penalty_jiffies += sysctl_penalty_extra_delay_jiffies;

	/*
	 * Don't sleep if the amount of jiffies this memcg owes us is so low
	 * that it's not even worth doing, in an attempt to be nice to those who
	 * go only a small amount over their memory.high value and maybe haven't
	 * been aggressively reclaimed enough yet.
	 */
	if (penalty_jiffies <= HZ / 100)
		goto out;

	/*
	 * If reclaim is making forward progress but we're still over
	 * memory.high, we want to encourage that rather than doing allocator
	 * throttling.
	 */
	if (nr_reclaimed || nr_retries--) {
		in_retry = true;
		goto retry_reclaim;
	}

	/*
	 * If we exit early, we're guaranteed to die (since
	 * schedule_timeout_killable sets TASK_KILLABLE). This means we don't
	 * need to account for any ill-begotten jiffies to pay them off later.
	 */
	psi_memstall_enter(&pflags);
	schedule_timeout_killable(penalty_jiffies);
	psi_memstall_leave(&pflags);

out:
	memcg_lat_stat_end(MEM_LAT_MEMCG_DIRECT_RECLAIM, start);
	css_put(&memcg->css);
}

static int try_charge(struct mem_cgroup *memcg, gfp_t gfp_mask,
		      unsigned int nr_pages)
{
	unsigned int batch = max(MEMCG_CHARGE_BATCH, nr_pages);
	int nr_retries = MAX_RECLAIM_RETRIES;
	struct mem_cgroup *mem_over_limit;
	struct page_counter *counter;
	enum oom_status oom_status;
	unsigned long nr_reclaimed;
	bool may_swap = true;
	bool drained = false;
	bool oomed = false;
	unsigned long pflags;
	u64 start;

	if (mem_cgroup_is_root(memcg))
		return 0;
retry:
	if (consume_stock(memcg, nr_pages))
		return 0;

	if (!do_memsw_account() ||
	    page_counter_try_charge(&memcg->memsw, batch, &counter)) {
		if (page_counter_try_charge(&memcg->memory, batch, &counter))
			goto done_restock;
		if (do_memsw_account())
			page_counter_uncharge(&memcg->memsw, batch);
		mem_over_limit = mem_cgroup_from_counter(counter, memory);
	} else {
		mem_over_limit = mem_cgroup_from_counter(counter, memsw);
		may_swap = false;
	}

	if (batch > nr_pages) {
		batch = nr_pages;
		goto retry;
	}

	/*
	 * Memcg doesn't have a dedicated reserve for atomic
	 * allocations. But like the global atomic pool, we need to
	 * put the burden of reclaim on regular allocation requests
	 * and let these go through as privileged allocations.
	 */
	if (gfp_mask & __GFP_ATOMIC)
		goto force;

	/*
	 * Unlike in global OOM situations, memcg is not in a physical
	 * memory shortage.  Allow dying and OOM-killed tasks to
	 * bypass the last charges so that they can exit quickly and
	 * free their memory.
	 */
	if (unlikely(should_force_charge()))
		goto force;

	/*
	 * Prevent unbounded recursion when reclaim operations need to
	 * allocate memory. This might exceed the limits temporarily,
	 * but we prefer facilitating memory reclaim and getting back
	 * under the limit over triggering OOM kills in these cases.
	 */
	if (unlikely(current->flags & PF_MEMALLOC))
		goto force;

	if (unlikely(task_in_memcg_oom(current)))
		goto nomem;

	if (!gfpflags_allow_blocking(gfp_mask))
		goto nomem;

	memcg_memory_event(mem_over_limit, MEMCG_MAX);

	memcg_lat_stat_start(&start);
	psi_memstall_enter(&pflags);
	nr_reclaimed = try_to_free_mem_cgroup_pages(mem_over_limit, nr_pages,
						    gfp_mask, may_swap);
	psi_memstall_leave(&pflags);
	memcg_lat_stat_end(MEM_LAT_MEMCG_DIRECT_RECLAIM, start);

	if (mem_cgroup_margin(mem_over_limit) >= nr_pages)
		goto retry;

	if (!drained) {
		drain_all_stock(mem_over_limit);
		drained = true;
		goto retry;
	}

	if (gfp_mask & __GFP_NORETRY)
		goto nomem;
	/*
	 * Even though the limit is exceeded at this point, reclaim
	 * may have been able to free some pages.  Retry the charge
	 * before killing the task.
	 *
	 * Only for regular pages, though: huge pages are rather
	 * unlikely to succeed so close to the limit, and we fall back
	 * to regular pages anyway in case of failure.
	 */
	if (nr_reclaimed && nr_pages <= (1 << PAGE_ALLOC_COSTLY_ORDER))
		goto retry;
	/*
	 * At task move, charge accounts can be doubly counted. So, it's
	 * better to wait until the end of task_move if something is going on.
	 */
	if (mem_cgroup_wait_acct_move(mem_over_limit))
		goto retry;

	if (nr_retries--)
		goto retry;

	if (gfp_mask & __GFP_RETRY_MAYFAIL && oomed)
		goto nomem;

	if (gfp_mask & __GFP_NOFAIL)
		goto force;

	if (fatal_signal_pending(current))
		goto force;

	/*
	 * keep retrying as long as the memcg oom killer is able to make
	 * a forward progress or bypass the charge if the oom killer
	 * couldn't make any progress.
	 */
	oom_status = mem_cgroup_oom(mem_over_limit, gfp_mask,
		       get_order(nr_pages * PAGE_SIZE));
	switch (oom_status) {
	case OOM_SUCCESS:
		nr_retries = MAX_RECLAIM_RETRIES;
		oomed = true;
		/*
		 * With !PREEMPT kernel if the memcg doesn't have reclaimable
		 * memory, the reclaim retry and oom logic may block scheduling
		 * indefinitely.
		 */
		cond_resched();
		goto retry;
	case OOM_FAILED:
		goto force;
	default:
		goto nomem;
	}
nomem:
	if (!(gfp_mask & __GFP_NOFAIL))
		return -ENOMEM;
force:
	/*
	 * The allocation either can't fail or will lead to more memory
	 * being freed very soon.  Allow memory usage go over the limit
	 * temporarily by force charging it.
	 */
	page_counter_charge(&memcg->memory, nr_pages);
	if (do_memsw_account())
		page_counter_charge(&memcg->memsw, nr_pages);
	css_get_many(&memcg->css, nr_pages);

	return 0;

done_restock:
	css_get_many(&memcg->css, batch);
	if (batch > nr_pages)
		refill_stock(memcg, batch - nr_pages);

	/*
	 * If the hierarchy is above the normal consumption range, schedule
	 * reclaim on returning to userland.  We can perform reclaim here
	 * if __GFP_RECLAIM but let's always punt for simplicity and so that
	 * GFP_KERNEL can consistently be used during reclaim.  @memcg is
	 * not recorded as it most likely matches current's and won't
	 * change in the meantime.  As high limit is checked again before
	 * reclaim, the cost of mismatch is negligible.
	 */
	do {
		bool mem_high, swap_high;

		if (!is_wmark_ok(memcg, true)) {
			queue_work(memcg_wmark_wq, &memcg->wmark_work);
			break;
		}

		mem_high = page_counter_read(&memcg->memory) >
			READ_ONCE(memcg->memory.high);
		if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
			swap_high = page_counter_read(&memcg->swap) >
						READ_ONCE(memcg->swap.high);
		else {
			unsigned long swap_usage;

			swap_usage = mem_cgroup_v1_swap_usage(memcg);
			swap_high = swap_usage > READ_ONCE(memcg->memsw.high);
		}

		/* Don't bother a random interrupted task */
		if (in_interrupt()) {
			if (mem_high) {
				schedule_work(&memcg->high_work);
				break;
			}
			continue;
		}

		if (mem_high || swap_high) {
			/*
			 * The allocating tasks in this cgroup will need to do
			 * reclaim or be throttled to prevent further growth
			 * of the memory or swap footprints.
			 *
			 * Target some best-effort fairness between the tasks,
			 * and distribute reclaim work and delay penalties
			 * based on how much each task is actually allocating.
			 */
			current->memcg_nr_pages_over_high += batch;
			set_notify_resume(current);
			break;
		}
	} while ((memcg = parent_mem_cgroup(memcg)));

	return 0;
}

#if defined(CONFIG_MEMCG_KMEM) || defined(CONFIG_MMU)
static void cancel_charge(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	if (mem_cgroup_is_root(memcg))
		return;

	page_counter_uncharge(&memcg->memory, nr_pages);
	if (do_memsw_account())
		page_counter_uncharge(&memcg->memsw, nr_pages);

	css_put_many(&memcg->css, nr_pages);
}
#endif

static void commit_charge(struct page *page, struct mem_cgroup *memcg)
{
	VM_BUG_ON_PAGE(page->mem_cgroup, page);
	/*
	 * Any of the following ensures page->mem_cgroup stability:
	 *
	 * - the page lock
	 * - LRU isolation
	 * - lock_page_memcg()
	 * - exclusive reference
	 */
	page->mem_cgroup = memcg;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	add_hugepage_to_queue(page);
#endif
}

#ifdef CONFIG_MEMCG_KMEM
static int memcg_alloc_cache_id(void)
{
	int id, size;
	int err;

	id = ida_simple_get(&memcg_cache_ida,
			    0, MEMCG_CACHES_MAX_SIZE, GFP_KERNEL);
	if (id < 0)
		return id;

	if (id < memcg_nr_cache_ids)
		return id;

	/*
	 * There's no space for the new id in memcg_caches arrays,
	 * so we have to grow them.
	 */
	down_write(&memcg_cache_ids_sem);

	size = 2 * (id + 1);
	if (size < MEMCG_CACHES_MIN_SIZE)
		size = MEMCG_CACHES_MIN_SIZE;
	else if (size > MEMCG_CACHES_MAX_SIZE)
		size = MEMCG_CACHES_MAX_SIZE;

	err = memcg_update_all_caches(size);
	if (!err)
		err = memcg_update_all_list_lrus(size);
	if (!err)
		memcg_nr_cache_ids = size;

	up_write(&memcg_cache_ids_sem);

	if (err) {
		ida_simple_remove(&memcg_cache_ida, id);
		return err;
	}
	return id;
}

static void memcg_free_cache_id(int id)
{
	ida_simple_remove(&memcg_cache_ida, id);
}

struct memcg_kmem_cache_create_work {
	struct mem_cgroup *memcg;
	struct kmem_cache *cachep;
	struct work_struct work;
};

static void memcg_kmem_cache_create_func(struct work_struct *w)
{
	struct memcg_kmem_cache_create_work *cw =
		container_of(w, struct memcg_kmem_cache_create_work, work);
	struct mem_cgroup *memcg = cw->memcg;
	struct kmem_cache *cachep = cw->cachep;

	memcg_create_kmem_cache(memcg, cachep);

	css_put(&memcg->css);
	kfree(cw);
}

/*
 * Enqueue the creation of a per-memcg kmem_cache.
 */
static void __memcg_schedule_kmem_cache_create(struct mem_cgroup *memcg,
					       struct kmem_cache *cachep)
{
	struct memcg_kmem_cache_create_work *cw;

	cw = kmalloc(sizeof(*cw), GFP_NOWAIT | __GFP_NOWARN);
	if (!cw)
		return;

	css_get(&memcg->css);

	cw->memcg = memcg;
	cw->cachep = cachep;
	INIT_WORK(&cw->work, memcg_kmem_cache_create_func);

	queue_work(memcg_kmem_cache_wq, &cw->work);
}

static void memcg_schedule_kmem_cache_create(struct mem_cgroup *memcg,
					     struct kmem_cache *cachep)
{
	/*
	 * We need to stop accounting when we kmalloc, because if the
	 * corresponding kmalloc cache is not yet created, the first allocation
	 * in __memcg_schedule_kmem_cache_create will recurse.
	 *
	 * However, it is better to enclose the whole function. Depending on
	 * the debugging options enabled, INIT_WORK(), for instance, can
	 * trigger an allocation. This too, will make us recurse. Because at
	 * this point we can't allow ourselves back into memcg_kmem_get_cache,
	 * the safest choice is to do it like this, wrapping the whole function.
	 */
	current->memcg_kmem_skip_account = 1;
	__memcg_schedule_kmem_cache_create(memcg, cachep);
	current->memcg_kmem_skip_account = 0;
}

static inline bool memcg_kmem_bypass(void)
{
	if (in_interrupt() || !current->mm || (current->flags & PF_KTHREAD))
		return true;
	return false;
}

/**
 * memcg_kmem_get_cache: select the correct per-memcg cache for allocation
 * @cachep: the original global kmem cache
 *
 * Return the kmem_cache we're supposed to use for a slab allocation.
 * We try to use the current memcg's version of the cache.
 *
 * If the cache does not exist yet, if we are the first user of it, we
 * create it asynchronously in a workqueue and let the current allocation
 * go through with the original cache.
 *
 * This function takes a reference to the cache it returns to assure it
 * won't get destroyed while we are working with it. Once the caller is
 * done with it, memcg_kmem_put_cache() must be called to release the
 * reference.
 */
struct kmem_cache *memcg_kmem_get_cache(struct kmem_cache *cachep)
{
	struct mem_cgroup *memcg;
	struct kmem_cache *memcg_cachep;
	int kmemcg_id;

	VM_BUG_ON(!is_root_cache(cachep));

	if (memcg_kmem_bypass())
		return cachep;

	if (current->memcg_kmem_skip_account)
		return cachep;

	memcg = get_mem_cgroup_from_current();
	kmemcg_id = READ_ONCE(memcg->kmemcg_id);
	if (kmemcg_id < 0)
		goto out;

	memcg_cachep = cache_from_memcg_idx(cachep, kmemcg_id);
	if (likely(memcg_cachep))
		return memcg_cachep;

	/*
	 * If we are in a safe context (can wait, and not in interrupt
	 * context), we could be be predictable and return right away.
	 * This would guarantee that the allocation being performed
	 * already belongs in the new cache.
	 *
	 * However, there are some clashes that can arrive from locking.
	 * For instance, because we acquire the slab_mutex while doing
	 * memcg_create_kmem_cache, this means no further allocation
	 * could happen with the slab_mutex held. So it's better to
	 * defer everything.
	 */
	memcg_schedule_kmem_cache_create(memcg, cachep);
out:
	css_put(&memcg->css);
	return cachep;
}

/**
 * memcg_kmem_put_cache: drop reference taken by memcg_kmem_get_cache
 * @cachep: the cache returned by memcg_kmem_get_cache
 */
void memcg_kmem_put_cache(struct kmem_cache *cachep)
{
	if (!is_root_cache(cachep))
		css_put(&cachep->memcg_params.memcg->css);
}

/**
 * memcg_kmem_charge_memcg: charge a kmem page
 * @page: page to charge
 * @gfp: reclaim mode
 * @order: allocation order
 * @memcg: memory cgroup to charge
 *
 * Returns 0 on success, an error code on failure.
 */
int memcg_kmem_charge_memcg(struct page *page, gfp_t gfp, int order,
			    struct mem_cgroup *memcg)
{
	unsigned int nr_pages = 1 << order;
	struct page_counter *counter;
	int ret;

	ret = try_charge(memcg, gfp, nr_pages);
	if (ret)
		return ret;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) &&
	    !page_counter_try_charge(&memcg->kmem, nr_pages, &counter)) {

		/*
		 * Enforce __GFP_NOFAIL allocation because callers are not
		 * prepared to see failures and likely do not have any failure
		 * handling code.
		 */
		if (gfp & __GFP_NOFAIL) {
			page_counter_charge(&memcg->kmem, nr_pages);
			return 0;
		}
		cancel_charge(memcg, nr_pages);
		return -ENOMEM;
	}

	page->mem_cgroup = memcg;

	return 0;
}

/**
 * memcg_kmem_charge: charge a kmem page to the current memory cgroup
 * @page: page to charge
 * @gfp: reclaim mode
 * @order: allocation order
 *
 * Returns 0 on success, an error code on failure.
 */
int memcg_kmem_charge(struct page *page, gfp_t gfp, int order)
{
	struct mem_cgroup *memcg;
	int ret = 0;

	if (mem_cgroup_disabled() || memcg_kmem_bypass())
		return 0;

	memcg = get_mem_cgroup_from_current();
	if (!mem_cgroup_is_root(memcg)) {
		ret = memcg_kmem_charge_memcg(page, gfp, order, memcg);
		if (!ret)
			__SetPageKmemcg(page);
	}
	css_put(&memcg->css);
	return ret;
}
/**
 * memcg_kmem_uncharge: uncharge a kmem page
 * @page: page to uncharge
 * @order: allocation order
 */
void memcg_kmem_uncharge(struct page *page, int order)
{
	struct mem_cgroup *memcg = page->mem_cgroup;
	unsigned int nr_pages = 1 << order;

	if (!memcg)
		return;

	VM_BUG_ON_PAGE(mem_cgroup_is_root(memcg), page);

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		page_counter_uncharge(&memcg->kmem, nr_pages);

	page_counter_uncharge(&memcg->memory, nr_pages);
	if (do_memsw_account())
		page_counter_uncharge(&memcg->memsw, nr_pages);

	page->mem_cgroup = NULL;

	/* slab pages do not have PageKmemcg flag set */
	if (PageKmemcg(page))
		__ClearPageKmemcg(page);

	css_put_many(&memcg->css, nr_pages);
}
#endif /* CONFIG_MEMCG_KMEM */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE

/*
 * Because tail pages are not marked as "used", set it. We're under
 * zone_lru_lock and migration entries setup in all page mappings.
 */
void mem_cgroup_split_huge_fixup(struct page *head)
{
	int i;

	if (mem_cgroup_disabled())
		return;

	for (i = 1; i < HPAGE_PMD_NR; i++)
		head[i].mem_cgroup = head->mem_cgroup;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_MEMCG_SWAP
/**
 * mem_cgroup_move_swap_account - move swap charge and swap_cgroup's record.
 * @entry: swap entry to be moved
 * @from:  mem_cgroup which the entry is moved from
 * @to:  mem_cgroup which the entry is moved to
 *
 * It succeeds only when the swap_cgroup's record for this entry is the same
 * as the mem_cgroup's id of @from.
 *
 * Returns 0 on success, -EINVAL on failure.
 *
 * The caller must have charged to @to, IOW, called page_counter_charge() about
 * both res and memsw, and called css_get().
 */
static int mem_cgroup_move_swap_account(swp_entry_t entry,
				struct mem_cgroup *from, struct mem_cgroup *to)
{
	unsigned short old_id, new_id;

	old_id = mem_cgroup_id(from);
	new_id = mem_cgroup_id(to);

	if (swap_cgroup_cmpxchg(entry, old_id, new_id) == old_id) {
		mod_memcg_state(from, MEMCG_SWAP, -1);
		mod_memcg_state(to, MEMCG_SWAP, 1);
		return 0;
	}
	return -EINVAL;
}
#else
static inline int mem_cgroup_move_swap_account(swp_entry_t entry,
				struct mem_cgroup *from, struct mem_cgroup *to)
{
	return -EINVAL;
}
#endif

static void setup_memcg_wmark(struct mem_cgroup *memcg)
{
	unsigned long high_wmark;
	unsigned long low_wmark;
	unsigned long max = memcg->memory.high > memcg->memory.max ?
			    memcg->memory.max : memcg->memory.high;
	unsigned int wmark_ratio = memcg->wmark_ratio;
	unsigned int wmark_scale_factor = memcg->wmark_scale_factor;
	unsigned long gap;

	if (wmark_ratio) {
		high_wmark = (max * wmark_ratio) / 100;

		/*
		 * Set the memcg watermark distance according to the
		 * scale factor in proportion to max limit.
		 */
		gap = mult_frac(max, wmark_scale_factor, 10000);

		low_wmark = high_wmark - gap;

		page_counter_set_wmark_low(&memcg->memory, low_wmark);
		page_counter_set_wmark_high(&memcg->memory, high_wmark);
	} else {
		page_counter_set_wmark_low(&memcg->memory, PAGE_COUNTER_MAX);
		page_counter_set_wmark_high(&memcg->memory, PAGE_COUNTER_MAX);
	}
}

static DEFINE_MUTEX(memcg_max_mutex);

static int mem_cgroup_resize_max(struct mem_cgroup *memcg,
				 unsigned long max, bool memsw)
{
	bool enlarge = false;
	bool drained = false;
	int ret;
	bool limits_invariant;
	struct page_counter *counter = memsw ? &memcg->memsw : &memcg->memory;

	do {
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		mutex_lock(&memcg_max_mutex);
		/*
		 * Make sure that the new limit (memsw or memory limit) doesn't
		 * break our basic invariant rule memory.max <= memsw.max.
		 */
		limits_invariant = memsw ? max >= READ_ONCE(memcg->memory.max) :
					   max <= memcg->memsw.max;
		if (!limits_invariant) {
			mutex_unlock(&memcg_max_mutex);
			ret = -EINVAL;
			break;
		}
		if (max > counter->max)
			enlarge = true;
		ret = page_counter_set_max(counter, max);
		mutex_unlock(&memcg_max_mutex);

		if (!ret)
			break;

		if (!drained) {
			drain_all_stock(memcg);
			drained = true;
			continue;
		}

		if (!try_to_free_mem_cgroup_pages(memcg, 1,
					GFP_KERNEL, !memsw)) {
			ret = -EBUSY;
			break;
		}
	} while (true);

	if (!ret) {
		setup_memcg_wmark(memcg);

		if (!is_wmark_ok(memcg, true))
			queue_work(memcg_wmark_wq, &memcg->wmark_work);

		if (enlarge)
			memcg_oom_recover(memcg);
	}

	return ret;
}

unsigned long mem_cgroup_soft_limit_reclaim(pg_data_t *pgdat, int order,
					    gfp_t gfp_mask,
					    unsigned long *total_scanned)
{
	unsigned long nr_reclaimed = 0;
	struct mem_cgroup_per_node *mz, *next_mz = NULL;
	unsigned long reclaimed;
	int loop = 0;
	struct mem_cgroup_tree_per_node *mctz;
	unsigned long excess;
	unsigned long nr_scanned;

	if (order > 0)
		return 0;

	mctz = soft_limit_tree_node(pgdat->node_id);

	/*
	 * Do not even bother to check the largest node if the root
	 * is empty. Do it lockless to prevent lock bouncing. Races
	 * are acceptable as soft limit is best effort anyway.
	 */
	if (!mctz || RB_EMPTY_ROOT(&mctz->rb_root))
		return 0;

	/*
	 * This loop can run a while, specially if mem_cgroup's continuously
	 * keep exceeding their soft limit and putting the system under
	 * pressure
	 */
	do {
		if (next_mz)
			mz = next_mz;
		else
			mz = mem_cgroup_largest_soft_limit_node(mctz);
		if (!mz)
			break;

		nr_scanned = 0;
		reclaimed = mem_cgroup_soft_reclaim(mz->memcg, pgdat,
						    gfp_mask, &nr_scanned);
		nr_reclaimed += reclaimed;
		*total_scanned += nr_scanned;
		spin_lock_irq(&mctz->lock);
		__mem_cgroup_remove_exceeded(mz, mctz);

		/*
		 * If we failed to reclaim anything from this memory cgroup
		 * it is time to move on to the next cgroup
		 */
		next_mz = NULL;
		if (!reclaimed)
			next_mz = __mem_cgroup_largest_soft_limit_node(mctz);

		excess = soft_limit_excess(mz->memcg);
		/*
		 * One school of thought says that we should not add
		 * back the node to the tree if reclaim returns 0.
		 * But our reclaim could return 0, simply because due
		 * to priority we are exposing a smaller subset of
		 * memory to reclaim from. Consider this as a longer
		 * term TODO.
		 */
		/* If excess == 0, no tree ops */
		__mem_cgroup_insert_exceeded(mz, mctz, excess);
		spin_unlock_irq(&mctz->lock);
		css_put(&mz->memcg->css);
		loop++;
		/*
		 * Could not reclaim anything and there are no more
		 * mem cgroups to try or we seem to be looping without
		 * reclaiming anything.
		 */
		if (!nr_reclaimed &&
			(next_mz == NULL ||
			loop > MEM_CGROUP_MAX_SOFT_LIMIT_RECLAIM_LOOPS))
			break;
	} while (!nr_reclaimed);
	if (next_mz)
		css_put(&next_mz->memcg->css);
	return nr_reclaimed;
}

/*
 * Test whether @memcg has children, dead or alive.  Note that this
 * function doesn't care whether @memcg has use_hierarchy enabled and
 * returns %true if there are child csses according to the cgroup
 * hierarchy.  Testing use_hierarchy is the caller's responsiblity.
 */
static inline bool memcg_has_children(struct mem_cgroup *memcg)
{
	bool ret;

	rcu_read_lock();
	ret = css_next_child(NULL, &memcg->css);
	rcu_read_unlock();
	return ret;
}

/*
 * Reclaims as many pages from the given memcg as possible.
 *
 * Caller is responsible for holding css reference for memcg.
 */
static int mem_cgroup_force_empty(struct mem_cgroup *memcg)
{
	int nr_retries = MAX_RECLAIM_RETRIES;

	/* we call try-to-free pages for make this cgroup empty */
	lru_add_drain_all();

	drain_all_stock(memcg);

	/* try to free all pages in this cgroup */
	while (nr_retries && page_counter_read(&memcg->memory)) {
		int progress;

		if (signal_pending(current))
			return -EINTR;

		progress = try_to_free_mem_cgroup_pages(memcg, 1,
							GFP_KERNEL, true);
		if (!progress) {
			nr_retries--;
			/* maybe some writeback is necessary */
			congestion_wait(BLK_RW_ASYNC, HZ/10);
		}

	}

	return 0;
}

static ssize_t mem_cgroup_force_empty_write(struct kernfs_open_file *of,
					    char *buf, size_t nbytes,
					    loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));

	if (mem_cgroup_is_root(memcg))
		return -EINVAL;
	return mem_cgroup_force_empty(memcg) ?: nbytes;
}

static u64 mem_cgroup_hierarchy_read(struct cgroup_subsys_state *css,
				     struct cftype *cft)
{
	return mem_cgroup_from_css(css)->use_hierarchy;
}

static int mem_cgroup_hierarchy_write(struct cgroup_subsys_state *css,
				      struct cftype *cft, u64 val)
{
	int retval = 0;
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mem_cgroup *parent_memcg = mem_cgroup_from_css(memcg->css.parent);

	if (memcg->use_hierarchy == val)
		return 0;

	/*
	 * If parent's use_hierarchy is set, we can't make any modifications
	 * in the child subtrees. If it is unset, then the change can
	 * occur, provided the current cgroup has no children.
	 *
	 * For the root cgroup, parent_mem is NULL, we allow value to be
	 * set if there are no children.
	 */
	if ((!parent_memcg || !parent_memcg->use_hierarchy) &&
				(val == 1 || val == 0)) {
		if (!memcg_has_children(memcg))
			memcg->use_hierarchy = val;
		else
			retval = -EBUSY;
	} else
		retval = -EINVAL;

	return retval;
}

static u64 mem_cgroup_priority_oom_read(struct cgroup_subsys_state *css,
					struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return memcg->use_priority_oom;
}

static int mem_cgroup_priority_oom_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	if (val > 1)
		return -EINVAL;

	memcg->use_priority_oom = val;

	return 0;
}

static unsigned long mem_cgroup_usage(struct mem_cgroup *memcg, bool swap)
{
	unsigned long val;

	if (mem_cgroup_is_root(memcg)) {
		val = memcg_page_state(memcg, NR_FILE_PAGES) +
			memcg_page_state(memcg, NR_ANON_MAPPED);
		if (swap)
			val += memcg_page_state(memcg, MEMCG_SWAP);
	} else {
		if (!swap)
			val = page_counter_read(&memcg->memory);
		else
			val = page_counter_read(&memcg->memsw);
	}
	return val;
}

enum {
	RES_USAGE,
	RES_LIMIT,
	RES_MAX_USAGE,
	RES_FAILCNT,
	RES_SOFT_LIMIT,
	WMARK_HIGH_LIMIT,
	WMARK_LOW_LIMIT,
};

static u64 mem_cgroup_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct page_counter *counter;

	switch (MEMFILE_TYPE(cft->private)) {
	case _MEM:
		counter = &memcg->memory;
		break;
	case _MEMSWAP:
		counter = &memcg->memsw;
		break;
	case _KMEM:
		counter = &memcg->kmem;
		break;
	case _TCP:
		counter = &memcg->tcpmem;
		break;
	default:
		BUG();
	}

	switch (MEMFILE_ATTR(cft->private)) {
	case RES_USAGE:
		if (counter == &memcg->memory)
			return (u64)mem_cgroup_usage(memcg, false) * PAGE_SIZE;
		if (counter == &memcg->memsw)
			return (u64)mem_cgroup_usage(memcg, true) * PAGE_SIZE;
		return (u64)page_counter_read(counter) * PAGE_SIZE;
	case RES_LIMIT:
		return (u64)counter->max * PAGE_SIZE;
	case RES_MAX_USAGE:
		return (u64)counter->watermark * PAGE_SIZE;
	case RES_FAILCNT:
		return counter->failcnt;
	case RES_SOFT_LIMIT:
		return (u64)memcg->soft_limit * PAGE_SIZE;
	case WMARK_HIGH_LIMIT:
		return (u64)counter->wmark_high * PAGE_SIZE;
	case WMARK_LOW_LIMIT:
		return (u64)counter->wmark_low * PAGE_SIZE;
	default:
		BUG();
	}
}

static void memcg_flush_percpu_vmstats(struct mem_cgroup *memcg)
{
	unsigned long stat[MEMCG_NR_STAT];
	struct mem_cgroup *mi;
	int node, cpu, i;

	for (i = 0; i < MEMCG_NR_STAT; i++)
		stat[i] = 0;

	for_each_online_cpu(cpu)
		for (i = 0; i < MEMCG_NR_STAT; i++)
			stat[i] += per_cpu(memcg->vmstats_percpu->stat[i], cpu);

	for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
		for (i = 0; i < MEMCG_NR_STAT; i++)
			atomic_long_add(stat[i], &mi->vmstats[i]);

	for_each_node(node) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[node];
		struct mem_cgroup_per_node *pi;

		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
			stat[i] = 0;

		for_each_online_cpu(cpu)
			for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
				stat[i] += per_cpu(
					pn->lruvec_stat_cpu->count[i], cpu);

		for (pi = pn; pi; pi = parent_nodeinfo(pi, node))
			for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
				atomic_long_add(stat[i], &pi->lruvec_stat[i]);
	}
}

static void memcg_flush_percpu_vmevents(struct mem_cgroup *memcg)
{
	unsigned long events[NR_VM_EVENT_ITEMS];
	struct mem_cgroup *mi;
	int cpu, i;

	for (i = 0; i < NR_VM_EVENT_ITEMS; i++)
		events[i] = 0;

	for_each_online_cpu(cpu)
		for (i = 0; i < NR_VM_EVENT_ITEMS; i++)
			events[i] += per_cpu(memcg->vmstats_percpu->events[i],
					     cpu);

	for (mi = memcg; mi; mi = parent_mem_cgroup(mi))
		for (i = 0; i < NR_VM_EVENT_ITEMS; i++)
			atomic_long_add(events[i], &mi->vmevents[i]);
}

#ifdef CONFIG_MEMCG_KMEM
static int memcg_online_kmem(struct mem_cgroup *memcg)
{
	int memcg_id;

	if (cgroup_memory_nokmem)
		return 0;

	BUG_ON(memcg->kmemcg_id >= 0);
	BUG_ON(memcg->kmem_state);

	memcg_id = memcg_alloc_cache_id();
	if (memcg_id < 0)
		return memcg_id;

	static_branch_inc(&memcg_kmem_enabled_key);
	/*
	 * A memory cgroup is considered kmem-online as soon as it gets
	 * kmemcg_id. Setting the id after enabling static branching will
	 * guarantee no one starts accounting before all call sites are
	 * patched.
	 */
	memcg->kmemcg_id = memcg_id;
	memcg->kmem_state = KMEM_ONLINE;
	INIT_LIST_HEAD(&memcg->kmem_caches);

	return 0;
}

static void memcg_offline_kmem(struct mem_cgroup *memcg)
{
	struct cgroup_subsys_state *css;
	struct mem_cgroup *parent, *child;
	int kmemcg_id;

	if (memcg->kmem_state != KMEM_ONLINE)
		return;
	/*
	 * Clear the online state before clearing memcg_caches array
	 * entries. The slab_mutex in memcg_deactivate_kmem_caches()
	 * guarantees that no cache will be created for this cgroup
	 * after we are done (see memcg_create_kmem_cache()).
	 */
	memcg->kmem_state = KMEM_ALLOCATED;

	memcg_deactivate_kmem_caches(memcg);

	kmemcg_id = memcg->kmemcg_id;
	BUG_ON(kmemcg_id < 0);

	parent = parent_mem_cgroup(memcg);
	if (!parent)
		parent = root_mem_cgroup;

	/*
	 * Change kmemcg_id of this cgroup and all its descendants to the
	 * parent's id, and then move all entries from this cgroup's list_lrus
	 * to ones of the parent. After we have finished, all list_lrus
	 * corresponding to this cgroup are guaranteed to remain empty. The
	 * ordering is imposed by list_lru_node->lock taken by
	 * memcg_drain_all_list_lrus().
	 */
	rcu_read_lock(); /* can be called from css_free w/o cgroup_mutex */
	css_for_each_descendant_pre(css, &memcg->css) {
		child = mem_cgroup_from_css(css);
		BUG_ON(child->kmemcg_id != kmemcg_id);
		child->kmemcg_id = parent->kmemcg_id;
		if (!memcg->use_hierarchy)
			break;
	}
	rcu_read_unlock();

	memcg_drain_all_list_lrus(kmemcg_id, parent);

	memcg_free_cache_id(kmemcg_id);
}

static void memcg_free_kmem(struct mem_cgroup *memcg)
{
	/* css_alloc() failed, offlining didn't happen */
	if (unlikely(memcg->kmem_state == KMEM_ONLINE))
		memcg_offline_kmem(memcg);

	if (memcg->kmem_state == KMEM_ALLOCATED) {
		memcg_destroy_kmem_caches(memcg);
		static_branch_dec(&memcg_kmem_enabled_key);
		WARN_ON(page_counter_read(&memcg->kmem));
	}
}
#else
static int memcg_online_kmem(struct mem_cgroup *memcg)
{
	return 0;
}
static void memcg_offline_kmem(struct mem_cgroup *memcg)
{
}
static void memcg_free_kmem(struct mem_cgroup *memcg)
{
}
#endif /* CONFIG_MEMCG_KMEM */

static int memcg_update_kmem_max(struct mem_cgroup *memcg,
				 unsigned long max)
{
	int ret;

	mutex_lock(&memcg_max_mutex);
	ret = page_counter_set_max(&memcg->kmem, max);
	mutex_unlock(&memcg_max_mutex);
	return ret;
}

static int memcg_update_tcp_max(struct mem_cgroup *memcg, unsigned long max)
{
	int ret;

	mutex_lock(&memcg_max_mutex);

	ret = page_counter_set_max(&memcg->tcpmem, max);
	if (ret)
		goto out;

	if (!memcg->tcpmem_active) {
		/*
		 * The active flag needs to be written after the static_key
		 * update. This is what guarantees that the socket activation
		 * function is the last one to run. See mem_cgroup_sk_alloc()
		 * for details, and note that we don't mark any socket as
		 * belonging to this memcg until that flag is up.
		 *
		 * We need to do this, because static_keys will span multiple
		 * sites, but we can't control their order. If we mark a socket
		 * as accounted, but the accounting functions are not patched in
		 * yet, we'll lose accounting.
		 *
		 * We never race with the readers in mem_cgroup_sk_alloc(),
		 * because when this value change, the code to process it is not
		 * patched in yet.
		 */
		static_branch_inc(&memcg_sockets_enabled_key);
		memcg->tcpmem_active = true;
	}
out:
	mutex_unlock(&memcg_max_mutex);
	return ret;
}

/*
 * The user of this function is...
 * RES_LIMIT.
 */
static ssize_t mem_cgroup_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long nr_pages;
	int ret;

	buf = strstrip(buf);
	ret = page_counter_memparse(buf, "-1", &nr_pages);
	if (ret)
		return ret;

	switch (MEMFILE_ATTR(of_cft(of)->private)) {
	case RES_LIMIT:
		if (mem_cgroup_is_root(memcg)) { /* Can't set limit on root */
			ret = -EINVAL;
			break;
		}
		switch (MEMFILE_TYPE(of_cft(of)->private)) {
		case _MEM:
			ret = mem_cgroup_resize_max(memcg, nr_pages, false);
			break;
		case _MEMSWAP:
			ret = mem_cgroup_resize_max(memcg, nr_pages, true);
			break;
		case _KMEM:
			ret = memcg_update_kmem_max(memcg, nr_pages);
			break;
		case _TCP:
			ret = memcg_update_tcp_max(memcg, nr_pages);
			break;
		}
		break;
	case RES_SOFT_LIMIT:
		memcg->soft_limit = nr_pages;
		ret = 0;
		break;
	}
	return ret ?: nbytes;
}

static ssize_t mem_cgroup_reset(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct page_counter *counter;

	switch (MEMFILE_TYPE(of_cft(of)->private)) {
	case _MEM:
		counter = &memcg->memory;
		break;
	case _MEMSWAP:
		counter = &memcg->memsw;
		break;
	case _KMEM:
		counter = &memcg->kmem;
		break;
	case _TCP:
		counter = &memcg->tcpmem;
		break;
	default:
		BUG();
	}

	switch (MEMFILE_ATTR(of_cft(of)->private)) {
	case RES_MAX_USAGE:
		page_counter_reset_watermark(counter);
		break;
	case RES_FAILCNT:
		counter->failcnt = 0;
		break;
	default:
		BUG();
	}

	return nbytes;
}

#ifdef CONFIG_KIDLED
static int mem_cgroup_idle_page_stats_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *iter, *memcg = mem_cgroup_from_css(seq_css(m));
	struct kidled_scan_period scan_period, period;
	struct idle_page_stats *stats, *cache;
	unsigned long scans;
	bool has_hierarchy = kidled_use_hierarchy();
	bool no_buckets = false;
	int i, j, t;

	stats = kmalloc(sizeof(struct idle_page_stats) * 2, GFP_KERNEL);
	if (!stats)
		return -ENOMEM;
	cache = stats + 1;

	down_read(&memcg->idle_stats_rwsem);
	*stats = memcg->idle_stats[memcg->idle_stable_idx];
	scans = memcg->idle_scans;
	scan_period = memcg->scan_period;
	up_read(&memcg->idle_stats_rwsem);

	/* Nothing will be outputed with invalid buckets */
	if (KIDLED_IS_BUCKET_INVALID(stats->buckets)) {
		no_buckets = true;
		scans = 0;
		goto output;
	}

	/* Zeroes will be output with mismatched scan period */
	if (!kidled_is_scan_period_equal(&scan_period)) {
		memset(&stats->count, 0, sizeof(stats->count));
		scan_period = kidled_get_current_scan_period();
		scans = 0;
		goto output;
	}

	if (mem_cgroup_is_root(memcg) || has_hierarchy) {
		for_each_mem_cgroup_tree(iter, memcg) {
			/* The root memcg was just accounted */
			if (iter == memcg)
				continue;

			down_read(&iter->idle_stats_rwsem);
			*cache = iter->idle_stats[iter->idle_stable_idx];
			period = memcg->scan_period;
			up_read(&iter->idle_stats_rwsem);

			/*
			 * Skip to account if the scan period is mismatched
			 * or buckets are invalid.
			 */
			if (!kidled_is_scan_period_equal(&period) ||
			     KIDLED_IS_BUCKET_INVALID(cache->buckets))
				continue;

			/*
			 * The buckets of current memory cgroup might be
			 * mismatched with that of root memory cgroup. We
			 * charge the current statistics to the possibly
			 * largest bucket. The users need to apply the
			 * consistent buckets into the memory cgroups in
			 * the hierarchy tree.
			 */
			for (i = 0; i < NUM_KIDLED_BUCKETS; i++) {
				for (j = 0; j < NUM_KIDLED_BUCKETS - 1; j++) {
					if (cache->buckets[i] <=
					    stats->buckets[j])
						break;
				}

				for (t = 0; t < KIDLE_NR_TYPE; t++)
					stats->count[t][j] +=
						cache->count[t][i];
			}
		}
	}


output:
	seq_printf(m, "# version: %s\n", KIDLED_VERSION);
	seq_printf(m, "# scans: %lu\n", scans);
	seq_printf(m, "# scan_period_in_seconds: %u\n", scan_period.duration);
	seq_printf(m, "# use_hierarchy: %u\n", kidled_use_hierarchy());
	seq_puts(m, "# buckets: ");
	if (no_buckets) {
		seq_puts(m, "no valid bucket available\n");
		goto out;
	}

	for (i = 0; i < NUM_KIDLED_BUCKETS; i++) {
		seq_printf(m, "%d", stats->buckets[i]);

		if ((i == NUM_KIDLED_BUCKETS - 1) ||
		    !stats->buckets[i + 1]) {
			seq_puts(m, "\n");
			j = i + 1;
			break;
		}
		seq_puts(m, ",");
	}
	seq_puts(m, "#\n");

	seq_puts(m, "#   _-----=> clean/dirty\n");
	seq_puts(m, "#  / _----=> swap/file\n");
	seq_puts(m, "# | / _---=> evict/unevict\n");
	seq_puts(m, "# || / _--=> inactive/active\n");
	seq_puts(m, "# ||| /\n");

	seq_printf(m, "# %-8s", "||||");
	for (i = 0; i < j; i++) {
		char region[20];

		if (i == j - 1) {
			snprintf(region, sizeof(region), "[%d,+inf)",
				 stats->buckets[i]);
		} else {
			snprintf(region, sizeof(region), "[%d,%d)",
				 stats->buckets[i],
				 stats->buckets[i + 1]);
		}

		seq_printf(m, " %14s", region);
	}
	seq_puts(m, "\n");

	for (t = 0; t < KIDLE_NR_TYPE; t++) {
		char kidled_type_str[5];

		kidled_type_str[0] = t & KIDLE_DIRTY   ? 'd' : 'c';
		kidled_type_str[1] = t & KIDLE_FILE    ? 'f' : 's';
		kidled_type_str[2] = t & KIDLE_UNEVICT ? 'u' : 'e';
		kidled_type_str[3] = t & KIDLE_ACTIVE  ? 'a' : 'i';
		kidled_type_str[4] = '\0';
		seq_printf(m, "  %-8s", kidled_type_str);

		for (i = 0; i < j; i++) {
			seq_printf(m, " %14lu",
				   stats->count[t][i] << PAGE_SHIFT);
		}

		seq_puts(m, "\n");
	}

out:
	kfree(stats);
	return 0;
}

static ssize_t mem_cgroup_idle_page_stats_write(struct kernfs_open_file *of,
						char *buf, size_t nbytes,
						loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct idle_page_stats *stable_stats, *unstable_stats;
	int buckets[NUM_KIDLED_BUCKETS] = { 0 }, i = 0, err;
	unsigned long prev = 0, curr;
	char *next;

	buf = strstrip(buf);
	while (*buf) {
		if (i >= NUM_KIDLED_BUCKETS)
			return -E2BIG;

		/* Get next entry */
		next = buf + 1;
		while (*next && *next >= '0' && *next <= '9')
			next++;
		while (*next && (*next == ' ' || *next == ','))
			*next++ = '\0';

		/* Should be monotonically increasing */
		err = kstrtoul(buf, 10, &curr);
		if (err ||  curr > KIDLED_MAX_IDLE_AGE || curr <= prev)
			return -EINVAL;

		buckets[i++] = curr;
		prev = curr;
		buf = next;
	}

	/* No buckets set, mark it invalid */
	if (i == 0)
		KIDLED_MARK_BUCKET_INVALID(buckets);
	if (down_write_killable(&memcg->idle_stats_rwsem))
		return -EINTR;
	stable_stats = mem_cgroup_get_stable_idle_stats(memcg);
	unstable_stats = mem_cgroup_get_unstable_idle_stats(memcg);
	memcpy(stable_stats->buckets, buckets, sizeof(buckets));

	/*
	 * We will clear the stats without check the buckets whether
	 * has been changed, it works when user only wants to reset
	 * stats but not to reset the buckets.
	 */
	memset(stable_stats->count, 0, sizeof(stable_stats->count));

	/*
	 * It's safe that the kidled reads the unstable buckets without
	 * holding any read side locks.
	 */
	KIDLED_MARK_BUCKET_INVALID(unstable_stats->buckets);
	memcg->idle_scans = 0;
	up_write(&memcg->idle_stats_rwsem);

	return nbytes;
}

static void kidled_memcg_init(struct mem_cgroup *memcg)
{
	int type;

	init_rwsem(&memcg->idle_stats_rwsem);
	for (type = 0; type < KIDLED_STATS_NR_TYPE; type++) {
		memcpy(memcg->idle_stats[type].buckets,
		       kidled_default_buckets,
		       sizeof(kidled_default_buckets));
	}
}

static void kidled_memcg_inherit_parent_buckets(struct mem_cgroup *parent,
						struct mem_cgroup *memcg)
{
	int idle_buckets[NUM_KIDLED_BUCKETS], type;

	down_read(&parent->idle_stats_rwsem);
	memcpy(idle_buckets,
	       parent->idle_stats[parent->idle_stable_idx].buckets,
	       sizeof(idle_buckets));
	up_read(&parent->idle_stats_rwsem);

	for (type = 0; type < KIDLED_STATS_NR_TYPE; type++) {
		memcpy(memcg->idle_stats[type].buckets,
		       idle_buckets,
		       sizeof(idle_buckets));
	}
}
#else
static void kidled_memcg_init(struct mem_cgroup *memcg)
{
}

static void kidled_memcg_inherit_parent_buckets(struct mem_cgroup *parent,
						struct mem_cgroup *memcg)
{
}
#endif /* CONFIG_KIDLED */

static u64 mem_cgroup_move_charge_read(struct cgroup_subsys_state *css,
					struct cftype *cft)
{
	return mem_cgroup_from_css(css)->move_charge_at_immigrate;
}

#ifdef CONFIG_MMU
static int mem_cgroup_move_charge_write(struct cgroup_subsys_state *css,
					struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	if (val & ~MOVE_MASK)
		return -EINVAL;

	/*
	 * No kind of locking is needed in here, because ->can_attach() will
	 * check this value once in the beginning of the process, and then carry
	 * on with stale data. This means that changes to this value will only
	 * affect task migrations starting after the change.
	 */
	memcg->move_charge_at_immigrate = val;
	return 0;
}
#else
static int mem_cgroup_move_charge_write(struct cgroup_subsys_state *css,
					struct cftype *cft, u64 val)
{
	return -ENOSYS;
}
#endif

#ifdef CONFIG_NUMA

#define LRU_ALL_FILE (BIT(LRU_INACTIVE_FILE) | BIT(LRU_ACTIVE_FILE))
#define LRU_ALL_ANON (BIT(LRU_INACTIVE_ANON) | BIT(LRU_ACTIVE_ANON))
#define LRU_ALL	     ((1 << NR_LRU_LISTS) - 1)

static unsigned long mem_cgroup_node_nr_lru_pages(struct mem_cgroup *memcg,
					   int nid, unsigned int lru_mask)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
	unsigned long nr = 0;
	enum lru_list lru;

	VM_BUG_ON((unsigned)nid >= nr_node_ids);

	for_each_lru(lru) {
		if (!(BIT(lru) & lru_mask))
			continue;
		nr += lruvec_page_state_local(lruvec, NR_LRU_BASE + lru);
	}
	return nr;
}

static unsigned long mem_cgroup_nr_lru_pages(struct mem_cgroup *memcg,
					     unsigned int lru_mask)
{
	unsigned long nr = 0;
	enum lru_list lru;

	for_each_lru(lru) {
		if (!(BIT(lru) & lru_mask))
			continue;
		nr += memcg_page_state_local(memcg, NR_LRU_BASE + lru);
	}
	return nr;
}

static int memcg_numa_stat_show(struct seq_file *m, void *v)
{
	struct numa_stat {
		const char *name;
		unsigned int lru_mask;
	};

	static const struct numa_stat stats[] = {
		{ "total", LRU_ALL },
		{ "file", LRU_ALL_FILE },
		{ "anon", LRU_ALL_ANON },
		{ "unevictable", BIT(LRU_UNEVICTABLE) },
	};
	const struct numa_stat *stat;
	int nid;
	unsigned long nr;
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	for (stat = stats; stat < stats + ARRAY_SIZE(stats); stat++) {
		nr = mem_cgroup_nr_lru_pages(memcg, stat->lru_mask);
		seq_printf(m, "%s=%lu", stat->name, nr);
		for_each_node_state(nid, N_MEMORY) {
			nr = mem_cgroup_node_nr_lru_pages(memcg, nid,
							  stat->lru_mask);
			seq_printf(m, " N%d=%lu", nid, nr);
		}
		seq_putc(m, '\n');
	}

	for (stat = stats; stat < stats + ARRAY_SIZE(stats); stat++) {
		struct mem_cgroup *iter;

		nr = 0;
		for_each_mem_cgroup_tree(iter, memcg)
			nr += mem_cgroup_nr_lru_pages(iter, stat->lru_mask);
		seq_printf(m, "hierarchical_%s=%lu", stat->name, nr);
		for_each_node_state(nid, N_MEMORY) {
			nr = 0;
			for_each_mem_cgroup_tree(iter, memcg)
				nr += mem_cgroup_node_nr_lru_pages(
					iter, nid, stat->lru_mask);
			seq_printf(m, " N%d=%lu", nid, nr);
		}
		seq_putc(m, '\n');
	}

	return 0;
}
#endif /* CONFIG_NUMA */

/* Universal VM events cgroup1 shows, original sort order */
static const unsigned int memcg1_events[] = {
	PGPGIN,
	PGPGOUT,
	PGFAULT,
	PGMAJFAULT,
};

static const char *const memcg1_event_names[] = {
	"pgpgin",
	"pgpgout",
	"pgfault",
	"pgmajfault",
};

static int memcg_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long memory, memsw;
	struct mem_cgroup *mi;
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(memcg1_stat_names) != ARRAY_SIZE(memcg1_stats));
	BUILD_BUG_ON(ARRAY_SIZE(mem_cgroup_lru_names) != NR_LRU_LISTS);

	for (i = 0; i < ARRAY_SIZE(memcg1_stats); i++) {
		unsigned long nr;

		if (memcg1_stats[i] == MEMCG_SWAP && !do_memsw_account())
			continue;
		nr = memcg_page_state_local(memcg, memcg1_stats[i]);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		if (memcg1_stats[i] == NR_ANON_THPS)
			nr *= HPAGE_PMD_NR;
#endif
		seq_printf(m, "%s %lu\n", memcg1_stat_names[i], nr * PAGE_SIZE);
	}

	for (i = 0; i < ARRAY_SIZE(memcg1_events); i++)
		seq_printf(m, "%s %lu\n", memcg1_event_names[i],
			   memcg_events_local(memcg, memcg1_events[i]));

	for (i = 0; i < NR_LRU_LISTS; i++)
		seq_printf(m, "%s %lu\n", mem_cgroup_lru_names[i],
			   memcg_page_state_local(memcg, NR_LRU_BASE + i) *
			   PAGE_SIZE);

	/* Hierarchical information */
	memory = memsw = PAGE_COUNTER_MAX;
	for (mi = memcg; mi; mi = parent_mem_cgroup(mi)) {
		memory = min(memory, READ_ONCE(mi->memory.max));
		memsw = min(memsw, READ_ONCE(mi->memsw.max));
	}
	seq_printf(m, "hierarchical_memory_limit %llu\n",
		   (u64)memory * PAGE_SIZE);
	if (do_memsw_account())
		seq_printf(m, "hierarchical_memsw_limit %llu\n",
			   (u64)memsw * PAGE_SIZE);

	for (i = 0; i < ARRAY_SIZE(memcg1_stats); i++) {
		unsigned long nr;

		if (memcg1_stats[i] == MEMCG_SWAP && !do_memsw_account())
			continue;
		nr = memcg_page_state(memcg, memcg1_stats[i]);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		if (memcg1_stats[i] == NR_ANON_THPS)
			nr *= HPAGE_PMD_NR;
#endif
		seq_printf(m, "total_%s %llu\n", memcg1_stat_names[i],
						(u64)nr * PAGE_SIZE);
	}

	for (i = 0; i < ARRAY_SIZE(memcg1_events); i++)
		seq_printf(m, "total_%s %llu\n", memcg1_event_names[i],
			   (u64)memcg_events(memcg, memcg1_events[i]));

	for (i = 0; i < NR_LRU_LISTS; i++)
		seq_printf(m, "total_%s %llu\n", mem_cgroup_lru_names[i],
			   (u64)memcg_page_state(memcg, NR_LRU_BASE + i) *
			   PAGE_SIZE);

#ifdef CONFIG_DEBUG_VM
	{
		pg_data_t *pgdat;
		struct mem_cgroup_per_node *mz;
		unsigned long anon_cost = 0;
		unsigned long file_cost = 0;

		for_each_online_pgdat(pgdat) {
			mz = mem_cgroup_nodeinfo(memcg, pgdat->node_id);

			anon_cost += mz->lruvec.anon_cost;
			file_cost += mz->lruvec.file_cost;
		}
		seq_printf(m, "anon_cost %lu\n", anon_cost);
		seq_printf(m, "file_cost %lu\n", file_cost);
	}
#endif

	return 0;
}

static u64 memcg_exstat_gather(struct mem_cgroup *memcg,
			       enum memcg_exstat_item idx)
{
	u64 sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu_ptr(memcg->exstat_cpu, cpu)->item[idx];

	return sum;
}

static int memcg_exstat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	seq_printf(m, "wmark_min_throttled_ms %llu\n",
		   memcg_exstat_gather(memcg, MEMCG_WMARK_MIN));
	seq_printf(m, "wmark_reclaim_work_ms %llu\n",
		   memcg_exstat_gather(memcg, MEMCG_WMARK_RECLAIM) >> 20);

	return 0;
}

static s64 mem_cgroup_swappiness_read(struct cgroup_subsys_state *css,
				      struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return mem_cgroup_swappiness(memcg);
}

static int mem_cgroup_swappiness_write(struct cgroup_subsys_state *css,
				       struct cftype *cft, s64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	if (val > 100 || val < -1 || (css->parent && val < 0))
		return -EINVAL;

	if (css->parent)
		memcg->swappiness = val;
	else
		vm_swappiness = val;

	return 0;
}

static u64 mem_cgroup_priority_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return memcg->priority;
}

static int mem_cgroup_priority_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	if (val > MEMCG_OOM_PRIORITY)
		return -EINVAL;

	memcg->priority = val;

	return 0;
}

static int memory_wmark_ratio_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned int wmark_ratio = READ_ONCE(memcg->wmark_ratio);

	seq_printf(m, "%d\n", wmark_ratio);

	return 0;
}

static ssize_t memory_wmark_ratio_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret, wmark_ratio;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	ret = kstrtouint(buf, 0, &wmark_ratio);
	if (ret)
		return ret;

	if (wmark_ratio > 100)
		return -EINVAL;

	xchg(&memcg->wmark_ratio, wmark_ratio);

	setup_memcg_wmark(memcg);

	if (!is_wmark_ok(memcg, true))
		queue_work(memcg_wmark_wq, &memcg->wmark_work);

	return nbytes;
}

static int memory_wmark_scale_factor_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned int wmark_scale_factor;

	wmark_scale_factor = READ_ONCE(memcg->wmark_scale_factor);

	seq_printf(m, "%d\n", wmark_scale_factor);

	return 0;
}

static ssize_t memory_wmark_scale_factor_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret, wmark_scale_factor;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	ret = kstrtouint(buf, 0, &wmark_scale_factor);
	if (ret)
		return ret;

	if (wmark_scale_factor > 1000 || wmark_scale_factor < 1)
		return -EINVAL;

	xchg(&memcg->wmark_scale_factor, wmark_scale_factor);

	setup_memcg_wmark(memcg);

	return nbytes;
}

/*
 * Figure out the maximal(most conservative) @wmark_min_adj along
 * the hierarchy but excluding intermediate default zero, as the
 * effective one.  Example:
 *                      root
 *                      / \
 *                     A   D
 *                    / \
 *                   B   C
 *                  / \
 *                 E   F
 *
 * wmark_min_adj:  A -10, B -25, C 0, D 50, E -25, F 50
 * wmark_min_eadj: A -10, B -10, C 0, D 50, E -10, F 50
 */
static void memcg_update_wmark_min_adj(struct mem_cgroup *memcg, int val)
{
	struct mem_cgroup *p;
	struct mem_cgroup *iter;

	mutex_lock(&cgroup_mutex);
	memcg->wmark_min_adj = val;
	/* update hierarchical wmark_min_eadj, pre-order iteration */
	for_each_mem_cgroup_tree(iter, memcg) {
		if (!mem_cgroup_online(iter))
			continue;
		val = iter->wmark_min_adj;
		p = parent_mem_cgroup(iter);
		if (p && p->wmark_min_eadj && p->wmark_min_eadj > val)
			val = p->wmark_min_eadj;
		iter->wmark_min_eadj = val;
	}
	mutex_unlock(&cgroup_mutex);
}

static int memory_wmark_min_adj_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	/* show the final effective value */
	seq_printf(m, "%d\n", memcg->wmark_min_eadj);

	return 0;
}

static ssize_t memory_wmark_min_adj_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret, wmark_min_adj;

	buf = strstrip(buf);
	ret = kstrtoint(buf, 0, &wmark_min_adj);
	if (ret)
		return ret;

	if (wmark_min_adj < -25 || wmark_min_adj > 50)
		return -EINVAL;

	memcg_update_wmark_min_adj(memcg, wmark_min_adj);

	return nbytes;
}

int memcg_get_wmark_min_adj(struct task_struct *curr)
{
	struct mem_cgroup *memcg;
	int val;

	if (mem_cgroup_disabled())
		return 0;

	rcu_read_lock();
	memcg = mem_cgroup_from_css(task_css(curr, memory_cgrp_id));
	if (mem_cgroup_is_root(memcg))
		val = 0;
	else
		val = memcg->wmark_min_eadj;
	rcu_read_unlock();

	return val;
}

/*
 * Scheduled by global page allocation to be executed from the userland
 * return path and throttle when free is under memcg's global WMARK_MIN.
 */
void mem_cgroup_wmark_min_throttle(void)
{
	unsigned int msec = current->wmark_min_throttle_ms;
	unsigned long pflags;
	struct mem_cgroup *memcg, *iter;

	if (likely(!msec))
		return;
	psi_memstall_enter(&pflags);
	msleep_interruptible(msec);
	psi_memstall_leave(&pflags);
	current->wmark_min_throttle_ms = 0;

	/* Account throttled time hierarchically, ignore premature sleep */
	memcg = get_mem_cgroup_from_mm(current->mm);
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter))
		__this_cpu_add(iter->exstat_cpu->item[MEMCG_WMARK_MIN], msec);
	css_put(&memcg->css);
}

#define WMARK_MIN_THROTTLE_MS 100UL
/*
 * Tasks in memcg having positive memory.wmark_min_adj has its
 * own global min watermark higher than the global WMARK_MIN:
 * "WMARK_MIN + (WMARK_LOW - WMARK_MIN) * memory.wmark_min_adj"
 *
 * Positive memory.wmark_min_adj means low QoS requirements. When
 * allocation broke memcg min watermark, it should trigger direct
 * reclaim traditionally, here trigger throttle instead to further
 * prevent them from disturbing others.
 *
 * The throttle time is simply linearly proportional to the pages
 * consumed below memcg's min watermark.
 *
 * The base throttle time is WMARK_MIN_THROTTLE_MS, and the maximal
 * throttle time is ten times WMARK_MIN_THROTTLE_MS.
 *
 * The actual throttling will be executed from the userland return
 * path, see mem_cgroup_wmark_min_throttle().
 */
void memcg_check_wmark_min_adj(struct task_struct *curr,
		struct alloc_context *ac)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long wmark_min, wmark, min_low_gap, free_pages;
	int wmark_min_adj = memcg_get_wmark_min_adj(curr);

	if (wmark_min_adj <= 0)
		return;

	if (curr->wmark_min_throttle_ms)
		return;

	z = first_zones_zonelist(ac->zonelist, ac->high_zoneidx, ac->nodemask);
	for_next_zone_zonelist_nodemask(zone, z, ac->zonelist,
			ac->high_zoneidx, ac->nodemask) {
		if (cpusets_enabled() &&
		    !__cpuset_zone_allowed(zone, __GFP_HARDWALL))
			continue;

		wmark_min = min_wmark_pages(zone);
		min_low_gap = low_wmark_pages(zone) - wmark_min;
		free_pages = zone_page_state(zone, NR_FREE_PAGES);
		wmark = wmark_min + min_low_gap * wmark_min_adj / 100;
		if (free_pages < wmark && wmark > wmark_min) {
			unsigned long msec;

			/*
			 * The throttle time is simply linearly proportional
			 * to the pages consumed below memcg's min watermark.
			 */
			msec = (wmark - free_pages) * WMARK_MIN_THROTTLE_MS /
					(wmark - wmark_min);
			msec = clamp(msec, 1UL, 10 * WMARK_MIN_THROTTLE_MS);
			curr->wmark_min_throttle_ms = msec;
			set_notify_resume(curr);
			break;
		}
	}
}

#ifdef CONFIG_MEMSLI
#define MEMCG_LAT_STAT_SMP_WRITE(name, sidx)				\
static void smp_write_##name(void *info)				\
{									\
	struct mem_cgroup *memcg = (struct mem_cgroup *)info;		\
	int i;								\
									\
	for (i = MEM_LAT_0_1; i < MEM_LAT_NR_COUNT; i++)		\
		this_cpu_write(memcg->lat_stat_cpu->item[sidx][i], 0);	\
}									\

MEMCG_LAT_STAT_SMP_WRITE(global_direct_reclaim, MEM_LAT_GLOBAL_DIRECT_RECLAIM);
MEMCG_LAT_STAT_SMP_WRITE(memcg_direct_reclaim, MEM_LAT_MEMCG_DIRECT_RECLAIM);
MEMCG_LAT_STAT_SMP_WRITE(direct_compact, MEM_LAT_DIRECT_COMPACT);
MEMCG_LAT_STAT_SMP_WRITE(global_direct_swapout, MEM_LAT_GLOBAL_DIRECT_SWAPOUT);
MEMCG_LAT_STAT_SMP_WRITE(memcg_direct_swapout, MEM_LAT_MEMCG_DIRECT_SWAPOUT);
MEMCG_LAT_STAT_SMP_WRITE(direct_swapin, MEM_LAT_DIRECT_SWAPIN);

smp_call_func_t smp_memcg_lat_write_funcs[] = {
	smp_write_global_direct_reclaim,
	smp_write_memcg_direct_reclaim,
	smp_write_direct_compact,
	smp_write_global_direct_swapout,
	smp_write_memcg_direct_swapout,
	smp_write_direct_swapin,
};

static int memcg_lat_stat_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	enum mem_lat_stat_item idx = cft->private;
	smp_call_func_t func = smp_memcg_lat_write_funcs[idx];

	if (val != 0)
		return -EINVAL;

	func((void *)memcg);
	smp_call_function(func, (void *)memcg, 1);

	return 0;
}

static u64 memcg_lat_stat_gather(struct mem_cgroup *memcg,
				 enum mem_lat_stat_item sidx,
				 enum mem_lat_count_t cidx)
{
	u64 sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu_ptr(memcg->lat_stat_cpu, cpu)->item[sidx][cidx];

	return sum;
}

static int memcg_lat_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	enum mem_lat_stat_item idx = seq_cft(m)->private;

	seq_printf(m, "0-1ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_0_1));
	seq_printf(m, "1-5ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_1_5));
	seq_printf(m, "5-10ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_5_10));
	seq_printf(m, "10-100ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_10_100));
	seq_printf(m, "100-500ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_100_500));
	seq_printf(m, "500-1000ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_500_1000));
	seq_printf(m, ">=1000ms: \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_1000_INF));
	seq_printf(m, "total(ms): \t%llu\n",
		   memcg_lat_stat_gather(memcg, idx, MEM_LAT_TOTAL) >> 20);

	return 0;
}

static enum mem_lat_count_t get_mem_lat_count_idx(u64 duration)
{
	enum mem_lat_count_t idx;

	duration = duration >> 20;
	if (duration < 1)
		idx = MEM_LAT_0_1;
	else if (duration < 5)
		idx = MEM_LAT_1_5;
	else if (duration < 10)
		idx = MEM_LAT_5_10;
	else if (duration < 100)
		idx = MEM_LAT_10_100;
	else if (duration < 500)
		idx = MEM_LAT_100_500;
	else if (duration < 1000)
		idx = MEM_LAT_500_1000;
	else
		idx = MEM_LAT_1000_INF;

	return idx;
}

void memcg_lat_stat_start(u64 *start)
{
	if (!static_branch_unlikely(&cgroup_memory_nosli) &&
	    !mem_cgroup_disabled())
		*start = ktime_get_ns();
	else
		*start = 0;
}

void memcg_lat_stat_end(enum mem_lat_stat_item sidx, u64 start)
{
	struct mem_cgroup *memcg, *iter;
	enum mem_lat_count_t cidx;
	u64 duration;

	if (static_branch_unlikely(&cgroup_memory_nosli) ||
	    mem_cgroup_disabled())
		return;

	if (start == 0)
		return;

	duration = ktime_get_ns() - start;
	cidx = get_mem_lat_count_idx(duration);
	memcg = get_mem_cgroup_from_mm(current->mm);
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		this_cpu_inc(iter->lat_stat_cpu->item[sidx][cidx]);
		this_cpu_add(iter->lat_stat_cpu->item[sidx][MEM_LAT_TOTAL],
			       duration);
	}
	css_put(&memcg->css);
}
#endif /* CONFIG_MEMSLI */

static void __mem_cgroup_threshold(struct mem_cgroup *memcg, bool swap)
{
	struct mem_cgroup_threshold_ary *t;
	unsigned long usage;
	int i;

	rcu_read_lock();
	if (!swap)
		t = rcu_dereference(memcg->thresholds.primary);
	else
		t = rcu_dereference(memcg->memsw_thresholds.primary);

	if (!t)
		goto unlock;

	usage = mem_cgroup_usage(memcg, swap);

	/*
	 * current_threshold points to threshold just below or equal to usage.
	 * If it's not true, a threshold was crossed after last
	 * call of __mem_cgroup_threshold().
	 */
	i = t->current_threshold;

	/*
	 * Iterate backward over array of thresholds starting from
	 * current_threshold and check if a threshold is crossed.
	 * If none of thresholds below usage is crossed, we read
	 * only one element of the array here.
	 */
	for (; i >= 0 && unlikely(t->entries[i].threshold > usage); i--)
		eventfd_signal(t->entries[i].eventfd, 1);

	/* i = current_threshold + 1 */
	i++;

	/*
	 * Iterate forward over array of thresholds starting from
	 * current_threshold+1 and check if a threshold is crossed.
	 * If none of thresholds above usage is crossed, we read
	 * only one element of the array here.
	 */
	for (; i < t->size && unlikely(t->entries[i].threshold <= usage); i++)
		eventfd_signal(t->entries[i].eventfd, 1);

	/* Update current_threshold */
	t->current_threshold = i - 1;
unlock:
	rcu_read_unlock();
}

static void mem_cgroup_threshold(struct mem_cgroup *memcg)
{
	while (memcg) {
		__mem_cgroup_threshold(memcg, false);
		if (do_memsw_account())
			__mem_cgroup_threshold(memcg, true);

		memcg = parent_mem_cgroup(memcg);
	}
}

static int compare_thresholds(const void *a, const void *b)
{
	const struct mem_cgroup_threshold *_a = a;
	const struct mem_cgroup_threshold *_b = b;

	if (_a->threshold > _b->threshold)
		return 1;

	if (_a->threshold < _b->threshold)
		return -1;

	return 0;
}

static int mem_cgroup_oom_notify_cb(struct mem_cgroup *memcg)
{
	struct mem_cgroup_eventfd_list *ev;

	spin_lock(&memcg_oom_lock);

	list_for_each_entry(ev, &memcg->oom_notify, list)
		eventfd_signal(ev->eventfd, 1);

	spin_unlock(&memcg_oom_lock);
	return 0;
}

static void mem_cgroup_oom_notify(struct mem_cgroup *memcg)
{
	struct mem_cgroup *iter;

	for_each_mem_cgroup_tree(iter, memcg)
		mem_cgroup_oom_notify_cb(iter);
}

static int __mem_cgroup_usage_register_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd, const char *args, enum res_type type)
{
	struct mem_cgroup_thresholds *thresholds;
	struct mem_cgroup_threshold_ary *new;
	unsigned long threshold;
	unsigned long usage;
	int i, size, ret;

	ret = page_counter_memparse(args, "-1", &threshold);
	if (ret)
		return ret;

	mutex_lock(&memcg->thresholds_lock);

	if (type == _MEM) {
		thresholds = &memcg->thresholds;
		usage = mem_cgroup_usage(memcg, false);
	} else if (type == _MEMSWAP) {
		thresholds = &memcg->memsw_thresholds;
		usage = mem_cgroup_usage(memcg, true);
	} else
		BUG();

	/* Check if a threshold crossed before adding a new one */
	if (thresholds->primary)
		__mem_cgroup_threshold(memcg, type == _MEMSWAP);

	size = thresholds->primary ? thresholds->primary->size + 1 : 1;

	/* Allocate memory for new array of thresholds */
	new = kmalloc(sizeof(*new) + size * sizeof(struct mem_cgroup_threshold),
			GFP_KERNEL);
	if (!new) {
		ret = -ENOMEM;
		goto unlock;
	}
	new->size = size;

	/* Copy thresholds (if any) to new array */
	if (thresholds->primary) {
		memcpy(new->entries, thresholds->primary->entries, (size - 1) *
				sizeof(struct mem_cgroup_threshold));
	}

	/* Add new threshold */
	new->entries[size - 1].eventfd = eventfd;
	new->entries[size - 1].threshold = threshold;

	/* Sort thresholds. Registering of new threshold isn't time-critical */
	sort(new->entries, size, sizeof(struct mem_cgroup_threshold),
			compare_thresholds, NULL);

	/* Find current threshold */
	new->current_threshold = -1;
	for (i = 0; i < size; i++) {
		if (new->entries[i].threshold <= usage) {
			/*
			 * new->current_threshold will not be used until
			 * rcu_assign_pointer(), so it's safe to increment
			 * it here.
			 */
			++new->current_threshold;
		} else
			break;
	}

	/* Free old spare buffer and save old primary buffer as spare */
	kfree(thresholds->spare);
	thresholds->spare = thresholds->primary;

	rcu_assign_pointer(thresholds->primary, new);

	/* To be sure that nobody uses thresholds */
	synchronize_rcu();

unlock:
	mutex_unlock(&memcg->thresholds_lock);

	return ret;
}

static int mem_cgroup_usage_register_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd, const char *args)
{
	return __mem_cgroup_usage_register_event(memcg, eventfd, args, _MEM);
}

static int memsw_cgroup_usage_register_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd, const char *args)
{
	return __mem_cgroup_usage_register_event(memcg, eventfd, args, _MEMSWAP);
}

static void __mem_cgroup_usage_unregister_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd, enum res_type type)
{
	struct mem_cgroup_thresholds *thresholds;
	struct mem_cgroup_threshold_ary *new;
	unsigned long usage;
	int i, j, size;

	mutex_lock(&memcg->thresholds_lock);

	if (type == _MEM) {
		thresholds = &memcg->thresholds;
		usage = mem_cgroup_usage(memcg, false);
	} else if (type == _MEMSWAP) {
		thresholds = &memcg->memsw_thresholds;
		usage = mem_cgroup_usage(memcg, true);
	} else
		BUG();

	if (!thresholds->primary)
		goto unlock;

	/* Check if a threshold crossed before removing */
	__mem_cgroup_threshold(memcg, type == _MEMSWAP);

	/* Calculate new number of threshold */
	size = 0;
	for (i = 0; i < thresholds->primary->size; i++) {
		if (thresholds->primary->entries[i].eventfd != eventfd)
			size++;
	}

	new = thresholds->spare;

	/* Set thresholds array to NULL if we don't have thresholds */
	if (!size) {
		kfree(new);
		new = NULL;
		goto swap_buffers;
	}

	new->size = size;

	/* Copy thresholds and find current threshold */
	new->current_threshold = -1;
	for (i = 0, j = 0; i < thresholds->primary->size; i++) {
		if (thresholds->primary->entries[i].eventfd == eventfd)
			continue;

		new->entries[j] = thresholds->primary->entries[i];
		if (new->entries[j].threshold <= usage) {
			/*
			 * new->current_threshold will not be used
			 * until rcu_assign_pointer(), so it's safe to increment
			 * it here.
			 */
			++new->current_threshold;
		}
		j++;
	}

swap_buffers:
	/* Swap primary and spare array */
	thresholds->spare = thresholds->primary;

	rcu_assign_pointer(thresholds->primary, new);

	/* To be sure that nobody uses thresholds */
	synchronize_rcu();

	/* If all events are unregistered, free the spare array */
	if (!new) {
		kfree(thresholds->spare);
		thresholds->spare = NULL;
	}
unlock:
	mutex_unlock(&memcg->thresholds_lock);
}

static void mem_cgroup_usage_unregister_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd)
{
	return __mem_cgroup_usage_unregister_event(memcg, eventfd, _MEM);
}

static void memsw_cgroup_usage_unregister_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd)
{
	return __mem_cgroup_usage_unregister_event(memcg, eventfd, _MEMSWAP);
}

static int mem_cgroup_oom_register_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd, const char *args)
{
	struct mem_cgroup_eventfd_list *event;

	event = kmalloc(sizeof(*event),	GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	spin_lock(&memcg_oom_lock);

	event->eventfd = eventfd;
	list_add(&event->list, &memcg->oom_notify);

	/* already in OOM ? */
	if (memcg->under_oom)
		eventfd_signal(eventfd, 1);
	spin_unlock(&memcg_oom_lock);

	return 0;
}

static void mem_cgroup_oom_unregister_event(struct mem_cgroup *memcg,
	struct eventfd_ctx *eventfd)
{
	struct mem_cgroup_eventfd_list *ev, *tmp;

	spin_lock(&memcg_oom_lock);

	list_for_each_entry_safe(ev, tmp, &memcg->oom_notify, list) {
		if (ev->eventfd == eventfd) {
			list_del(&ev->list);
			kfree(ev);
		}
	}

	spin_unlock(&memcg_oom_lock);
}

static int mem_cgroup_oom_control_read(struct seq_file *sf, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(sf));

	seq_printf(sf, "oom_kill_disable %d\n", memcg->oom_kill_disable);
	seq_printf(sf, "under_oom %d\n", (bool)memcg->under_oom);
	seq_printf(sf, "oom_kill %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_OOM_KILL]));
	return 0;
}

static int mem_cgroup_oom_control_write(struct cgroup_subsys_state *css,
	struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	/* cannot set to root cgroup and only 0 and 1 are allowed */
	if (!css->parent || !((val == 0) || (val == 1)))
		return -EINVAL;

	memcg->oom_kill_disable = val;
	if (!val)
		memcg_oom_recover(memcg);

	return 0;
}

static int memory_oom_group_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	seq_printf(m, "%d\n", memcg->oom_group);

	return 0;
}

static ssize_t memory_oom_group_write(struct kernfs_open_file *of,
				      char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret, oom_group;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	ret = kstrtoint(buf, 0, &oom_group);
	if (ret)
		return ret;

	if (oom_group != 0 && oom_group != 1)
		return -EINVAL;

	memcg->oom_group = oom_group;

	return nbytes;
}
#ifdef CONFIG_CGROUP_WRITEBACK

static int memcg_wb_domain_init(struct mem_cgroup *memcg, gfp_t gfp)
{
	return wb_domain_init(&memcg->cgwb_domain, gfp);
}

static void memcg_wb_domain_exit(struct mem_cgroup *memcg)
{
	wb_domain_exit(&memcg->cgwb_domain);
}

static void memcg_wb_domain_size_changed(struct mem_cgroup *memcg)
{
	wb_domain_size_changed(&memcg->cgwb_domain);
}

struct wb_domain *mem_cgroup_wb_domain(struct bdi_writeback *wb)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(wb->memcg_css);

	if (!memcg->css.parent)
		return NULL;

	return &memcg->cgwb_domain;
}

/*
 * idx can be of type enum memcg_stat_item or node_stat_item.
 * Keep in sync with memcg_exact_page().
 */
static unsigned long memcg_exact_page_state(struct mem_cgroup *memcg, int idx)
{
	long x = atomic_long_read(&memcg->vmstats[idx]);
	int cpu;

	for_each_online_cpu(cpu)
		x += per_cpu_ptr(memcg->vmstats_percpu, cpu)->stat[idx];
	if (x < 0)
		x = 0;
	return x;
}

/**
 * mem_cgroup_wb_stats - retrieve writeback related stats from its memcg
 * @wb: bdi_writeback in question
 * @pfilepages: out parameter for number of file pages
 * @pheadroom: out parameter for number of allocatable pages according to memcg
 * @pdirty: out parameter for number of dirty pages
 * @pwriteback: out parameter for number of pages under writeback
 *
 * Determine the numbers of file, headroom, dirty, and writeback pages in
 * @wb's memcg.  File, dirty and writeback are self-explanatory.  Headroom
 * is a bit more involved.
 *
 * A memcg's headroom is "min(max, high) - used".  In the hierarchy, the
 * headroom is calculated as the lowest headroom of itself and the
 * ancestors.  Note that this doesn't consider the actual amount of
 * available memory in the system.  The caller should further cap
 * *@pheadroom accordingly.
 */
void mem_cgroup_wb_stats(struct bdi_writeback *wb, unsigned long *pfilepages,
			 unsigned long *pheadroom, unsigned long *pdirty,
			 unsigned long *pwriteback)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(wb->memcg_css);
	struct mem_cgroup *parent;

	*pdirty = memcg_exact_page_state(memcg, NR_FILE_DIRTY);

	/* this should eventually include NR_UNSTABLE_NFS */
	*pwriteback = memcg_exact_page_state(memcg, NR_WRITEBACK);
	*pfilepages = memcg_exact_page_state(memcg, NR_INACTIVE_FILE) +
			memcg_exact_page_state(memcg, NR_ACTIVE_FILE);
	*pheadroom = PAGE_COUNTER_MAX;

	while ((parent = parent_mem_cgroup(memcg))) {
		unsigned long ceiling = min(READ_ONCE(memcg->memory.max),
					    READ_ONCE(memcg->memory.high));
		unsigned long used = page_counter_read(&memcg->memory);

		*pheadroom = min(*pheadroom, ceiling - min(ceiling, used));
		memcg = parent;
	}
}

#else	/* CONFIG_CGROUP_WRITEBACK */

static int memcg_wb_domain_init(struct mem_cgroup *memcg, gfp_t gfp)
{
	return 0;
}

static void memcg_wb_domain_exit(struct mem_cgroup *memcg)
{
}

static void memcg_wb_domain_size_changed(struct mem_cgroup *memcg)
{
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

/*
 * DO NOT USE IN NEW FILES.
 *
 * "cgroup.event_control" implementation.
 *
 * This is way over-engineered.  It tries to support fully configurable
 * events for each user.  Such level of flexibility is completely
 * unnecessary especially in the light of the planned unified hierarchy.
 *
 * Please deprecate this and replace with something simpler if at all
 * possible.
 */

/*
 * Unregister event and free resources.
 *
 * Gets called from workqueue.
 */
static void memcg_event_remove(struct work_struct *work)
{
	struct mem_cgroup_event *event =
		container_of(work, struct mem_cgroup_event, remove);
	struct mem_cgroup *memcg = event->memcg;

	remove_wait_queue(event->wqh, &event->wait);

	event->unregister_event(memcg, event->eventfd);

	/* Notify userspace the event is going away. */
	eventfd_signal(event->eventfd, 1);

	eventfd_ctx_put(event->eventfd);
	kfree(event);
	css_put(&memcg->css);
}

/*
 * Gets called on EPOLLHUP on eventfd when user closes it.
 *
 * Called with wqh->lock held and interrupts disabled.
 */
static int memcg_event_wake(wait_queue_entry_t *wait, unsigned mode,
			    int sync, void *key)
{
	struct mem_cgroup_event *event =
		container_of(wait, struct mem_cgroup_event, wait);
	struct mem_cgroup *memcg = event->memcg;
	__poll_t flags = key_to_poll(key);

	if (flags & EPOLLHUP) {
		/*
		 * If the event has been detached at cgroup removal, we
		 * can simply return knowing the other side will cleanup
		 * for us.
		 *
		 * We can't race against event freeing since the other
		 * side will require wqh->lock via remove_wait_queue(),
		 * which we hold.
		 */
		spin_lock(&memcg->event_list_lock);
		if (!list_empty(&event->list)) {
			list_del_init(&event->list);
			/*
			 * We are in atomic context, but cgroup_event_remove()
			 * may sleep, so we have to call it in workqueue.
			 */
			schedule_work(&event->remove);
		}
		spin_unlock(&memcg->event_list_lock);
	}

	return 0;
}

static void memcg_event_ptable_queue_proc(struct file *file,
		wait_queue_head_t *wqh, poll_table *pt)
{
	struct mem_cgroup_event *event =
		container_of(pt, struct mem_cgroup_event, pt);

	event->wqh = wqh;
	add_wait_queue(wqh, &event->wait);
}

/*
 * DO NOT USE IN NEW FILES.
 *
 * Parse input and register new cgroup event handler.
 *
 * Input must be in format '<event_fd> <control_fd> <args>'.
 * Interpretation of args is defined by control file implementation.
 */
static ssize_t memcg_write_event_control(struct kernfs_open_file *of,
					 char *buf, size_t nbytes, loff_t off)
{
	struct cgroup_subsys_state *css = of_css(of);
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mem_cgroup_event *event;
	struct cgroup_subsys_state *cfile_css;
	unsigned int efd, cfd;
	struct fd efile;
	struct fd cfile;
	const char *name;
	char *endp;
	int ret;

	buf = strstrip(buf);

	efd = simple_strtoul(buf, &endp, 10);
	if (*endp != ' ')
		return -EINVAL;
	buf = endp + 1;

	cfd = simple_strtoul(buf, &endp, 10);
	if ((*endp != ' ') && (*endp != '\0'))
		return -EINVAL;
	buf = endp + 1;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	event->memcg = memcg;
	INIT_LIST_HEAD(&event->list);
	init_poll_funcptr(&event->pt, memcg_event_ptable_queue_proc);
	init_waitqueue_func_entry(&event->wait, memcg_event_wake);
	INIT_WORK(&event->remove, memcg_event_remove);

	efile = fdget(efd);
	if (!efile.file) {
		ret = -EBADF;
		goto out_kfree;
	}

	event->eventfd = eventfd_ctx_fileget(efile.file);
	if (IS_ERR(event->eventfd)) {
		ret = PTR_ERR(event->eventfd);
		goto out_put_efile;
	}

	cfile = fdget(cfd);
	if (!cfile.file) {
		ret = -EBADF;
		goto out_put_eventfd;
	}

	/* the process need read permission on control file */
	/* AV: shouldn't we check that it's been opened for read instead? */
	ret = inode_permission(file_inode(cfile.file), MAY_READ);
	if (ret < 0)
		goto out_put_cfile;

	/*
	 * Determine the event callbacks and set them in @event.  This used
	 * to be done via struct cftype but cgroup core no longer knows
	 * about these events.  The following is crude but the whole thing
	 * is for compatibility anyway.
	 *
	 * DO NOT ADD NEW FILES.
	 */
	name = cfile.file->f_path.dentry->d_name.name;

	if (!strcmp(name, "memory.usage_in_bytes")) {
		event->register_event = mem_cgroup_usage_register_event;
		event->unregister_event = mem_cgroup_usage_unregister_event;
	} else if (!strcmp(name, "memory.oom_control")) {
		event->register_event = mem_cgroup_oom_register_event;
		event->unregister_event = mem_cgroup_oom_unregister_event;
	} else if (!strcmp(name, "memory.pressure_level")) {
		event->register_event = vmpressure_register_event;
		event->unregister_event = vmpressure_unregister_event;
	} else if (!strcmp(name, "memory.memsw.usage_in_bytes")) {
		event->register_event = memsw_cgroup_usage_register_event;
		event->unregister_event = memsw_cgroup_usage_unregister_event;
	} else {
		ret = -EINVAL;
		goto out_put_cfile;
	}

	/*
	 * Verify @cfile should belong to @css.  Also, remaining events are
	 * automatically removed on cgroup destruction but the removal is
	 * asynchronous, so take an extra ref on @css.
	 */
	cfile_css = css_tryget_online_from_dir(cfile.file->f_path.dentry->d_parent,
					       &memory_cgrp_subsys);
	ret = -EINVAL;
	if (IS_ERR(cfile_css))
		goto out_put_cfile;
	if (cfile_css != css) {
		css_put(cfile_css);
		goto out_put_cfile;
	}

	ret = event->register_event(memcg, event->eventfd, buf);
	if (ret)
		goto out_put_css;

	vfs_poll(efile.file, &event->pt);

	spin_lock(&memcg->event_list_lock);
	list_add(&event->list, &memcg->event_list);
	spin_unlock(&memcg->event_list_lock);

	fdput(cfile);
	fdput(efile);

	return nbytes;

out_put_css:
	css_put(css);
out_put_cfile:
	fdput(cfile);
out_put_eventfd:
	eventfd_ctx_put(event->eventfd);
out_put_efile:
	fdput(efile);
out_kfree:
	kfree(event);

	return ret;
}

static int memory_min_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long min = READ_ONCE(memcg->memory.min);

	if (min == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)min * PAGE_SIZE);

	return 0;
}

static ssize_t memory_min_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long min;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &min);
	if (err)
		return err;

	page_counter_set_min(&memcg->memory, min);

	return nbytes;
}

static int memory_low_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long low = READ_ONCE(memcg->memory.low);

	if (low == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)low * PAGE_SIZE);

	return 0;
}

static ssize_t memory_low_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long low;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &low);
	if (err)
		return err;

	page_counter_set_low(&memcg->memory, low);

	return nbytes;
}

static int memory_high_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long high = READ_ONCE(memcg->memory.high);

	if (high == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)high * PAGE_SIZE);

	return 0;
}

static ssize_t memory_high_write(struct kernfs_open_file *of,
				 char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int nr_retries = MAX_RECLAIM_RETRIES;
	bool drained = false;
	unsigned long high;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &high);
	if (err)
		return err;

	for (;;) {
		unsigned long nr_pages = page_counter_read(&memcg->memory);
		unsigned long reclaimed;

		if (nr_pages <= high)
			break;

		if (signal_pending(current))
			break;

		if (!drained) {
			drain_all_stock(memcg);
			drained = true;
			continue;
		}

		reclaimed = try_to_free_mem_cgroup_pages(memcg, nr_pages - high,
							 GFP_KERNEL, true);

		if (!reclaimed && !nr_retries--)
			break;
	}

	page_counter_set_high(&memcg->memory, high);

	setup_memcg_wmark(memcg);

	if (!is_wmark_ok(memcg, true))
		queue_work(memcg_wmark_wq, &memcg->wmark_work);

	memcg_wb_domain_size_changed(memcg);

	return nbytes;
}

static void __memory_events_show(struct seq_file *m, atomic_long_t *events)
{
	seq_printf(m, "low %lu\n", atomic_long_read(&events[MEMCG_LOW]));
	seq_printf(m, "high %lu\n", atomic_long_read(&events[MEMCG_HIGH]));
	seq_printf(m, "max %lu\n", atomic_long_read(&events[MEMCG_MAX]));
	seq_printf(m, "oom %lu\n", atomic_long_read(&events[MEMCG_OOM]));
	seq_printf(m, "oom_kill %lu\n",
		   atomic_long_read(&events[MEMCG_OOM_KILL]));
}

static int memory_events_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	__memory_events_show(m, memcg->memory_events);
	return 0;
}

static int memory_events_local_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	__memory_events_show(m, memcg->memory_events_local);
	return 0;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static int memcg_thp_reclaim_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	int thp_reclaim = READ_ONCE(memcg->thp_reclaim);

	if (thp_reclaim == THP_RECLAIM_ZSR)
		seq_puts(m, "[reclaim] swap disable\n");
	else if (memcg->thp_reclaim == THP_RECLAIM_SWAP)
		seq_puts(m, "reclaim [swap] disable\n");
	else
		seq_puts(m, "reclaim swap [disable]\n");

	return 0;
}

static ssize_t memcg_thp_reclaim_write(struct kernfs_open_file *of, char *buf,
				      size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));

	buf = strstrip(buf);
	if (!strcmp(buf, "reclaim"))
		WRITE_ONCE(memcg->thp_reclaim,
			   THP_RECLAIM_ZSR);
	else if (!strcmp(buf, "swap"))
		WRITE_ONCE(memcg->thp_reclaim, THP_RECLAIM_SWAP);
	else if (!strcmp(buf, "disable"))
		WRITE_ONCE(memcg->thp_reclaim, THP_RECLAIM_DISABLE);
	else
		return -EINVAL;

	return nbytes;
}

static int memcg_thp_reclaim_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct mem_cgroup_per_node *mz;
	int node;
	unsigned long len;

	seq_puts(m, "queue_length\t");
	for_each_node(node) {
		mz = mem_cgroup_nodeinfo(memcg, node);
		len = READ_ONCE(mz->hugepage_reclaim_queue.reclaim_queue_len);
		seq_printf(m, "%-24lu", len);
	}

	seq_puts(m, "\n");
	seq_puts(m, "split_hugepage\t");
	for_each_node(node) {
		mz = mem_cgroup_nodeinfo(memcg, node);
		len = atomic_long_read(&mz->hugepage_reclaim_queue.split_hugepage);
		seq_printf(m, "%-24lu", len);
	}

	seq_puts(m, "\n");
	seq_puts(m, "reclaim_subpage\t");
	for_each_node(node) {
		mz = mem_cgroup_nodeinfo(memcg, node);
		len = atomic_long_read(&mz->hugepage_reclaim_queue.reclaim_subpage);
		seq_printf(m, "%-24lu", len);
	}
	seq_puts(m, "\n");

	return 0;
}

static inline char *strsep_s(char **s, const char *ct)
{
	char *p;

	while ((p = strsep(s, ct))) {
		if (*p)
			return p;
	}
	return NULL;
}

static int memcg_thp_reclaim_ctrl_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	int thp_reclaim_threshold = READ_ONCE(memcg->thp_reclaim_threshold);

	seq_printf(m, "threshold\t%d\n", thp_reclaim_threshold);

	return 0;
}

#define CTRL_RECLAIM_MEMCG 1 /* only relciam current memcg*/
#define CTRL_RECLAIM_ALL   2 /* reclaim current memcg and all the child memcg */
static ssize_t memcg_thp_reclaim_ctrl_write(struct kernfs_open_file *of,
					char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret, threshold, mode;
	char *key, *value;

	key = strsep_s(&buf, " \t\n");
	if (!key)
		return -EINVAL;

	if (!strcmp(key, "threshold")) {
		value = strsep_s(&buf, " \t\n");
		if (!value)
			return -EINVAL;

		ret = kstrtouint(value, 0, &threshold);
		if (ret)
			return ret;

		if (threshold > HPAGE_PMD_NR || threshold < 1)
			return -EINVAL;

		xchg(&memcg->thp_reclaim_threshold, threshold);
	} else if (!strcmp(key, "reclaim")) {
		struct mem_cgroup *iter;

		value = strsep_s(&buf, " \t\n");
		if (!value)
			return -EINVAL;

		ret = kstrtouint(value, 0, &mode);
		if (ret)
			return ret;

		switch (mode) {
		case CTRL_RECLAIM_MEMCG:
			reclaim_memcg_huge_pages(memcg);
			break;
		case CTRL_RECLAIM_ALL:
			iter = mem_cgroup_iter(memcg, NULL, NULL);
			do {
				reclaim_memcg_huge_pages(iter);
			} while ((iter = mem_cgroup_iter(memcg, iter, NULL)));
			break;
		default:
			return -EINVAL;
		}
	} else
		return -EINVAL;

	return nbytes;
}
#endif

static struct cftype mem_cgroup_legacy_files[] = {
	{
		.name = "usage_in_bytes",
		.private = MEMFILE_PRIVATE(_MEM, RES_USAGE),
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "max_usage_in_bytes",
		.private = MEMFILE_PRIVATE(_MEM, RES_MAX_USAGE),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "limit_in_bytes",
		.private = MEMFILE_PRIVATE(_MEM, RES_LIMIT),
		.write = mem_cgroup_write,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "soft_limit_in_bytes",
		.private = MEMFILE_PRIVATE(_MEM, RES_SOFT_LIMIT),
		.write = mem_cgroup_write,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "failcnt",
		.private = MEMFILE_PRIVATE(_MEM, RES_FAILCNT),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "stat",
		.seq_show = memcg_stat_show,
	},
	{
		.name = "exstat",
		.seq_show = memcg_exstat_show,
	},
	{
		.name = "wmark_ratio",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_ratio_show,
		.write = memory_wmark_ratio_write,
	},
	{
		.name = "wmark_high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = MEMFILE_PRIVATE(_MEM, WMARK_HIGH_LIMIT),
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "wmark_low",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = MEMFILE_PRIVATE(_MEM, WMARK_LOW_LIMIT),
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "wmark_scale_factor",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_scale_factor_show,
		.write = memory_wmark_scale_factor_write,
	},
	{
		.name = "wmark_min_adj",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_min_adj_show,
		.write = memory_wmark_min_adj_write,
	},
#ifdef CONFIG_MEMSLI
	{
		.name = "direct_reclaim_global_latency",
		.private = MEM_LAT_GLOBAL_DIRECT_RECLAIM,
		.write_u64 = memcg_lat_stat_write,
		.seq_show =  memcg_lat_stat_show,
	},
	{
		.name = "direct_reclaim_memcg_latency",
		.private = MEM_LAT_MEMCG_DIRECT_RECLAIM,
		.write_u64 = memcg_lat_stat_write,
		.seq_show =  memcg_lat_stat_show,
	},
	{
		.name = "direct_compact_latency",
		.private = MEM_LAT_DIRECT_COMPACT,
		.write_u64 = memcg_lat_stat_write,
		.seq_show =  memcg_lat_stat_show,
	},
	{
		.name = "direct_swapout_global_latency",
		.private = MEM_LAT_GLOBAL_DIRECT_SWAPOUT,
		.write_u64 = memcg_lat_stat_write,
		.seq_show =  memcg_lat_stat_show,
	},
	{
		.name = "direct_swapout_memcg_latency",
		.private = MEM_LAT_MEMCG_DIRECT_SWAPOUT,
		.write_u64 = memcg_lat_stat_write,
		.seq_show =  memcg_lat_stat_show,
	},
	{
		.name = "direct_swapin_latency",
		.private = MEM_LAT_DIRECT_SWAPIN,
		.write_u64 = memcg_lat_stat_write,
		.seq_show =  memcg_lat_stat_show,
	},
#endif /* CONFIG_MEMSLI */
	{
		.name = "force_empty",
		.write = mem_cgroup_force_empty_write,
	},
	{
		.name = "use_hierarchy",
		.write_u64 = mem_cgroup_hierarchy_write,
		.read_u64 = mem_cgroup_hierarchy_read,
	},
	{
		.name = "use_priority_oom",
		.write_u64 = mem_cgroup_priority_oom_write,
		.read_u64 = mem_cgroup_priority_oom_read,
	},
	{
		.name = "cgroup.event_control",		/* XXX: for compat */
		.write = memcg_write_event_control,
		.flags = CFTYPE_NO_PREFIX | CFTYPE_WORLD_WRITABLE,
	},
	{
		.name = "swappiness",
		.read_s64 = mem_cgroup_swappiness_read,
		.write_s64 = mem_cgroup_swappiness_write,
	},
	{
		.name = "priority",
		.read_u64 = mem_cgroup_priority_read,
		.write_u64 = mem_cgroup_priority_write,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "move_charge_at_immigrate",
		.read_u64 = mem_cgroup_move_charge_read,
		.write_u64 = mem_cgroup_move_charge_write,
	},
	{
		.name = "oom_control",
		.seq_show = mem_cgroup_oom_control_read,
		.write_u64 = mem_cgroup_oom_control_write,
		.private = MEMFILE_PRIVATE(_OOM_TYPE, OOM_CONTROL),
	},
	{
		.name = "oom.group",
		.flags = CFTYPE_NOT_ON_ROOT | CFTYPE_NS_DELEGATABLE,
		.seq_show = memory_oom_group_show,
		.write = memory_oom_group_write,
	},
	{
		.name = "pressure_level",
	},
#ifdef CONFIG_NUMA
	{
		.name = "numa_stat",
		.seq_show = memcg_numa_stat_show,
	},
#endif
	{
		.name = "kmem.limit_in_bytes",
		.private = MEMFILE_PRIVATE(_KMEM, RES_LIMIT),
		.write = mem_cgroup_write,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "kmem.usage_in_bytes",
		.private = MEMFILE_PRIVATE(_KMEM, RES_USAGE),
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "kmem.failcnt",
		.private = MEMFILE_PRIVATE(_KMEM, RES_FAILCNT),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "kmem.max_usage_in_bytes",
		.private = MEMFILE_PRIVATE(_KMEM, RES_MAX_USAGE),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
#if defined(CONFIG_SLAB) || defined(CONFIG_SLUB_DEBUG)
	{
		.name = "kmem.slabinfo",
		.seq_start = memcg_slab_start,
		.seq_next = memcg_slab_next,
		.seq_stop = memcg_slab_stop,
		.seq_show = memcg_slab_show,
	},
#endif
	{
		.name = "kmem.tcp.limit_in_bytes",
		.private = MEMFILE_PRIVATE(_TCP, RES_LIMIT),
		.write = mem_cgroup_write,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "kmem.tcp.usage_in_bytes",
		.private = MEMFILE_PRIVATE(_TCP, RES_USAGE),
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "kmem.tcp.failcnt",
		.private = MEMFILE_PRIVATE(_TCP, RES_FAILCNT),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "kmem.tcp.max_usage_in_bytes",
		.private = MEMFILE_PRIVATE(_TCP, RES_MAX_USAGE),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
#ifdef CONFIG_KIDLED
	{
		.name = "idle_page_stats",
		.seq_show = mem_cgroup_idle_page_stats_show,
		.write = mem_cgroup_idle_page_stats_write,
	},
#endif
	{
		.name = "min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_min_show,
		.write = memory_min_write,
	},
	{
		.name = "low",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_low_show,
		.write = memory_low_write,
	},
	{
		.name = "high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_high_show,
		.write = memory_high_write,
	},
	{
		.name = "events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, events_file),
		.seq_show = memory_events_show,
	},
	{
		.name = "events.local",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, events_local_file),
		.seq_show = memory_events_local_show,
	},
	{
		.name = "swap.high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = swap_high_show,
		.write = swap_high_write,
	},
	{
		.name = "swap.events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, swap_events_file),
		.seq_show = swap_events_show,
	},
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	{
		.name = "thp_reclaim",
		.seq_show = memcg_thp_reclaim_show,
		.write = memcg_thp_reclaim_write,
	},
	{
		.name = "thp_reclaim_stat",
		.seq_show = memcg_thp_reclaim_stat_show,
	},
	{
		.name = "thp_reclaim_ctrl",
		.seq_show = memcg_thp_reclaim_ctrl_show,
		.write = memcg_thp_reclaim_ctrl_write,
	},
#endif
	{ },	/* terminate */
};

/*
 * Private memory cgroup IDR
 *
 * Swap-out records and page cache shadow entries need to store memcg
 * references in constrained space, so we maintain an ID space that is
 * limited to 16 bit (MEM_CGROUP_ID_MAX), limiting the total number of
 * memory-controlled cgroups to 64k.
 *
 * However, there usually are many references to the oflline CSS after
 * the cgroup has been destroyed, such as page cache or reclaimable
 * slab objects, that don't need to hang on to the ID. We want to keep
 * those dead CSS from occupying IDs, or we might quickly exhaust the
 * relatively small ID space and prevent the creation of new cgroups
 * even when there are much fewer than 64k cgroups - possibly none.
 *
 * Maintain a private 16-bit ID space for memcg, and allow the ID to
 * be freed and recycled when it's no longer needed, which is usually
 * when the CSS is offlined.
 *
 * The only exception to that are records of swapped out tmpfs/shmem
 * pages that need to be attributed to live ancestors on swapin. But
 * those references are manageable from userspace.
 */

static DEFINE_IDR(mem_cgroup_idr);

static void mem_cgroup_id_remove(struct mem_cgroup *memcg)
{
	if (memcg->id.id > 0) {
		idr_remove(&mem_cgroup_idr, memcg->id.id);
		memcg->id.id = 0;
	}
}

static void mem_cgroup_id_get_many(struct mem_cgroup *memcg, unsigned int n)
{
	VM_BUG_ON(atomic_read(&memcg->id.ref) <= 0);
	atomic_add(n, &memcg->id.ref);
}

static void mem_cgroup_id_put_many(struct mem_cgroup *memcg, unsigned int n)
{
	VM_BUG_ON(atomic_read(&memcg->id.ref) < n);
	if (atomic_sub_and_test(n, &memcg->id.ref)) {
		mem_cgroup_id_remove(memcg);

		/* Memcg ID pins CSS */
		css_put(&memcg->css);
	}
}

static inline void mem_cgroup_id_get(struct mem_cgroup *memcg)
{
	mem_cgroup_id_get_many(memcg, 1);
}

static inline void mem_cgroup_id_put(struct mem_cgroup *memcg)
{
	mem_cgroup_id_put_many(memcg, 1);
}

/**
 * mem_cgroup_from_id - look up a memcg from a memcg id
 * @id: the memcg id to look up
 *
 * Caller must hold rcu_read_lock().
 */
struct mem_cgroup *mem_cgroup_from_id(unsigned short id)
{
	WARN_ON_ONCE(!rcu_read_lock_held());
	return idr_find(&mem_cgroup_idr, id);
}

static void mem_cgroup_per_node_clean_up(struct mem_cgroup *memcg, int node)
{
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[node];
	struct lruvec_stat __percpu *lruvec_stat_local = pn->lruvec_stat_local;
	struct lruvec_stat __percpu *lruvec_stat_cpu = pn->lruvec_stat_cpu;
	int i;

	for_each_possible_cpu(i) {
		memset(per_cpu_ptr(lruvec_stat_local, i), 0,
				sizeof(*lruvec_stat_local));
		memset(per_cpu_ptr(lruvec_stat_cpu, i), 0,
				sizeof(*lruvec_stat_cpu));
	}

	memset(pn, 0, sizeof(*pn));

	pn->lruvec_stat_local = lruvec_stat_local;
	pn->lruvec_stat_cpu = lruvec_stat_cpu;
}

static void mem_cgroup_clean_up(void **ptr)
{
	struct mem_cgroup *memcg = *ptr;
	struct memcg_vmstats_percpu __percpu *vmstats_percpu = memcg->vmstats_percpu;
	struct memcg_vmstats_percpu __percpu *vmstats_local = memcg->vmstats_local;
	struct mem_cgroup_exstat_cpu __percpu *exstat_cpu = memcg->exstat_cpu;
	struct mem_cgroup_lat_stat_cpu __percpu *lat_stat_cpu = memcg->lat_stat_cpu;
	int i;
	int node;

	for_each_possible_cpu(i) {
		memset(per_cpu_ptr(vmstats_percpu, i), 0, sizeof(*vmstats_percpu));
		memset(per_cpu_ptr(vmstats_local, i), 0, sizeof(*vmstats_local));
		memset(per_cpu_ptr(exstat_cpu, i), 0, sizeof(*exstat_cpu));
		memset(per_cpu_ptr(lat_stat_cpu, i), 0, sizeof(*lat_stat_cpu));
	}

	memset(memcg, 0, offsetof(struct mem_cgroup, nodeinfo));

	memcg->vmstats_percpu = vmstats_percpu;
	memcg->vmstats_local = vmstats_local;
	memcg->exstat_cpu = exstat_cpu;
	memcg->lat_stat_cpu = lat_stat_cpu;

	for_each_node(node)
		mem_cgroup_per_node_clean_up(memcg, node);

}

static bool mem_cgroup_check_integrity(struct mem_cgroup *memcg)
{
	int node;

	if (!memcg->exstat_cpu || !memcg->vmstats_percpu ||
			!memcg->vmstats_local || !memcg->lat_stat_cpu)
		return false;

	for_each_node(node)
		if (!memcg->nodeinfo[node])
			return false;

	return true;
}

static void mem_cgroup_per_node_info_init(struct mem_cgroup *memcg, int node)
{
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[node];

	lruvec_init(&pn->lruvec);
	pn->usage_in_excess = 0;
	pn->on_tree = false;
	pn->memcg = memcg;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	spin_lock_init(&pn->deferred_split_queue.split_queue_lock);
	INIT_LIST_HEAD(&pn->deferred_split_queue.split_queue);
	pn->deferred_split_queue.split_queue_len = 0;

	spin_lock_init(&pn->hugepage_reclaim_queue.reclaim_queue_lock);
	INIT_LIST_HEAD(&pn->hugepage_reclaim_queue.reclaim_queue);
	pn->hugepage_reclaim_queue.reclaim_queue_len = 0;
#endif
}

static bool __mem_cgroup_init(struct mem_cgroup *memcg)
{
	int node;

	if (memcg_wb_domain_init(memcg, GFP_KERNEL))
		return false;

	for_each_node(node)
		mem_cgroup_per_node_info_init(memcg, node);

	INIT_WORK(&memcg->high_work, high_work_func);
	INIT_WORK(&memcg->wmark_work, wmark_work_func);
	memcg->last_scanned_node = MAX_NUMNODES;
	INIT_LIST_HEAD(&memcg->oom_notify);
	mutex_init(&memcg->thresholds_lock);
	spin_lock_init(&memcg->move_lock);
	vmpressure_init(&memcg->vmpressure);
	INIT_LIST_HEAD(&memcg->event_list);
	spin_lock_init(&memcg->event_list_lock);
	memcg->socket_pressure = jiffies;
#ifdef CONFIG_MEMCG_KMEM
	memcg->kmemcg_id = -1;
#endif
#ifdef CONFIG_CGROUP_WRITEBACK
	INIT_LIST_HEAD(&memcg->cgwb_list);
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	memcg->thp_reclaim = THP_RECLAIM_DISABLE;
	memcg->thp_reclaim_threshold = THP_RECLAIM_THRESHOLD_DEFAULT;
#endif
	kidled_memcg_init(memcg);
	idr_replace(&mem_cgroup_idr, memcg, memcg->id.id);

	return true;
}

static int alloc_mem_cgroup_per_node_info(struct mem_cgroup *memcg, int node)
{
	struct mem_cgroup_per_node *pn;
	int tmp = node;
	/*
	 * This routine is called against possible nodes.
	 * But it's BUG to call kmalloc() against offline node.
	 *
	 * TODO: this routine can waste much memory for nodes which will
	 *       never be onlined. It's better to use memory hotplug callback
	 *       function.
	 */
	if (!node_state(node, N_NORMAL_MEMORY))
		tmp = -1;
	pn = kzalloc_node(sizeof(*pn), GFP_KERNEL, tmp);
	if (!pn)
		return 1;

	pn->lruvec_stat_local = alloc_percpu(struct lruvec_stat);
	if (!pn->lruvec_stat_local) {
		kfree(pn);
		return 1;
	}

	pn->lruvec_stat_cpu = alloc_percpu(struct lruvec_stat);
	if (!pn->lruvec_stat_cpu) {
		free_percpu(pn->lruvec_stat_local);
		kfree(pn);
		return 1;
	}

	memcg->nodeinfo[node] = pn;
	return 0;
}

static void free_mem_cgroup_per_node_info(struct mem_cgroup *memcg, int node)
{
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[node];

	if (!pn)
		return;

	free_percpu(pn->lruvec_stat_cpu);
	free_percpu(pn->lruvec_stat_local);
	kfree(pn);
}

static void __mem_cgroup_free(void **ptr)
{
	struct mem_cgroup *memcg = *ptr;
	int node;

	for_each_node(node)
		free_mem_cgroup_per_node_info(memcg, node);
	free_percpu(memcg->vmstats_percpu);
	free_percpu(memcg->vmstats_local);
	free_percpu(memcg->exstat_cpu);
	free_percpu(memcg->lat_stat_cpu);
	kfree(memcg);
}

CACHE_HEADER(mem_cgroup_cache_header, DEFAULT_CACHE_SIZE,
		mem_cgroup_clean_up, __mem_cgroup_free);

static void mem_cgroup_free(struct mem_cgroup *memcg)
{
	memcg_wb_domain_exit(memcg);
	/*
	 * Flush percpu vmstats and vmevents to guarantee the value correctness
	 * on parent's and all ancestor levels.
	 */
	memcg_flush_percpu_vmstats(memcg);
	memcg_flush_percpu_vmevents(memcg);

	if (mem_cgroup_check_integrity(memcg))
		if (put_to_cache(&mem_cgroup_cache_header, (void **)&memcg, 1))
			return;

	__mem_cgroup_free((void **)&memcg);
}

static struct mem_cgroup *mem_cgroup_alloc(void)
{
	struct mem_cgroup *memcg;
	size_t size;
	int node;

	if (get_from_cache(&mem_cgroup_cache_header, (void **)&memcg, 1)) {
		if (__mem_cgroup_init(memcg))
			return memcg;
		__mem_cgroup_free((void **)&memcg);
	}

	size = sizeof(struct mem_cgroup);
	size += nr_node_ids * sizeof(struct mem_cgroup_per_node *);

	memcg = kzalloc(size, GFP_KERNEL);
	if (!memcg)
		return NULL;

	memcg->id.id = idr_alloc(&mem_cgroup_idr, NULL,
				 1, MEM_CGROUP_ID_MAX,
				 GFP_KERNEL);
	if (memcg->id.id < 0)
		goto fail;

	memcg->vmstats_local = alloc_percpu(struct memcg_vmstats_percpu);
	if (!memcg->vmstats_local)
		goto fail;

	memcg->vmstats_percpu = alloc_percpu(struct memcg_vmstats_percpu);
	if (!memcg->vmstats_percpu)
		goto fail;

	memcg->exstat_cpu = alloc_percpu(struct mem_cgroup_exstat_cpu);
	if (!memcg->exstat_cpu)
		goto fail;

	memcg->lat_stat_cpu = alloc_percpu(struct mem_cgroup_lat_stat_cpu);
	if (!memcg->lat_stat_cpu)
		goto fail;

	for_each_node(node)
		if (alloc_mem_cgroup_per_node_info(memcg, node))
			goto fail;

	if (!__mem_cgroup_init(memcg))
		goto fail;

	return memcg;
fail:
	mem_cgroup_id_remove(memcg);
	__mem_cgroup_free((void **)&memcg);
	return NULL;
}

static struct cgroup_subsys_state * __ref
mem_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct mem_cgroup *parent = mem_cgroup_from_css(parent_css);
	struct mem_cgroup *memcg;
	long error = -ENOMEM;

	memcg = mem_cgroup_alloc();
	if (!memcg)
		return ERR_PTR(error);

	page_counter_set_high(&memcg->memory, PAGE_COUNTER_MAX);
	memcg->soft_limit = PAGE_COUNTER_MAX;
	page_counter_set_high(&memcg->swap, PAGE_COUNTER_MAX);
	page_counter_set_high(&memcg->memsw, PAGE_COUNTER_MAX);
	if (parent) {
		memcg->swappiness = max(mem_cgroup_swappiness(parent), 0);
		memcg->oom_kill_disable = parent->oom_kill_disable;
		memcg->wmark_ratio = parent->wmark_ratio;
		/* Default gap is 0.5% max limit */
		memcg->wmark_scale_factor = parent->wmark_scale_factor ?
					    : 50;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		memcg->thp_reclaim = parent->thp_reclaim;
		memcg->thp_reclaim_threshold = parent->thp_reclaim_threshold;
#endif
		kidled_memcg_inherit_parent_buckets(parent, memcg);
	}
	if (parent && parent->use_hierarchy) {
		memcg->use_hierarchy = true;
		page_counter_init(&memcg->memory, &parent->memory);
		page_counter_init(&memcg->swap, &parent->swap);
		page_counter_init(&memcg->memsw, &parent->memsw);
		page_counter_init(&memcg->kmem, &parent->kmem);
		page_counter_init(&memcg->tcpmem, &parent->tcpmem);
	} else {
		page_counter_init(&memcg->memory, NULL);
		page_counter_init(&memcg->swap, NULL);
		page_counter_init(&memcg->memsw, NULL);
		page_counter_init(&memcg->kmem, NULL);
		page_counter_init(&memcg->tcpmem, NULL);
		/*
		 * Deeper hierachy with use_hierarchy == false doesn't make
		 * much sense so let cgroup subsystem know about this
		 * unfortunate state in our controller.
		 */
		if (parent != root_mem_cgroup)
			memory_cgrp_subsys.broken_hierarchy = true;
	}

	setup_memcg_wmark(memcg);

	if (parent) {
		memcg->wmark_min_adj = parent->wmark_min_adj;
		memcg->wmark_min_eadj = parent->wmark_min_eadj;
	}

	/* The following stuff does not apply to the root */
	if (!parent) {
		root_mem_cgroup = memcg;
		return &memcg->css;
	}

	error = memcg_online_kmem(memcg);
	if (error)
		goto fail;

	if (cgroup_subsys_on_dfl(memory_cgrp_subsys) && !cgroup_memory_nosocket)
		static_branch_inc(&memcg_sockets_enabled_key);

	return &memcg->css;
fail:
	mem_cgroup_id_remove(memcg);
	mem_cgroup_free(memcg);
	return ERR_PTR(-ENOMEM);
}

static int mem_cgroup_css_online(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	/*
	 * A memcg must be visible for memcg_expand_shrinker_maps()
	 * by the time the maps are allocated. So, we allocate maps
	 * here, when for_each_mem_cgroup() can't skip it.
	 */
	if (memcg_alloc_shrinker_maps(memcg)) {
		mem_cgroup_id_remove(memcg);
		return -ENOMEM;
	}

	/* Online state pins memcg ID, memcg ID pins CSS */
	atomic_set(&memcg->id.ref, 1);
	css_get(css);
	return 0;
}

static void mem_cgroup_css_offline(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mem_cgroup_event *event, *tmp;

	memcg->offline_jiffies = jiffies;

	/*
	 * Unregister events and notify userspace.
	 * Notify userspace about cgroup removing only after rmdir of cgroup
	 * directory to avoid race between userspace and kernelspace.
	 */
	spin_lock(&memcg->event_list_lock);
	list_for_each_entry_safe(event, tmp, &memcg->event_list, list) {
		list_del_init(&event->list);
		schedule_work(&event->remove);
	}
	spin_unlock(&memcg->event_list_lock);

	page_counter_set_min(&memcg->memory, 0);
	page_counter_set_low(&memcg->memory, 0);

	page_counter_set_wmark_low(&memcg->memory, PAGE_COUNTER_MAX);
	page_counter_set_wmark_high(&memcg->memory, PAGE_COUNTER_MAX);

	memcg_offline_kmem(memcg);
	wb_memcg_offline(memcg);

	mem_cgroup_id_put(memcg);
}

static void mem_cgroup_css_released(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	invalidate_reclaim_iterators(memcg);
}

static void mem_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	if (cgroup_subsys_on_dfl(memory_cgrp_subsys) && !cgroup_memory_nosocket)
		static_branch_dec(&memcg_sockets_enabled_key);

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && memcg->tcpmem_active)
		static_branch_dec(&memcg_sockets_enabled_key);

	vmpressure_cleanup(&memcg->vmpressure);
	cancel_work_sync(&memcg->high_work);
	cancel_work_sync(&memcg->wmark_work);
	mem_cgroup_remove_from_trees(memcg);
	memcg_free_shrinker_maps(memcg);
	memcg_free_kmem(memcg);
	mem_cgroup_free(memcg);
}

/**
 * mem_cgroup_css_reset - reset the states of a mem_cgroup
 * @css: the target css
 *
 * Reset the states of the mem_cgroup associated with @css.  This is
 * invoked when the userland requests disabling on the default hierarchy
 * but the memcg is pinned through dependency.  The memcg should stop
 * applying policies and should revert to the vanilla state as it may be
 * made visible again.
 *
 * The current implementation only resets the essential configurations.
 * This needs to be expanded to cover all the visible parts.
 */
static void mem_cgroup_css_reset(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	page_counter_set_max(&memcg->memory, PAGE_COUNTER_MAX);
	page_counter_set_max(&memcg->swap, PAGE_COUNTER_MAX);
	page_counter_set_max(&memcg->memsw, PAGE_COUNTER_MAX);
	page_counter_set_max(&memcg->kmem, PAGE_COUNTER_MAX);
	page_counter_set_max(&memcg->tcpmem, PAGE_COUNTER_MAX);
	page_counter_set_min(&memcg->memory, 0);
	page_counter_set_low(&memcg->memory, 0);
	page_counter_set_wmark_low(&memcg->memory, PAGE_COUNTER_MAX);
	page_counter_set_wmark_high(&memcg->memory, PAGE_COUNTER_MAX);
	page_counter_set_high(&memcg->memory, PAGE_COUNTER_MAX);
	memcg->soft_limit = PAGE_COUNTER_MAX;
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		page_counter_set_high(&memcg->swap, PAGE_COUNTER_MAX);
	else
		page_counter_set_high(&memcg->memsw, PAGE_COUNTER_MAX);
	memcg_wb_domain_size_changed(memcg);
}

#ifdef CONFIG_MMU
/* Handlers for move charge at task migration. */
static int mem_cgroup_do_precharge(unsigned long count)
{
	int ret;

	/* Try a single bulk charge without reclaim first, kswapd may wake */
	ret = try_charge(mc.to, GFP_KERNEL & ~__GFP_DIRECT_RECLAIM, count);
	if (!ret) {
		mc.precharge += count;
		return ret;
	}

	/* Try charges one by one with reclaim, but do not retry */
	while (count--) {
		ret = try_charge(mc.to, GFP_KERNEL | __GFP_NORETRY, 1);
		if (ret)
			return ret;
		mc.precharge++;
		cond_resched();
	}
	return 0;
}

union mc_target {
	struct page	*page;
	swp_entry_t	ent;
};

enum mc_target_type {
	MC_TARGET_NONE = 0,
	MC_TARGET_PAGE,
	MC_TARGET_SWAP,
	MC_TARGET_DEVICE,
};

static struct page *mc_handle_present_pte(struct vm_area_struct *vma,
						unsigned long addr, pte_t ptent)
{
	struct page *page = _vm_normal_page(vma, addr, ptent, true);

	if (!page || !page_mapped(page))
		return NULL;
	if (PageAnon(page)) {
		if (!(mc.flags & MOVE_ANON))
			return NULL;
	} else {
		if (!(mc.flags & MOVE_FILE))
			return NULL;
	}
	if (!get_page_unless_zero(page))
		return NULL;

	return page;
}

#if defined(CONFIG_SWAP) || defined(CONFIG_DEVICE_PRIVATE)
static struct page *mc_handle_swap_pte(struct vm_area_struct *vma,
			pte_t ptent, swp_entry_t *entry)
{
	struct page *page = NULL;
	swp_entry_t ent = pte_to_swp_entry(ptent);

	if (!(mc.flags & MOVE_ANON) || non_swap_entry(ent))
		return NULL;

	/*
	 * Handle MEMORY_DEVICE_PRIVATE which are ZONE_DEVICE page belonging to
	 * a device and because they are not accessible by CPU they are store
	 * as special swap entry in the CPU page table.
	 */
	if (is_device_private_entry(ent)) {
		page = device_private_entry_to_page(ent);
		/*
		 * MEMORY_DEVICE_PRIVATE means ZONE_DEVICE page and which have
		 * a refcount of 1 when free (unlike normal page)
		 */
		if (!page_ref_add_unless(page, 1, 1))
			return NULL;
		return page;
	}

	/*
	 * Because lookup_swap_cache() updates some statistics counter,
	 * we call find_get_page() with swapper_space directly.
	 */
	page = find_get_page(swap_address_space(ent), swp_offset(ent));
	entry->val = ent.val;

	return page;
}
#else
static struct page *mc_handle_swap_pte(struct vm_area_struct *vma,
			pte_t ptent, swp_entry_t *entry)
{
	return NULL;
}
#endif

static struct page *mc_handle_file_pte(struct vm_area_struct *vma,
			unsigned long addr, pte_t ptent, swp_entry_t *entry)
{
	struct page *page = NULL;
	struct address_space *mapping;
	pgoff_t pgoff;

	if (!vma->vm_file) /* anonymous vma */
		return NULL;
	if (!(mc.flags & MOVE_FILE))
		return NULL;

	mapping = vma->vm_file->f_mapping;
	pgoff = linear_page_index(vma, addr);

	/* page is moved even if it's not RSS of this task(page-faulted). */
#ifdef CONFIG_SWAP
	/* shmem/tmpfs may report page out on swap: account for that too. */
	if (shmem_mapping(mapping)) {
		page = find_get_entry(mapping, pgoff);
		if (radix_tree_exceptional_entry(page)) {
			swp_entry_t swp = radix_to_swp_entry(page);
			*entry = swp;
			page = find_get_page(swap_address_space(swp),
					     swp_offset(swp));
		}
	} else
		page = find_get_page(mapping, pgoff);
#else
	page = find_get_page(mapping, pgoff);
#endif
	return page;
}

/**
 * mem_cgroup_move_account - move account of the page
 * @page: the page
 * @compound: charge the page as compound or small page
 * @from: mem_cgroup which the page is moved from.
 * @to:	mem_cgroup which the page is moved to. @from != @to.
 *
 * The caller must make sure the page is not on LRU (isolate_page() is useful.)
 *
 * This function doesn't do "charge" to new cgroup and doesn't do "uncharge"
 * from old cgroup.
 */
static int mem_cgroup_move_account(struct page *page,
				   bool compound,
				   struct mem_cgroup *from,
				   struct mem_cgroup *to)
{
	struct lruvec *from_vec, *to_vec;
	struct pglist_data *pgdat;
	unsigned int nr_pages = compound ? hpage_nr_pages(page) : 1;
	int ret;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	bool del_hugepage_queue;
#endif

	VM_BUG_ON(from == to);
	VM_BUG_ON_PAGE(PageLRU(page), page);
	VM_BUG_ON(compound && !PageTransHuge(page));

	/*
	 * Prevent mem_cgroup_migrate() from looking at
	 * page->mem_cgroup of its source page while we change it.
	 */
	ret = -EBUSY;
	if (!trylock_page(page))
		goto out;

	ret = -EINVAL;
	if (page->mem_cgroup != from)
		goto out_unlock;

	pgdat = page_pgdat(page);
	from_vec = mem_cgroup_lruvec(from, pgdat);
	to_vec = mem_cgroup_lruvec(to, pgdat);

	lock_page_memcg(page);

	if (PageAnon(page)) {
		if (page_mapped(page)) {
			__mod_lruvec_state(from_vec, NR_ANON_MAPPED, -nr_pages);
			__mod_lruvec_state(to_vec, NR_ANON_MAPPED, nr_pages);
			if (PageTransHuge(page)) {
				__mod_lruvec_state(from_vec, NR_ANON_THPS,
						   -nr_pages);
				__mod_lruvec_state(to_vec, NR_ANON_THPS,
						   nr_pages);
			}

		}
	} else {
		__mod_lruvec_state(from_vec, NR_FILE_PAGES, -nr_pages);
		__mod_lruvec_state(to_vec, NR_FILE_PAGES, nr_pages);

		if (PageSwapBacked(page)) {
			__mod_lruvec_state(from_vec, NR_SHMEM, -nr_pages);
			__mod_lruvec_state(to_vec, NR_SHMEM, nr_pages);
		}

		if (page_mapped(page)) {
			__mod_lruvec_state(from_vec, NR_FILE_MAPPED, -nr_pages);
			__mod_lruvec_state(to_vec, NR_FILE_MAPPED, nr_pages);
		}

		if (PageDirty(page)) {
			struct address_space *mapping = page_mapping(page);

			if (mapping_cap_account_dirty(mapping)) {
				__mod_lruvec_state(from_vec, NR_FILE_DIRTY,
						   -nr_pages);
				__mod_lruvec_state(to_vec, NR_FILE_DIRTY,
						   nr_pages);
			}
		}
	}

	if (PageWriteback(page)) {
		__mod_lruvec_state(from_vec, NR_WRITEBACK, -nr_pages);
		__mod_lruvec_state(to_vec, NR_WRITEBACK, nr_pages);
	}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	del_hugepage_queue = del_hugepage_from_queue(page);
#endif

	/*
	 * All state has been migrated, let's switch to the new memcg.
	 *
	 * It is safe to change page->mem_cgroup here because the page
	 * is referenced, charged, isolated, and locked: we can't race
	 * with (un)charging, migration, LRU putback, or anything else
	 * that would rely on a stable page->mem_cgroup.
	 *
	 * Note that lock_page_memcg is a memcg lock, not a page lock,
	 * to save space. As soon as we switch page->mem_cgroup to a
	 * new memcg that isn't locked, the above state can change
	 * concurrently again. Make sure we're truly done with it.
	 */
	smp_mb();

	page->mem_cgroup = to; 	/* caller should have done css_get */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (del_hugepage_queue)
		add_hugepage_to_queue(page);
#endif
	__unlock_page_memcg(from);

	ret = 0;

	kidled_mem_cgroup_move_stats(from, to, page, nr_pages);

	local_irq_disable();
	mem_cgroup_charge_statistics(to, page, nr_pages);
	memcg_check_events(to, page);
	mem_cgroup_charge_statistics(from, page, -nr_pages);
	memcg_check_events(from, page);
	local_irq_enable();
out_unlock:
	unlock_page(page);
out:
	return ret;
}

/**
 * get_mctgt_type - get target type of moving charge
 * @vma: the vma the pte to be checked belongs
 * @addr: the address corresponding to the pte to be checked
 * @ptent: the pte to be checked
 * @target: the pointer the target page or swap ent will be stored(can be NULL)
 *
 * Returns
 *   0(MC_TARGET_NONE): if the pte is not a target for move charge.
 *   1(MC_TARGET_PAGE): if the page corresponding to this pte is a target for
 *     move charge. if @target is not NULL, the page is stored in target->page
 *     with extra refcnt got(Callers should handle it).
 *   2(MC_TARGET_SWAP): if the swap entry corresponding to this pte is a
 *     target for charge migration. if @target is not NULL, the entry is stored
 *     in target->ent.
 *   3(MC_TARGET_DEVICE): like MC_TARGET_PAGE  but page is MEMORY_DEVICE_PUBLIC
 *     or MEMORY_DEVICE_PRIVATE (so ZONE_DEVICE page and thus not on the lru).
 *     For now we such page is charge like a regular page would be as for all
 *     intent and purposes it is just special memory taking the place of a
 *     regular page.
 *
 *     See Documentations/vm/hmm.txt and include/linux/hmm.h
 *
 * Called with pte lock held.
 */

static enum mc_target_type get_mctgt_type(struct vm_area_struct *vma,
		unsigned long addr, pte_t ptent, union mc_target *target)
{
	struct page *page = NULL;
	enum mc_target_type ret = MC_TARGET_NONE;
	swp_entry_t ent = { .val = 0 };

	if (pte_present(ptent))
		page = mc_handle_present_pte(vma, addr, ptent);
	else if (is_swap_pte(ptent))
		page = mc_handle_swap_pte(vma, ptent, &ent);
	else if (pte_none(ptent))
		page = mc_handle_file_pte(vma, addr, ptent, &ent);

	if (!page && !ent.val)
		return ret;
	if (page) {
		/*
		 * Do only loose check w/o serialization.
		 * mem_cgroup_move_account() checks the page is valid or
		 * not under LRU exclusion.
		 */
		if (page->mem_cgroup == mc.from) {
			ret = MC_TARGET_PAGE;
			if (is_device_private_page(page) ||
			    is_device_public_page(page))
				ret = MC_TARGET_DEVICE;
			if (target)
				target->page = page;
		}
		if (!ret || !target)
			put_page(page);
	}
	/*
	 * There is a swap entry and a page doesn't exist or isn't charged.
	 * But we cannot move a tail-page in a THP.
	 */
	if (ent.val && !ret && (!page || !PageTransCompound(page)) &&
	    mem_cgroup_id(mc.from) == lookup_swap_cgroup_id(ent)) {
		ret = MC_TARGET_SWAP;
		if (target)
			target->ent = ent;
	}
	return ret;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * We don't consider PMD mapped swapping or file mapped pages because THP does
 * not support them for now.
 * Caller should make sure that pmd_trans_huge(pmd) is true.
 */
static enum mc_target_type get_mctgt_type_thp(struct vm_area_struct *vma,
		unsigned long addr, pmd_t pmd, union mc_target *target)
{
	struct page *page = NULL;
	enum mc_target_type ret = MC_TARGET_NONE;

	if (unlikely(is_swap_pmd(pmd))) {
		VM_BUG_ON(thp_migration_supported() &&
				  !is_pmd_migration_entry(pmd));
		return ret;
	}
	page = pmd_page(pmd);
	VM_BUG_ON_PAGE(!page || !PageHead(page), page);
	if (!(mc.flags & MOVE_ANON))
		return ret;
	if (page->mem_cgroup == mc.from) {
		ret = MC_TARGET_PAGE;
		if (target) {
			get_page(page);
			target->page = page;
		}
	}
	return ret;
}
#else
static inline enum mc_target_type get_mctgt_type_thp(struct vm_area_struct *vma,
		unsigned long addr, pmd_t pmd, union mc_target *target)
{
	return MC_TARGET_NONE;
}
#endif

static int mem_cgroup_count_precharge_pte_range(pmd_t *pmd,
					unsigned long addr, unsigned long end,
					struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	pte_t *pte;
	spinlock_t *ptl;

	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		/*
		 * Note their can not be MC_TARGET_DEVICE for now as we do not
		 * support transparent huge page with MEMORY_DEVICE_PUBLIC or
		 * MEMORY_DEVICE_PRIVATE but this might change.
		 */
		if (get_mctgt_type_thp(vma, addr, *pmd, NULL) == MC_TARGET_PAGE)
			mc.precharge += HPAGE_PMD_NR;
		spin_unlock(ptl);
		return 0;
	}

	if (pmd_trans_unstable(pmd))
		return 0;
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	for (; addr != end; pte++, addr += PAGE_SIZE)
		if (get_mctgt_type(vma, addr, *pte, NULL))
			mc.precharge++;	/* increment precharge temporarily */
	pte_unmap_unlock(pte - 1, ptl);
	cond_resched();

	return 0;
}

static unsigned long mem_cgroup_count_precharge(struct mm_struct *mm)
{
	unsigned long precharge;

	struct mm_walk mem_cgroup_count_precharge_walk = {
		.pmd_entry = mem_cgroup_count_precharge_pte_range,
		.mm = mm,
	};
	down_read(&mm->mmap_sem);
	walk_page_range(0, mm->highest_vm_end,
			&mem_cgroup_count_precharge_walk);
	up_read(&mm->mmap_sem);

	precharge = mc.precharge;
	mc.precharge = 0;

	return precharge;
}

static int mem_cgroup_precharge_mc(struct mm_struct *mm)
{
	unsigned long precharge = mem_cgroup_count_precharge(mm);

	VM_BUG_ON(mc.moving_task);
	mc.moving_task = current;
	return mem_cgroup_do_precharge(precharge);
}

/* cancels all extra charges on mc.from and mc.to, and wakes up all waiters. */
static void __mem_cgroup_clear_mc(void)
{
	struct mem_cgroup *from = mc.from;
	struct mem_cgroup *to = mc.to;

	/* we must uncharge all the leftover precharges from mc.to */
	if (mc.precharge) {
		cancel_charge(mc.to, mc.precharge);
		mc.precharge = 0;
	}
	/*
	 * we didn't uncharge from mc.from at mem_cgroup_move_account(), so
	 * we must uncharge here.
	 */
	if (mc.moved_charge) {
		cancel_charge(mc.from, mc.moved_charge);
		mc.moved_charge = 0;
	}
	/* we must fixup refcnts and charges */
	if (mc.moved_swap) {
		/* uncharge swap account from the old cgroup */
		if (!mem_cgroup_is_root(mc.from))
			page_counter_uncharge(&mc.from->memsw, mc.moved_swap);

		mem_cgroup_id_put_many(mc.from, mc.moved_swap);

		/*
		 * we charged both to->memory and to->memsw, so we
		 * should uncharge to->memory.
		 */
		if (!mem_cgroup_is_root(mc.to))
			page_counter_uncharge(&mc.to->memory, mc.moved_swap);

		mem_cgroup_id_get_many(mc.to, mc.moved_swap);
		css_put_many(&mc.to->css, mc.moved_swap);

		mc.moved_swap = 0;
	}
	memcg_oom_recover(from);
	memcg_oom_recover(to);
	wake_up_all(&mc.waitq);
}

static void mem_cgroup_clear_mc(void)
{
	struct mm_struct *mm = mc.mm;

	/*
	 * we must clear moving_task before waking up waiters at the end of
	 * task migration.
	 */
	mc.moving_task = NULL;
	__mem_cgroup_clear_mc();
	spin_lock(&mc.lock);
	mc.from = NULL;
	mc.to = NULL;
	mc.mm = NULL;
	spin_unlock(&mc.lock);

	mmput(mm);
}

static int mem_cgroup_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct mem_cgroup *memcg = NULL; /* unneeded init to make gcc happy */
	struct mem_cgroup *from;
	struct task_struct *leader, *p;
	struct mm_struct *mm;
	unsigned long move_flags;
	int ret = 0;

	/* charge immigration isn't supported on the default hierarchy */
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return 0;

	/*
	 * Multi-process migrations only happen on the default hierarchy
	 * where charge immigration is not used.  Perform charge
	 * immigration if @tset contains a leader and whine if there are
	 * multiple.
	 */
	p = NULL;
	cgroup_taskset_for_each_leader(leader, css, tset) {
		WARN_ON_ONCE(p);
		p = leader;
		memcg = mem_cgroup_from_css(css);
	}
	if (!p)
		return 0;

	/*
	 * We are now commited to this value whatever it is. Changes in this
	 * tunable will only affect upcoming migrations, not the current one.
	 * So we need to save it, and keep it going.
	 */
	move_flags = READ_ONCE(memcg->move_charge_at_immigrate);
	if (!move_flags)
		return 0;

	from = mem_cgroup_from_task(p);

	VM_BUG_ON(from == memcg);

	mm = get_task_mm(p);
	if (!mm)
		return 0;
	/* We move charges only when we move a owner of the mm */
	if (mm->owner == p) {
		VM_BUG_ON(mc.from);
		VM_BUG_ON(mc.to);
		VM_BUG_ON(mc.precharge);
		VM_BUG_ON(mc.moved_charge);
		VM_BUG_ON(mc.moved_swap);

		spin_lock(&mc.lock);
		mc.mm = mm;
		mc.from = from;
		mc.to = memcg;
		mc.flags = move_flags;
		spin_unlock(&mc.lock);
		/* We set mc.moving_task later */

		ret = mem_cgroup_precharge_mc(mm);
		if (ret)
			mem_cgroup_clear_mc();
	} else {
		mmput(mm);
	}
	return ret;
}

static void mem_cgroup_cancel_attach(struct cgroup_taskset *tset)
{
	if (mc.to)
		mem_cgroup_clear_mc();
}

static int mem_cgroup_move_charge_pte_range(pmd_t *pmd,
				unsigned long addr, unsigned long end,
				struct mm_walk *walk)
{
	int ret = 0;
	struct vm_area_struct *vma = walk->vma;
	pte_t *pte;
	spinlock_t *ptl;
	enum mc_target_type target_type;
	union mc_target target;
	struct page *page;

	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		if (mc.precharge < HPAGE_PMD_NR) {
			spin_unlock(ptl);
			return 0;
		}
		target_type = get_mctgt_type_thp(vma, addr, *pmd, &target);
		if (target_type == MC_TARGET_PAGE) {
			page = target.page;
			if (!isolate_lru_page(page)) {
				if (!mem_cgroup_move_account(page, true,
							     mc.from, mc.to)) {
					mc.precharge -= HPAGE_PMD_NR;
					mc.moved_charge += HPAGE_PMD_NR;
				}
				putback_lru_page(page);
			}
			put_page(page);
		} else if (target_type == MC_TARGET_DEVICE) {
			page = target.page;
			if (!mem_cgroup_move_account(page, true,
						     mc.from, mc.to)) {
				mc.precharge -= HPAGE_PMD_NR;
				mc.moved_charge += HPAGE_PMD_NR;
			}
			put_page(page);
		}
		spin_unlock(ptl);
		return 0;
	}

	if (pmd_trans_unstable(pmd))
		return 0;
retry:
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	for (; addr != end; addr += PAGE_SIZE) {
		pte_t ptent = *(pte++);
		bool device = false;
		swp_entry_t ent;

		if (!mc.precharge)
			break;

		switch (get_mctgt_type(vma, addr, ptent, &target)) {
		case MC_TARGET_DEVICE:
			device = true;
			/* fall through */
		case MC_TARGET_PAGE:
			page = target.page;
			/*
			 * We can have a part of the split pmd here. Moving it
			 * can be done but it would be too convoluted so simply
			 * ignore such a partial THP and keep it in original
			 * memcg. There should be somebody mapping the head.
			 */
			if (PageTransCompound(page))
				goto put;
			if (!device && isolate_lru_page(page))
				goto put;
			if (!mem_cgroup_move_account(page, false,
						mc.from, mc.to)) {
				mc.precharge--;
				/* we uncharge from mc.from later. */
				mc.moved_charge++;
			}
			if (!device)
				putback_lru_page(page);
put:			/* get_mctgt_type() gets the page */
			put_page(page);
			break;
		case MC_TARGET_SWAP:
			ent = target.ent;
			if (!mem_cgroup_move_swap_account(ent, mc.from, mc.to)) {
				mc.precharge--;
				/* we fixup refcnts and charges later. */
				mc.moved_swap++;
			}
			break;
		default:
			break;
		}
	}
	pte_unmap_unlock(pte - 1, ptl);
	cond_resched();

	if (addr != end) {
		/*
		 * We have consumed all precharges we got in can_attach().
		 * We try charge one by one, but don't do any additional
		 * charges to mc.to if we have failed in charge once in attach()
		 * phase.
		 */
		ret = mem_cgroup_do_precharge(1);
		if (!ret)
			goto retry;
	}

	return ret;
}

static void mem_cgroup_move_charge(void)
{
	struct mm_walk mem_cgroup_move_charge_walk = {
		.pmd_entry = mem_cgroup_move_charge_pte_range,
		.mm = mc.mm,
	};

	lru_add_drain_all();
	/*
	 * Signal lock_page_memcg() to take the memcg's move_lock
	 * while we're moving its pages to another memcg. Then wait
	 * for already started RCU-only updates to finish.
	 */
	atomic_inc(&mc.from->moving_account);
	synchronize_rcu();
retry:
	if (unlikely(!down_read_trylock(&mc.mm->mmap_sem))) {
		/*
		 * Someone who are holding the mmap_sem might be waiting in
		 * waitq. So we cancel all extra charges, wake up all waiters,
		 * and retry. Because we cancel precharges, we might not be able
		 * to move enough charges, but moving charge is a best-effort
		 * feature anyway, so it wouldn't be a big problem.
		 */
		__mem_cgroup_clear_mc();
		cond_resched();
		goto retry;
	}
	/*
	 * When we have consumed all precharges and failed in doing
	 * additional charge, the page walk just aborts.
	 */
	walk_page_range(0, mc.mm->highest_vm_end, &mem_cgroup_move_charge_walk);

	up_read(&mc.mm->mmap_sem);
	atomic_dec(&mc.from->moving_account);
}

static void mem_cgroup_move_task(void)
{
	if (mc.to) {
		mem_cgroup_move_charge();
		mem_cgroup_clear_mc();
	}
}
#else	/* !CONFIG_MMU */
static int mem_cgroup_can_attach(struct cgroup_taskset *tset)
{
	return 0;
}
static void mem_cgroup_cancel_attach(struct cgroup_taskset *tset)
{
}
static void mem_cgroup_move_task(void)
{
}
#endif

/*
 * Cgroup retains root cgroups across [un]mount cycles making it necessary
 * to verify whether we're attached to the default hierarchy on each mount
 * attempt.
 */
static void mem_cgroup_bind(struct cgroup_subsys_state *root_css)
{
	/*
	 * use_hierarchy is forced on the default hierarchy.  cgroup core
	 * guarantees that @root doesn't have any children, so turning it
	 * on for the root memcg is enough.
	 */
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		root_mem_cgroup->use_hierarchy = true;
	else
		root_mem_cgroup->use_hierarchy = false;
}

static u64 memory_current_read(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return (u64)page_counter_read(&memcg->memory) * PAGE_SIZE;
}

static int memory_wmark_low_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long wmark_low = READ_ONCE(memcg->memory.wmark_low);

	if (wmark_low == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)wmark_low * PAGE_SIZE);

	return 0;
}

static int memory_wmark_high_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long wmark_high = READ_ONCE(memcg->memory.wmark_high);

	if (wmark_high == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)wmark_high * PAGE_SIZE);

	return 0;
}

static int memory_max_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long max = READ_ONCE(memcg->memory.max);

	if (max == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)max * PAGE_SIZE);

	return 0;
}

static ssize_t memory_max_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int nr_reclaims = MAX_RECLAIM_RETRIES;
	bool drained = false;
	unsigned long max;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &max);
	if (err)
		return err;

	xchg(&memcg->memory.max, max);

	for (;;) {
		unsigned long nr_pages = page_counter_read(&memcg->memory);

		if (nr_pages <= max)
			break;

		if (signal_pending(current)) {
			err = -EINTR;
			break;
		}

		if (!drained) {
			drain_all_stock(memcg);
			drained = true;
			continue;
		}

		if (nr_reclaims) {
			if (!try_to_free_mem_cgroup_pages(memcg, nr_pages - max,
							  GFP_KERNEL, true))
				nr_reclaims--;
			continue;
		}

		memcg_memory_event(memcg, MEMCG_OOM);
		if (!mem_cgroup_out_of_memory(memcg, GFP_KERNEL, 0))
			break;
	}

	setup_memcg_wmark(memcg);

	if (!is_wmark_ok(memcg, true))
		queue_work(memcg_wmark_wq, &memcg->wmark_work);

	memcg_wb_domain_size_changed(memcg);
	return nbytes;
}

static int memory_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	int i;

	/*
	 * Provide statistics on the state of the memory subsystem as
	 * well as cumulative event counters that show past behavior.
	 *
	 * This list is ordered following a combination of these gradients:
	 * 1) generic big picture -> specifics and details
	 * 2) reflecting userspace activity -> reflecting kernel heuristics
	 *
	 * Current memory state:
	 */

	seq_printf(m, "anon %llu\n",
		   (u64)memcg_page_state(memcg, NR_ANON_MAPPED) * PAGE_SIZE);
	seq_printf(m, "file %llu\n",
		   (u64)memcg_page_state(memcg, NR_FILE_PAGES) * PAGE_SIZE);
	seq_printf(m, "kernel_stack %llu\n",
		   (u64)memcg_page_state(memcg, MEMCG_KERNEL_STACK_KB) * 1024);
	seq_printf(m, "slab %llu\n",
		   (u64)(memcg_page_state(memcg, NR_SLAB_RECLAIMABLE) +
			 memcg_page_state(memcg, NR_SLAB_UNRECLAIMABLE)) *
		   PAGE_SIZE);
	seq_printf(m, "sock %llu\n",
		   (u64)memcg_page_state(memcg, MEMCG_SOCK) * PAGE_SIZE);

	seq_printf(m, "shmem %llu\n",
		   (u64)memcg_page_state(memcg, NR_SHMEM) * PAGE_SIZE);
	seq_printf(m, "file_mapped %llu\n",
		   (u64)memcg_page_state(memcg, NR_FILE_MAPPED) * PAGE_SIZE);
	seq_printf(m, "file_dirty %llu\n",
		   (u64)memcg_page_state(memcg, NR_FILE_DIRTY) * PAGE_SIZE);
	seq_printf(m, "file_writeback %llu\n",
		   (u64)memcg_page_state(memcg, NR_WRITEBACK) * PAGE_SIZE);

	for (i = 0; i < NR_LRU_LISTS; i++)
		seq_printf(m, "%s %llu\n", mem_cgroup_lru_names[i],
			   (u64)memcg_page_state(memcg, NR_LRU_BASE + i) *
			   PAGE_SIZE);

	seq_printf(m, "slab_reclaimable %llu\n",
		   (u64)memcg_page_state(memcg, NR_SLAB_RECLAIMABLE) *
		   PAGE_SIZE);
	seq_printf(m, "slab_unreclaimable %llu\n",
		   (u64)memcg_page_state(memcg, NR_SLAB_UNRECLAIMABLE) *
		   PAGE_SIZE);

	/* Accumulated memory events */

	seq_printf(m, "pgfault %lu\n", memcg_events(memcg, PGFAULT));
	seq_printf(m, "pgmajfault %lu\n", memcg_events(memcg, PGMAJFAULT));

	seq_printf(m, "workingset_refault_anon %lu\n",
		   memcg_page_state(memcg, WORKINGSET_REFAULT_ANON));
	seq_printf(m, "workingset_refault_file %lu\n",
		   memcg_page_state(memcg, WORKINGSET_REFAULT_FILE));
	seq_printf(m, "workingset_activate_anon %lu\n",
		   memcg_page_state(memcg, WORKINGSET_ACTIVATE_ANON));
	seq_printf(m, "workingset_activate_file %lu\n",
		   memcg_page_state(memcg, WORKINGSET_ACTIVATE_FILE));
	seq_printf(m, "workingset_restore_anon %lu\n",
		   memcg_page_state(memcg, WORKINGSET_RESTORE_ANON));
	seq_printf(m, "workingset_restore_file %lu\n",
		   memcg_page_state(memcg, WORKINGSET_RESTORE_FILE));
	seq_printf(m, "workingset_nodereclaim %lu\n",
		   memcg_page_state(memcg, WORKINGSET_NODERECLAIM));

	seq_printf(m, "pgrefill %lu\n", memcg_events(memcg, PGREFILL));
	seq_printf(m, "pgscan %lu\n", memcg_events(memcg, PGSCAN_KSWAPD) +
		   memcg_events(memcg, PGSCAN_DIRECT));
	seq_printf(m, "pgsteal %lu\n", memcg_events(memcg, PGSTEAL_KSWAPD) +
		   memcg_events(memcg, PGSTEAL_DIRECT));
	seq_printf(m, "pgactivate %lu\n", memcg_events(memcg, PGACTIVATE));
	seq_printf(m, "pgdeactivate %lu\n", memcg_events(memcg, PGDEACTIVATE));
	seq_printf(m, "pglazyfree %lu\n", memcg_events(memcg, PGLAZYFREE));
	seq_printf(m, "pglazyfreed %lu\n", memcg_events(memcg, PGLAZYFREED));

	return 0;
}

static struct cftype memory_files[] = {
	{
		.name = "current",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = memory_current_read,
	},
	{
		.name = "min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_min_show,
		.write = memory_min_write,
	},
	{
		.name = "low",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_low_show,
		.write = memory_low_write,
	},
	{
		.name = "high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_high_show,
		.write = memory_high_write,
	},
	{
		.name = "max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_max_show,
		.write = memory_max_write,
	},
	{
		.name = "wmark_ratio",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_ratio_show,
		.write = memory_wmark_ratio_write,
	},
	{
		.name = "wmark_high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_high_show,
	},
	{
		.name = "wmark_low",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_low_show,
	},
	{
		.name = "wmark_scale_factor",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_wmark_scale_factor_show,
		.write = memory_wmark_scale_factor_write,
	},
	{
		.name = "priority",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = mem_cgroup_priority_read,
		.write_u64 = mem_cgroup_priority_write,
	},
	{
		.name = "use_priority_oom",
		.write_u64 = mem_cgroup_priority_oom_write,
		.read_u64 = mem_cgroup_priority_oom_read,
	},
	{
		.name = "events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, events_file),
		.seq_show = memory_events_show,
	},
	{
		.name = "events.local",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, events_local_file),
		.seq_show = memory_events_local_show,
	},
	{
		.name = "stat",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_stat_show,
	},
	{
		.name = "oom.group",
		.flags = CFTYPE_NOT_ON_ROOT | CFTYPE_NS_DELEGATABLE,
		.seq_show = memory_oom_group_show,
		.write = memory_oom_group_write,
	},
#ifdef CONFIG_KIDLED
	{
		.name = "idle_page_stats",
		.seq_show = mem_cgroup_idle_page_stats_show,
		.write = mem_cgroup_idle_page_stats_write,
	},
#endif
	{ }	/* terminate */
};

struct cgroup_subsys memory_cgrp_subsys = {
	.css_alloc = mem_cgroup_css_alloc,
	.css_online = mem_cgroup_css_online,
	.css_offline = mem_cgroup_css_offline,
	.css_released = mem_cgroup_css_released,
	.css_free = mem_cgroup_css_free,
	.css_reset = mem_cgroup_css_reset,
	.can_attach = mem_cgroup_can_attach,
	.cancel_attach = mem_cgroup_cancel_attach,
	.post_attach = mem_cgroup_move_task,
	.bind = mem_cgroup_bind,
	.dfl_cftypes = memory_files,
	.legacy_cftypes = mem_cgroup_legacy_files,
	.early_init = 0,
};

/**
 * mem_cgroup_protected - check if memory consumption is in the normal range
 * @root: the top ancestor of the sub-tree being checked
 * @memcg: the memory cgroup to check
 *
 * WARNING: This function is not stateless! It can only be used as part
 *          of a top-down tree iteration, not for isolated queries.
 *
 * Returns one of the following:
 *   MEMCG_PROT_NONE: cgroup memory is not protected
 *   MEMCG_PROT_LOW: cgroup memory is protected as long there is
 *     an unprotected supply of reclaimable memory from other cgroups.
 *   MEMCG_PROT_MIN: cgroup memory is protected
 *
 * @root is exclusive; it is never protected when looked at directly
 *
 * To provide a proper hierarchical behavior, effective memory.min/low values
 * are used. Below is the description of how effective memory.low is calculated.
 * Effective memory.min values is calculated in the same way.
 *
 * Effective memory.low is always equal or less than the original memory.low.
 * If there is no memory.low overcommittment (which is always true for
 * top-level memory cgroups), these two values are equal.
 * Otherwise, it's a part of parent's effective memory.low,
 * calculated as a cgroup's memory.low usage divided by sum of sibling's
 * memory.low usages, where memory.low usage is the size of actually
 * protected memory.
 *
 *                                             low_usage
 * elow = min( memory.low, parent->elow * ------------------ ),
 *                                        siblings_low_usage
 *
 *             | memory.current, if memory.current < memory.low
 * low_usage = |
	       | 0, otherwise.
 *
 *
 * Such definition of the effective memory.low provides the expected
 * hierarchical behavior: parent's memory.low value is limiting
 * children, unprotected memory is reclaimed first and cgroups,
 * which are not using their guarantee do not affect actual memory
 * distribution.
 *
 * For example, if there are memcgs A, A/B, A/C, A/D and A/E:
 *
 *     A      A/memory.low = 2G, A/memory.current = 6G
 *    //\\
 *   BC  DE   B/memory.low = 3G  B/memory.current = 2G
 *            C/memory.low = 1G  C/memory.current = 2G
 *            D/memory.low = 0   D/memory.current = 2G
 *            E/memory.low = 10G E/memory.current = 0
 *
 * and the memory pressure is applied, the following memory distribution
 * is expected (approximately):
 *
 *     A/memory.current = 2G
 *
 *     B/memory.current = 1.3G
 *     C/memory.current = 0.6G
 *     D/memory.current = 0
 *     E/memory.current = 0
 *
 * These calculations require constant tracking of the actual low usages
 * (see propagate_protected_usage()), as well as recursive calculation of
 * effective memory.low values. But as we do call mem_cgroup_protected()
 * path for each memory cgroup top-down from the reclaim,
 * it's possible to optimize this part, and save calculated elow
 * for next usage. This part is intentionally racy, but it's ok,
 * as memory.low is a best-effort mechanism.
 */
enum mem_cgroup_protection mem_cgroup_protected(struct mem_cgroup *root,
						struct mem_cgroup *memcg)
{
	struct mem_cgroup *parent;
	unsigned long emin, parent_emin;
	unsigned long elow, parent_elow;
	unsigned long usage;

	if (mem_cgroup_disabled())
		return MEMCG_PROT_NONE;

	if (!root)
		root = root_mem_cgroup;
	if (memcg == root)
		return MEMCG_PROT_NONE;

	usage = page_counter_read(&memcg->memory);
	if (!usage)
		return MEMCG_PROT_NONE;

	emin = READ_ONCE(memcg->memory.min);
	elow = READ_ONCE(memcg->memory.low);

	parent = parent_mem_cgroup(memcg);
	/* No parent means a non-hierarchical mode on v1 memcg */
	if (!parent)
		return MEMCG_PROT_NONE;

	if (parent == root)
		goto exit;

	parent_emin = READ_ONCE(parent->memory.emin);
	emin = min(emin, parent_emin);
	if (emin && parent_emin) {
		unsigned long min_usage, siblings_min_usage;

		min_usage = min(usage, memcg->memory.min);
		siblings_min_usage = atomic_long_read(
			&parent->memory.children_min_usage);

		if (min_usage && siblings_min_usage)
			emin = min(emin, parent_emin * min_usage /
				   siblings_min_usage);
	}

	parent_elow = READ_ONCE(parent->memory.elow);
	elow = min(elow, parent_elow);
	if (elow && parent_elow) {
		unsigned long low_usage, siblings_low_usage;

		low_usage = min(usage, memcg->memory.low);
		siblings_low_usage = atomic_long_read(
			&parent->memory.children_low_usage);

		if (low_usage && siblings_low_usage)
			elow = min(elow, parent_elow * low_usage /
				   siblings_low_usage);
	}

exit:
	memcg->memory.emin = emin;
	memcg->memory.elow = elow;

	if (usage <= emin)
		return MEMCG_PROT_MIN;
	else if (usage <= elow)
		return MEMCG_PROT_LOW;
	else
		return MEMCG_PROT_NONE;
}

/**
 * mem_cgroup_charge - charge a newly allocated page to a cgroup
 * @page: page to charge
 * @mm: mm context of the victim
 * @gfp_mask: reclaim mode
 *
 * Try to charge @page to the memcg that @mm belongs to, reclaiming
 * pages according to @gfp_mask if necessary.
 *
 * Returns 0 on success. Otherwise, an error code is returned.
 */
int mem_cgroup_charge(struct page *page, struct mm_struct *mm, gfp_t gfp_mask)
{
	unsigned int nr_pages = hpage_nr_pages(page);
	struct mem_cgroup *memcg = NULL;
	int ret = 0;

	if (mem_cgroup_disabled())
		goto out;

	if (PageSwapCache(page)) {
		swp_entry_t ent = { .val = page_private(page), };
		unsigned short id;

		/*
		 * Every swap fault against a single page tries to charge the
		 * page, bail as early as possible.  shmem_unuse() encounters
		 * already charged pages, too.  page->mem_cgroup is protected
		 * by the page lock, which serializes swap cache removal, which
		 * in turn serializes uncharging.
		 */
		VM_BUG_ON_PAGE(!PageLocked(page), page);
		if (compound_head(page)->mem_cgroup)
			goto out;

		id = lookup_swap_cgroup_id(ent);
		rcu_read_lock();
		memcg = mem_cgroup_from_id(id);
		if (memcg && !css_tryget_online(&memcg->css))
			memcg = NULL;
		rcu_read_unlock();
	}

	if (!memcg)
		memcg = get_mem_cgroup_from_mm(mm);

	ret = try_charge(memcg, gfp_mask, nr_pages);
	if (ret)
		goto out_put;

	commit_charge(page, memcg);

	local_irq_disable();
	mem_cgroup_charge_statistics(memcg, page, nr_pages);
	memcg_check_events(memcg, page);
	local_irq_enable();

	if (PageSwapCache(page)) {
		swp_entry_t entry = { .val = page_private(page) };
		/*
		 * The swap entry might not get freed for a long time,
		 * let's not wait for it.  The page already received a
		 * memory+swap charge, drop the swap entry duplicate.
		 */
		mem_cgroup_uncharge_swap(entry, nr_pages);
	}

out_put:
	css_put(&memcg->css);
out:
	return ret;
}

struct uncharge_gather {
	struct mem_cgroup *memcg;
	unsigned long nr_pages;
	unsigned long pgpgout;
	unsigned long nr_kmem;
	struct page *dummy_page;
};

static inline void uncharge_gather_clear(struct uncharge_gather *ug)
{
	memset(ug, 0, sizeof(*ug));
}

static void uncharge_batch(const struct uncharge_gather *ug)
{
	unsigned long flags;

	if (!mem_cgroup_is_root(ug->memcg)) {
		page_counter_uncharge(&ug->memcg->memory, ug->nr_pages);
		if (do_memsw_account())
			page_counter_uncharge(&ug->memcg->memsw, ug->nr_pages);
		if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && ug->nr_kmem)
			page_counter_uncharge(&ug->memcg->kmem, ug->nr_kmem);
		memcg_oom_recover(ug->memcg);
	}

	local_irq_save(flags);
	__count_memcg_events(ug->memcg, PGPGOUT, ug->pgpgout);
	__this_cpu_add(ug->memcg->vmstats_percpu->nr_page_events, ug->nr_pages);
	memcg_check_events(ug->memcg, ug->dummy_page);
	local_irq_restore(flags);

	if (!mem_cgroup_is_root(ug->memcg))
		css_put_many(&ug->memcg->css, ug->nr_pages);
}

static void uncharge_page(struct page *page, struct uncharge_gather *ug)
{
	unsigned long nr_pages;

	VM_BUG_ON_PAGE(PageLRU(page), page);

	if (!page->mem_cgroup)
		return;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	del_hugepage_from_queue(page);
#endif

	/*
	 * Nobody should be changing or seriously looking at
	 * page->mem_cgroup at this point, we have fully
	 * exclusive access to the page.
	 */

	if (ug->memcg != page->mem_cgroup) {
		if (ug->memcg) {
			uncharge_batch(ug);
			uncharge_gather_clear(ug);
		}
		ug->memcg = page->mem_cgroup;
	}

	nr_pages = 1 << compound_order(page);
	ug->nr_pages += nr_pages;

	if (!PageKmemcg(page)) {
		ug->pgpgout++;
	} else {
		ug->nr_kmem += nr_pages;
		__ClearPageKmemcg(page);
	}

	ug->dummy_page = page;
	page->mem_cgroup = NULL;
}

static void uncharge_list(struct list_head *page_list)
{
	struct uncharge_gather ug;
	struct list_head *next;

	uncharge_gather_clear(&ug);

	/*
	 * Note that the list can be a single page->lru; hence the
	 * do-while loop instead of a simple list_for_each_entry().
	 */
	next = page_list->next;
	do {
		struct page *page;

		page = list_entry(next, struct page, lru);
		next = page->lru.next;

		uncharge_page(page, &ug);
	} while (next != page_list);

	if (ug.memcg)
		uncharge_batch(&ug);
}

/**
 * mem_cgroup_uncharge - uncharge a page
 * @page: page to uncharge
 *
 * Uncharge a page previously charged with mem_cgroup_charge().
 */
void mem_cgroup_uncharge(struct page *page)
{
	struct uncharge_gather ug;

	if (mem_cgroup_disabled())
		return;

	/* Don't touch page->lru of any random page, pre-check: */
	if (!page->mem_cgroup)
		return;

	uncharge_gather_clear(&ug);
	uncharge_page(page, &ug);
	uncharge_batch(&ug);
}

/**
 * mem_cgroup_uncharge_list - uncharge a list of page
 * @page_list: list of pages to uncharge
 *
 * Uncharge a list of pages previously charged with
 * mem_cgroup_charge().
 */
void mem_cgroup_uncharge_list(struct list_head *page_list)
{
	if (mem_cgroup_disabled())
		return;

	if (!list_empty(page_list))
		uncharge_list(page_list);
}

/**
 * mem_cgroup_migrate - charge a page's replacement
 * @oldpage: currently circulating page
 * @newpage: replacement page
 *
 * Charge @newpage as a replacement page for @oldpage. @oldpage will
 * be uncharged upon free.
 *
 * Both pages must be locked, @newpage->mapping must be set up.
 */
void mem_cgroup_migrate(struct page *oldpage, struct page *newpage)
{
	struct mem_cgroup *memcg;
	unsigned int nr_pages;
	bool compound;
	unsigned long flags;

	VM_BUG_ON_PAGE(!PageLocked(oldpage), oldpage);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);
	VM_BUG_ON_PAGE(PageAnon(oldpage) != PageAnon(newpage), newpage);
	VM_BUG_ON_PAGE(PageTransHuge(oldpage) != PageTransHuge(newpage),
		       newpage);

	if (mem_cgroup_disabled())
		return;

	/* Page cache replacement: new page already charged? */
	if (newpage->mem_cgroup)
		return;

	/* Swapcache readahead pages can get replaced before being charged */
	memcg = oldpage->mem_cgroup;
	if (!memcg)
		return;

	/* Force-charge the new page. The old one will be freed soon */
	compound = PageTransHuge(newpage);
	nr_pages = compound ? hpage_nr_pages(newpage) : 1;

	page_counter_charge(&memcg->memory, nr_pages);
	if (do_memsw_account())
		page_counter_charge(&memcg->memsw, nr_pages);
	css_get_many(&memcg->css, nr_pages);

	commit_charge(newpage, memcg);

	local_irq_save(flags);
	mem_cgroup_charge_statistics(memcg, newpage, nr_pages);
	memcg_check_events(memcg, newpage);
	local_irq_restore(flags);
}

DEFINE_STATIC_KEY_FALSE(memcg_sockets_enabled_key);
EXPORT_SYMBOL(memcg_sockets_enabled_key);

void mem_cgroup_sk_alloc(struct sock *sk)
{
	struct mem_cgroup *memcg;

	if (!mem_cgroup_sockets_enabled)
		return;

	/*
	 * Socket cloning can throw us here with sk_memcg already
	 * filled. It won't however, necessarily happen from
	 * process context. So the test for root memcg given
	 * the current task's memcg won't help us in this case.
	 *
	 * Respecting the original socket's memcg is a better
	 * decision in this case.
	 */
	if (sk->sk_memcg) {
		css_get(&sk->sk_memcg->css);
		return;
	}

	rcu_read_lock();
	memcg = mem_cgroup_from_task(current);
	if (memcg == root_mem_cgroup)
		goto out;
	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && !memcg->tcpmem_active)
		goto out;
	if (css_tryget_online(&memcg->css))
		sk->sk_memcg = memcg;
out:
	rcu_read_unlock();
}

void mem_cgroup_sk_free(struct sock *sk)
{
	if (sk->sk_memcg)
		css_put(&sk->sk_memcg->css);
}

/**
 * mem_cgroup_charge_skmem - charge socket memory
 * @memcg: memcg to charge
 * @nr_pages: number of pages to charge
 *
 * Charges @nr_pages to @memcg. Returns %true if the charge fit within
 * @memcg's configured limit, %false if the charge had to be forced.
 */
bool mem_cgroup_charge_skmem(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	gfp_t gfp_mask = GFP_KERNEL;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys)) {
		struct page_counter *fail;

		if (page_counter_try_charge(&memcg->tcpmem, nr_pages, &fail)) {
			memcg->tcpmem_pressure = 0;
			return true;
		}
		page_counter_charge(&memcg->tcpmem, nr_pages);
		memcg->tcpmem_pressure = 1;
		return false;
	}

	/* Don't block in the packet receive path */
	if (in_softirq())
		gfp_mask = GFP_NOWAIT;

	mod_memcg_state(memcg, MEMCG_SOCK, nr_pages);

	if (try_charge(memcg, gfp_mask, nr_pages) == 0)
		return true;

	try_charge(memcg, gfp_mask|__GFP_NOFAIL, nr_pages);
	return false;
}

/**
 * mem_cgroup_uncharge_skmem - uncharge socket memory
 * @memcg: memcg to uncharge
 * @nr_pages: number of pages to uncharge
 */
void mem_cgroup_uncharge_skmem(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys)) {
		page_counter_uncharge(&memcg->tcpmem, nr_pages);
		return;
	}

	mod_memcg_state(memcg, MEMCG_SOCK, -nr_pages);

	refill_stock(memcg, nr_pages);
}

static int __init cgroup_memory(char *s)
{
	char *token;

	while ((token = strsep(&s, ",")) != NULL) {
		if (!*token)
			continue;
		if (!strcmp(token, "nosocket"))
			cgroup_memory_nosocket = true;
		if (!strcmp(token, "nokmem"))
			cgroup_memory_nokmem = true;
	}
	return 0;
}
__setup("cgroup.memory=", cgroup_memory);

#ifdef CONFIG_CGROUP_WRITEBACK
bool cgwb_v1 = false;

static int __init enable_cgroup_writeback_v1(char *s)
{
	cgwb_v1 = true;

	return 0;
}
__setup("cgwb_v1", enable_cgroup_writeback_v1);
#endif

#ifdef CONFIG_MEMSLI
static int memsli_enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", !static_key_enabled(&cgroup_memory_nosli));
	return 0;
}

static int memsli_enabled_open(struct inode *inode, struct file *file)
{
	return single_open(file, memsli_enabled_show, NULL);
}

static ssize_t memsli_enabled_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char val = -1;
	int ret = count;

	if (count < 1 || *ppos) {
		ret = -EINVAL;
		goto out;
	}

	if (copy_from_user(&val, ubuf, 1)) {
		ret = -EFAULT;
		goto out;
	}

	switch (val) {
	case '0':
		static_branch_enable(&cgroup_memory_nosli);
		break;
	case '1':
		static_branch_disable(&cgroup_memory_nosli);
		break;
	default:
		ret = -EINVAL;
	}

out:
	return ret;
}

static const struct file_operations memsli_enabled_fops = {
	.open           = memsli_enabled_open,
	.read           = seq_read,
	.write          = memsli_enabled_write,
	.llseek         = seq_lseek,
	.release        = single_release,
};
#endif /* CONFIG_MEMSLI */

/*
 * subsys_initcall() for memory controller.
 *
 * Some parts like memcg_hotplug_cpu_dead() have to be initialized from this
 * context because of lock dependencies (cgroup_lock -> cpu hotplug) but
 * basically everything that doesn't depend on a specific mem_cgroup structure
 * should be initialized from here.
 */
static int __init mem_cgroup_init(void)
{
	int cpu, node;
#ifdef CONFIG_MEMSLI
	struct proc_dir_entry *memsli_dir, *memsli_enabled_file;

	memsli_dir = proc_mkdir("memsli", NULL);
	if (!memsli_dir)
		return -ENOMEM;

	memsli_enabled_file = proc_create("enabled", 0600,
					  memsli_dir, &memsli_enabled_fops);
	if (!memsli_enabled_file) {
		remove_proc_entry("memsli", NULL);
		return -ENOMEM;
	}
#endif /* CONFIG_MEMSLI */

	memcg_wmark_wq = alloc_workqueue("memcg_wmark", WQ_MEM_RECLAIM |
				WQ_UNBOUND | WQ_FREEZABLE,
				WQ_UNBOUND_MAX_ACTIVE);

	if (!memcg_wmark_wq)
		return -ENOMEM;

#ifdef CONFIG_MEMCG_KMEM
	/*
	 * Kmem cache creation is mostly done with the slab_mutex held,
	 * so use a workqueue with limited concurrency to avoid stalling
	 * all worker threads in case lots of cgroups are created and
	 * destroyed simultaneously.
	 */
	memcg_kmem_cache_wq = alloc_workqueue("memcg_kmem_cache", 0, 1);
	BUG_ON(!memcg_kmem_cache_wq);
#endif

	cpuhp_setup_state_nocalls(CPUHP_MM_MEMCQ_DEAD, "mm/memctrl:dead", NULL,
				  memcg_hotplug_cpu_dead);

	for_each_possible_cpu(cpu)
		INIT_WORK(&per_cpu_ptr(&memcg_stock, cpu)->work,
			  drain_local_stock);

	for_each_node(node) {
		struct mem_cgroup_tree_per_node *rtpn;

		rtpn = kzalloc_node(sizeof(*rtpn), GFP_KERNEL,
				    node_online(node) ? node : NUMA_NO_NODE);

		rtpn->rb_root = RB_ROOT;
		rtpn->rb_rightmost = NULL;
		spin_lock_init(&rtpn->lock);
		soft_limit_tree.rb_tree_per_node[node] = rtpn;
	}

	return 0;
}
subsys_initcall(mem_cgroup_init);

#ifdef CONFIG_MEMCG_SWAP
static struct mem_cgroup *mem_cgroup_id_get_online(struct mem_cgroup *memcg)
{
	while (!atomic_inc_not_zero(&memcg->id.ref)) {
		/*
		 * The root cgroup cannot be destroyed, so it's refcount must
		 * always be >= 1.
		 */
		if (WARN_ON_ONCE(memcg == root_mem_cgroup)) {
			VM_BUG_ON(1);
			break;
		}
		memcg = parent_mem_cgroup(memcg);
		if (!memcg)
			memcg = root_mem_cgroup;
	}
	return memcg;
}

/**
 * mem_cgroup_swapout - transfer a memsw charge to swap
 * @page: page whose memsw charge to transfer
 * @entry: swap entry to move the charge to
 *
 * Transfer the memsw charge of @page to @entry.
 */
void mem_cgroup_swapout(struct page *page, swp_entry_t entry)
{
	struct mem_cgroup *memcg, *swap_memcg;
	unsigned int nr_entries;
	unsigned short oldid;

	VM_BUG_ON_PAGE(PageLRU(page), page);
	VM_BUG_ON_PAGE(page_count(page), page);

	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return;

	memcg = page->mem_cgroup;

	/* Readahead page, never charged */
	if (!memcg)
		return;

	/*
	 * In case the memcg owning these pages has been offlined and doesn't
	 * have an ID allocated to it anymore, charge the closest online
	 * ancestor for the swap instead and transfer the memory+swap charge.
	 */
	swap_memcg = mem_cgroup_id_get_online(memcg);
	nr_entries = hpage_nr_pages(page);
	/* Get references for the tail pages, too */
	if (nr_entries > 1)
		mem_cgroup_id_get_many(swap_memcg, nr_entries - 1);
	oldid = swap_cgroup_record(entry, mem_cgroup_id(swap_memcg),
				   nr_entries);
	VM_BUG_ON_PAGE(oldid, page);
	mod_memcg_state(swap_memcg, MEMCG_SWAP, nr_entries);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	del_hugepage_from_queue(page);
#endif

	page->mem_cgroup = NULL;

	if (!mem_cgroup_is_root(memcg))
		page_counter_uncharge(&memcg->memory, nr_entries);

	if (!cgroup_memory_noswap && memcg != swap_memcg) {
		if (!mem_cgroup_is_root(swap_memcg))
			page_counter_charge(&swap_memcg->memsw, nr_entries);
		page_counter_uncharge(&memcg->memsw, nr_entries);
	}

	/*
	 * Interrupts should be disabled here because the caller holds the
	 * i_pages lock which is taken with interrupts-off. It is
	 * important here to have the interrupts disabled because it is the
	 * only synchronisation we have for updating the per-CPU variables.
	 */
	VM_BUG_ON(!irqs_disabled());
	mem_cgroup_charge_statistics(memcg, page, -nr_entries);
	memcg_check_events(memcg, page);

	if (!mem_cgroup_is_root(memcg))
		css_put_many(&memcg->css, nr_entries);
}

/**
 * mem_cgroup_try_charge_swap - try charging swap space for a page
 * @page: page being added to swap
 * @entry: swap entry to charge
 *
 * Try to charge @page's memcg for the swap space at @entry.
 *
 * Returns 0 on success, -ENOMEM on failure.
 */
int mem_cgroup_try_charge_swap(struct page *page, swp_entry_t entry)
{
	unsigned int nr_pages = hpage_nr_pages(page);
	struct page_counter *counter;
	struct mem_cgroup *memcg;
	unsigned short oldid;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return 0;

	memcg = page->mem_cgroup;

	/* Readahead page, never charged */
	if (!memcg)
		return 0;

	if (!entry.val) {
		memcg_memory_event(memcg, MEMCG_SWAP_FAIL);
		return 0;
	}

	memcg = mem_cgroup_id_get_online(memcg);

	if (!cgroup_memory_noswap && !mem_cgroup_is_root(memcg) &&
	    !page_counter_try_charge(&memcg->swap, nr_pages, &counter)) {
		memcg_memory_event(memcg, MEMCG_SWAP_MAX);
		memcg_memory_event(memcg, MEMCG_SWAP_FAIL);
		mem_cgroup_id_put(memcg);
		return -ENOMEM;
	}

	/* Get references for the tail pages, too */
	if (nr_pages > 1)
		mem_cgroup_id_get_many(memcg, nr_pages - 1);
	oldid = swap_cgroup_record(entry, mem_cgroup_id(memcg), nr_pages);
	VM_BUG_ON_PAGE(oldid, page);
	mod_memcg_state(memcg, MEMCG_SWAP, nr_pages);

	return 0;
}

/**
 * mem_cgroup_uncharge_swap - uncharge swap space
 * @entry: swap entry to uncharge
 * @nr_pages: the amount of swap space to uncharge
 */
void mem_cgroup_uncharge_swap(swp_entry_t entry, unsigned int nr_pages)
{
	struct mem_cgroup *memcg;
	unsigned short id;

	id = swap_cgroup_record(entry, 0, nr_pages);
	rcu_read_lock();
	memcg = mem_cgroup_from_id(id);
	if (memcg) {
		if (!cgroup_memory_noswap && !mem_cgroup_is_root(memcg)) {
			if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
				page_counter_uncharge(&memcg->swap, nr_pages);
			else
				page_counter_uncharge(&memcg->memsw, nr_pages);
		}
		mod_memcg_state(memcg, MEMCG_SWAP, -nr_pages);
		mem_cgroup_id_put_many(memcg, nr_pages);
	}
	rcu_read_unlock();
}

long mem_cgroup_get_nr_swap_pages(struct mem_cgroup *memcg)
{
	long nr_swap_pages = get_nr_swap_pages();

	if (cgroup_memory_noswap || !cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return nr_swap_pages;
	for (; memcg != root_mem_cgroup; memcg = parent_mem_cgroup(memcg))
		nr_swap_pages = min_t(long, nr_swap_pages,
				      READ_ONCE(memcg->swap.max) -
				      page_counter_read(&memcg->swap));
	return nr_swap_pages;
}

bool mem_cgroup_swap_full(struct page *page)
{
	struct mem_cgroup *memcg;

	VM_BUG_ON_PAGE(!PageLocked(page), page);

	if (vm_swap_full())
		return true;
	if (cgroup_memory_noswap || !cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return false;

	memcg = page->mem_cgroup;
	if (!memcg)
		return false;

	for (; memcg != root_mem_cgroup; memcg = parent_mem_cgroup(memcg)) {
		unsigned long usage = page_counter_read(&memcg->swap);

		if (usage * 2 >= READ_ONCE(memcg->swap.high) ||
		    usage * 2 >= READ_ONCE(memcg->swap.max))
			return true;
	}

	return false;
}

static int __init setup_swap_account(char *s)
{
	if (!strcmp(s, "1"))
		cgroup_memory_noswap = 0;
	else if (!strcmp(s, "0"))
		cgroup_memory_noswap = 1;
	return 1;
}
__setup("swapaccount=", setup_swap_account);

static u64 swap_current_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return (u64)page_counter_read(&memcg->swap) * PAGE_SIZE;
}

static int swap_high_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long high = READ_ONCE(memcg->swap.high);

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		high = READ_ONCE(memcg->memsw.high);

	if (high == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)high * PAGE_SIZE);

	return 0;
}

static ssize_t swap_high_write(struct kernfs_open_file *of,
			       char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long high;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &high);
	if (err)
		return err;

	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		page_counter_set_high(&memcg->swap, high);
	else
		page_counter_set_high(&memcg->memsw, high);

	return nbytes;
}

static int swap_max_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	unsigned long max = READ_ONCE(memcg->swap.max);

	if (max == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)max * PAGE_SIZE);

	return 0;
}

static ssize_t swap_max_write(struct kernfs_open_file *of,
			      char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long max;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &max);
	if (err)
		return err;

	xchg(&memcg->swap.max, max);

	return nbytes;
}

static int swap_events_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	seq_printf(m, "high %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_SWAP_HIGH]));
	seq_printf(m, "max %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_SWAP_MAX]));
	seq_printf(m, "fail %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_SWAP_FAIL]));

	return 0;
}

static struct cftype swap_files[] = {
	{
		.name = "swap.current",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = swap_current_read,
	},
	{
		.name = "swap.high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = swap_high_show,
		.write = swap_high_write,
	},
	{
		.name = "swap.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = swap_max_show,
		.write = swap_max_write,
	},
	{
		.name = "swap.events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, swap_events_file),
		.seq_show = swap_events_show,
	},
	{ }	/* terminate */
};

static struct cftype memsw_files[] = {
	{
		.name = "memsw.usage_in_bytes",
		.private = MEMFILE_PRIVATE(_MEMSWAP, RES_USAGE),
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "memsw.max_usage_in_bytes",
		.private = MEMFILE_PRIVATE(_MEMSWAP, RES_MAX_USAGE),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "memsw.limit_in_bytes",
		.private = MEMFILE_PRIVATE(_MEMSWAP, RES_LIMIT),
		.write = mem_cgroup_write,
		.read_u64 = mem_cgroup_read_u64,
	},
	{
		.name = "memsw.failcnt",
		.private = MEMFILE_PRIVATE(_MEMSWAP, RES_FAILCNT),
		.write = mem_cgroup_reset,
		.read_u64 = mem_cgroup_read_u64,
	},
	{ },	/* terminate */
};

/*
 * If mem_cgroup_swap_init() is implemented as a subsys_initcall()
 * instead of a core_initcall(), this could mean cgroup_memory_noswap still
 * remains set to false even when memcg is disabled via "cgroup_disable=memory"
 * boot parameter. This may result in premature OOPS inside
 * mem_cgroup_get_nr_swap_pages() function in corner cases.
 */
static int __init mem_cgroup_swap_init(void)
{
	/* No memory control -> no swap control */
	if (mem_cgroup_disabled())
		cgroup_memory_noswap = true;

	if (cgroup_memory_noswap)
		return 0;

	WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys, swap_files));
	WARN_ON(cgroup_add_legacy_cftypes(&memory_cgrp_subsys, memsw_files));

	return 0;
}
core_initcall(mem_cgroup_swap_init);

#endif /* CONFIG_MEMCG_SWAP */

#ifdef CONFIG_RICH_CONTAINER
static inline struct mem_cgroup *css_memcg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct mem_cgroup, css) : NULL;
}

/* with rcu lock held */
struct mem_cgroup *rich_container_get_memcg(void)
{
	struct cgroup_subsys_state *css;
	struct mem_cgroup *memcg_src;

#ifdef CONFIG_RICH_CONTAINER_CG_SWITCH
	css = task_css(current, memory_cgrp_id);
	while (css) {
		if (test_bit(CGRP_RICH_CONTAINER_SOURCE, &css->cgroup->flags))
			break;
		css = css->parent;
	}
#else
	if (sysctl_rich_container_source == 1)
		css = NULL;
	else
		css = task_css(current, memory_cgrp_id);
#endif

	if (css) {
		memcg_src = css_memcg(css);
	} else {
		read_lock(&tasklist_lock);
		memcg_src = mem_cgroup_from_task(task_active_pid_ns(current)->child_reaper);
		read_unlock(&tasklist_lock);
	}

	if (css_tryget(&memcg_src->css))
		return memcg_src;
	else
		return NULL;
}

void memcg_meminfo(struct mem_cgroup *memcg,
		struct sysinfo *info, struct sysinfo_ext *ext)
{
	struct mem_cgroup *iter;
	unsigned long limit, memsw_limit, usage;
	unsigned long pagecache, memcg_wmark, swap_size;
	int i;

	ext->cached = memcg_page_state(memcg, NR_FILE_PAGES);
	ext->file_dirty = memcg_page_state(memcg, NR_FILE_DIRTY);
	ext->writeback = memcg_page_state(memcg, NR_WRITEBACK);
	ext->anon_mapped = memcg_page_state(memcg, NR_ANON_MAPPED);
	ext->file_mapped = memcg_page_state(memcg, NR_FILE_MAPPED);
	ext->slab_reclaimable = memcg_page_state(memcg, NR_SLAB_RECLAIMABLE);
	ext->slab_unreclaimable =
		memcg_page_state(memcg, NR_SLAB_UNRECLAIMABLE);
	ext->kernel_stack_kb = memcg_page_state(memcg, MEMCG_KERNEL_STACK_KB);
	ext->unstable_nfs = 0;
	ext->writeback_temp = 0;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	ext->anon_thps = memcg_page_state(memcg, NR_ANON_THPS);
#endif
	ext->shmem_thps = 0;
	ext->shmem_pmd_mapped = 0;

	swap_size = memcg_page_state(memcg, MEMCG_SWAP);
	limit = memsw_limit = PAGE_COUNTER_MAX;
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		limit = min(limit, iter->memory.max);
		memsw_limit = min(memsw_limit, iter->memsw.max);
	}
	usage = mem_cgroup_usage(memcg, false);
	info->totalram = limit > totalram_pages ? totalram_pages : limit;
	info->sharedram = memcg_page_state(memcg, NR_SHMEM);
	info->freeram = info->totalram - usage;
	/* these are not accounted by memcg yet */
	/* if give bufferram the global value, free may show a quite
	 * large number in the ±buffers/caches row, the reason is
	 * it's equal to group_used - global_buffer - group_cached,
	 * if global_buffer > group_used, we get a rewind large value.
	 */
	info->bufferram = 0;
	info->totalhigh = totalhigh_pages;
	info->freehigh = nr_free_highpages();
	info->mem_unit = PAGE_SIZE;

	/* fill in swinfo */
	if (!cgroup_memory_noswap) {
		si_swapinfo(info);
		if (memsw_limit < info->totalswap)
			info->totalswap = memsw_limit;
		info->freeswap = info->totalswap - swap_size;
	} else {
		info->totalswap = 0;
		info->freeswap = 0;
	}

	for (i = 0; i < NR_LRU_LISTS; i++)
		ext->lrupages[i] = memcg_page_state(memcg, NR_LRU_BASE + i);

	/* Like what si_mem_available() does */
	memcg_wmark = memcg->memory.wmark_high;
	if (memcg->wmark_ratio && info->totalram > memcg_wmark)
		memcg_wmark = info->totalram - memcg_wmark;
	else
		memcg_wmark = 0;
	pagecache = ext->lrupages[LRU_ACTIVE_FILE] +
		ext->lrupages[LRU_INACTIVE_FILE];
	pagecache -= min(pagecache / 2, memcg_wmark);
	ext->available = info->freeram + pagecache;
	ext->available += ext->slab_reclaimable -
		min(ext->slab_reclaimable / 2, memcg_wmark);
}
#endif
