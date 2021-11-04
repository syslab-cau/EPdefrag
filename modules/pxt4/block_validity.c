// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/pxt4/block_validity.c
 *
 * Copyright (C) 2009
 * Theodore Ts'o (tytso@mit.edu)
 *
 * Track which blocks in the filesystem are metadata blocks that
 * should never be used as data blocks by files or directories.
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include "pxt4.h"

struct pxt4_system_zone {
	struct rb_node	node;
	pxt4_fsblk_t	start_blk;
	unsigned int	count;
};

static struct kmem_cache *pxt4_system_zone_cachep;

int __init pxt4_init_system_zone(void)
{
	pxt4_system_zone_cachep = KMEM_CACHE(pxt4_system_zone, 0);
	if (pxt4_system_zone_cachep == NULL)
		return -ENOMEM;
	return 0;
}

void pxt4_exit_system_zone(void)
{
	rcu_barrier();
	kmem_cache_destroy(pxt4_system_zone_cachep);
}

static inline int can_merge(struct pxt4_system_zone *entry1,
		     struct pxt4_system_zone *entry2)
{
	if ((entry1->start_blk + entry1->count) == entry2->start_blk)
		return 1;
	return 0;
}

static void release_system_zone(struct pxt4_system_blocks *system_blks)
{
	struct pxt4_system_zone	*entry, *n;

	rbtree_postorder_for_each_entry_safe(entry, n,
				&system_blks->root, node)
		kmem_cache_free(pxt4_system_zone_cachep, entry);
}

/*
 * Mark a range of blocks as belonging to the "system zone" --- that
 * is, filesystem metadata blocks which should never be used by
 * inodes.
 */
static int add_system_zone(struct pxt4_system_blocks *system_blks,
			   pxt4_fsblk_t start_blk,
			   unsigned int count)
{
	struct pxt4_system_zone *new_entry = NULL, *entry;
	struct rb_node **n = &system_blks->root.rb_node, *node;
	struct rb_node *parent = NULL, *new_node = NULL;

	while (*n) {
		parent = *n;
		entry = rb_entry(parent, struct pxt4_system_zone, node);
		if (start_blk < entry->start_blk)
			n = &(*n)->rb_left;
		else if (start_blk >= (entry->start_blk + entry->count))
			n = &(*n)->rb_right;
		else {
			if (start_blk + count > (entry->start_blk +
						 entry->count))
				entry->count = (start_blk + count -
						entry->start_blk);
			new_node = *n;
			new_entry = rb_entry(new_node, struct pxt4_system_zone,
					     node);
			break;
		}
	}

	if (!new_entry) {
		new_entry = kmem_cache_alloc(pxt4_system_zone_cachep,
					     GFP_KERNEL);
		if (!new_entry)
			return -ENOMEM;
		new_entry->start_blk = start_blk;
		new_entry->count = count;
		new_node = &new_entry->node;

		rb_link_node(new_node, parent, n);
		rb_insert_color(new_node, &system_blks->root);
	}

	/* Can we merge to the left? */
	node = rb_prev(new_node);
	if (node) {
		entry = rb_entry(node, struct pxt4_system_zone, node);
		if (can_merge(entry, new_entry)) {
			new_entry->start_blk = entry->start_blk;
			new_entry->count += entry->count;
			rb_erase(node, &system_blks->root);
			kmem_cache_free(pxt4_system_zone_cachep, entry);
		}
	}

	/* Can we merge to the right? */
	node = rb_next(new_node);
	if (node) {
		entry = rb_entry(node, struct pxt4_system_zone, node);
		if (can_merge(new_entry, entry)) {
			new_entry->count += entry->count;
			rb_erase(node, &system_blks->root);
			kmem_cache_free(pxt4_system_zone_cachep, entry);
		}
	}
	return 0;
}

static void debug_print_tree(struct pxt4_sb_info *sbi)
{
	struct rb_node *node;
	struct pxt4_system_zone *entry;
	struct pxt4_system_blocks *system_blks;
	int first = 1;

	printk(KERN_INFO "System zones: ");
	rcu_read_lock();
	system_blks = rcu_dereference(sbi->system_blks);
	node = rb_first(&system_blks->root);
	while (node) {
		entry = rb_entry(node, struct pxt4_system_zone, node);
		printk(KERN_CONT "%s%llu-%llu", first ? "" : ", ",
		       entry->start_blk, entry->start_blk + entry->count - 1);
		first = 0;
		node = rb_next(node);
	}
	rcu_read_unlock();
	printk(KERN_CONT "\n");
}

/*
 * Returns 1 if the passed-in block region (start_blk,
 * start_blk+count) is valid; 0 if some part of the block region
 * overlaps with filesystem metadata blocks.
 */
static int pxt4_data_block_valid_rcu(struct pxt4_sb_info *sbi,
				     struct pxt4_system_blocks *system_blks,
				     pxt4_fsblk_t start_blk,
				     unsigned int count)
{
	struct pxt4_system_zone *entry;
	struct rb_node *n;

	if ((start_blk <= le32_to_cpu(sbi->s_es->s_first_data_block)) ||
	    (start_blk + count < start_blk) ||
	    (start_blk + count > pxt4_blocks_count(sbi->s_es))) {
		sbi->s_es->s_last_error_block = cpu_to_le64(start_blk);
		return 0;
	}

	if (system_blks == NULL)
		return 1;

	n = system_blks->root.rb_node;
	while (n) {
		entry = rb_entry(n, struct pxt4_system_zone, node);
		if (start_blk + count - 1 < entry->start_blk)
			n = n->rb_left;
		else if (start_blk >= (entry->start_blk + entry->count))
			n = n->rb_right;
		else {
			sbi->s_es->s_last_error_block = cpu_to_le64(start_blk);
			return 0;
		}
	}
	return 1;
}

static int pxt4_protect_reserved_inode(struct super_block *sb,
				       struct pxt4_system_blocks *system_blks,
				       u32 ino)
{
	struct inode *inode;
	struct pxt4_sb_info *sbi = PXT4_SB(sb);
	struct pxt4_map_blocks map;
	u32 i = 0, num;
	int err = 0, n;

	if ((ino < PXT4_ROOT_INO) ||
	    (ino > le32_to_cpu(sbi->s_es->s_inodes_count)))
		return -EINVAL;
	inode = pxt4_iget(sb, ino, PXT4_IGET_SPECIAL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	num = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	while (i < num) {
		cond_resched();
		map.m_lblk = i;
		map.m_len = num - i;
		n = pxt4_map_blocks(NULL, inode, &map, 0);
		if (n < 0) {
			err = n;
			break;
		}
		if (n == 0) {
			i++;
		} else {
			if (!pxt4_data_block_valid_rcu(sbi, system_blks,
						map.m_pblk, n)) {
				pxt4_error(sb, "blocks %llu-%llu from inode %u "
					   "overlap system zone", map.m_pblk,
					   map.m_pblk + map.m_len - 1, ino);
				err = -EFSCORRUPTED;
				break;
			}
			err = add_system_zone(system_blks, map.m_pblk, n);
			if (err < 0)
				break;
			i += n;
		}
	}
	iput(inode);
	return err;
}

static void pxt4_destroy_system_zone(struct rcu_head *rcu)
{
	struct pxt4_system_blocks *system_blks;

	system_blks = container_of(rcu, struct pxt4_system_blocks, rcu);
	release_system_zone(system_blks);
	kfree(system_blks);
}

/*
 * Build system zone rbtree which is used for block validity checking.
 *
 * The update of system_blks pointer in this function is protected by
 * sb->s_umount semaphore. However we have to be careful as we can be
 * racing with pxt4_data_block_valid() calls reading system_blks rbtree
 * protected only by RCU. That's why we first build the rbtree and then
 * swap it in place.
 */
int pxt4_setup_system_zone(struct super_block *sb)
{
	pxt4_group_t ngroups = pxt4_get_groups_count(sb);
	struct pxt4_sb_info *sbi = PXT4_SB(sb);
	struct pxt4_system_blocks *system_blks;
	struct pxt4_group_desc *gdp;
	pxt4_group_t i;
	int flex_size = pxt4_flex_bg_size(sbi);
	int ret;

	if (!test_opt(sb, BLOCK_VALIDITY)) {
		if (sbi->system_blks)
			pxt4_release_system_zone(sb);
		return 0;
	}
	if (sbi->system_blks)
		return 0;

	system_blks = kzalloc(sizeof(*system_blks), GFP_KERNEL);
	if (!system_blks)
		return -ENOMEM;

	for (i=0; i < ngroups; i++) {
		cond_resched();
		if (pxt4_bg_has_super(sb, i) &&
		    ((i < 5) || ((i % flex_size) == 0)))
			add_system_zone(system_blks,
					pxt4_group_first_block_no(sb, i),
					pxt4_bg_num_gdb(sb, i) + 1);
		gdp = pxt4_get_group_desc(sb, i, NULL);
		ret = add_system_zone(system_blks,
				pxt4_block_bitmap(sb, gdp), 1);
		if (ret)
			goto err;
		ret = add_system_zone(system_blks,
				pxt4_inode_bitmap(sb, gdp), 1);
		if (ret)
			goto err;
		ret = add_system_zone(system_blks,
				pxt4_inode_table(sb, gdp),
				sbi->s_itb_per_group);
		if (ret)
			goto err;
	}
	if (pxt4_has_feature_journal(sb) && sbi->s_es->s_journal_inum) {
		ret = pxt4_protect_reserved_inode(sb, system_blks,
				le32_to_cpu(sbi->s_es->s_journal_inum));
		if (ret)
			goto err;
	}

	/*
	 * System blks rbtree complete, announce it once to prevent racing
	 * with pxt4_data_block_valid() accessing the rbtree at the same
	 * time.
	 */
	rcu_assign_pointer(sbi->system_blks, system_blks);

	if (test_opt(sb, DEBUG))
		debug_print_tree(sbi);
	return 0;
err:
	release_system_zone(system_blks);
	kfree(system_blks);
	return ret;
}

/*
 * Called when the filesystem is unmounted or when remounting it with
 * noblock_validity specified.
 *
 * The update of system_blks pointer in this function is protected by
 * sb->s_umount semaphore. However we have to be careful as we can be
 * racing with pxt4_data_block_valid() calls reading system_blks rbtree
 * protected only by RCU. So we first clear the system_blks pointer and
 * then free the rbtree only after RCU grace period expires.
 */
void pxt4_release_system_zone(struct super_block *sb)
{
	struct pxt4_system_blocks *system_blks;

	system_blks = rcu_dereference_protected(PXT4_SB(sb)->system_blks,
					lockdep_is_held(&sb->s_umount));
	rcu_assign_pointer(PXT4_SB(sb)->system_blks, NULL);

	if (system_blks)
		call_rcu(&system_blks->rcu, pxt4_destroy_system_zone);
}

int pxt4_data_block_valid(struct pxt4_sb_info *sbi, pxt4_fsblk_t start_blk,
			  unsigned int count)
{
	struct pxt4_system_blocks *system_blks;
	int ret;

	/*
	 * Lock the system zone to prevent it being released concurrently
	 * when doing a remount which inverse current "[no]block_validity"
	 * mount option.
	 */
	rcu_read_lock();
	system_blks = rcu_dereference(sbi->system_blks);
	ret = pxt4_data_block_valid_rcu(sbi, system_blks, start_blk,
					count);
	rcu_read_unlock();
	return ret;
}

int pxt4_check_blockref(const char *function, unsigned int line,
			struct inode *inode, __le32 *p, unsigned int max)
{
	struct pxt4_super_block *es = PXT4_SB(inode->i_sb)->s_es;
	__le32 *bref = p;
	unsigned int blk;

	if (pxt4_has_feature_journal(inode->i_sb) &&
	    (inode->i_ino ==
	     le32_to_cpu(PXT4_SB(inode->i_sb)->s_es->s_journal_inum)))
		return 0;

	while (bref < p+max) {
		blk = le32_to_cpu(*bref++);
		if (blk &&
		    unlikely(!pxt4_data_block_valid(PXT4_SB(inode->i_sb),
						    blk, 1))) {
			es->s_last_error_block = cpu_to_le64(blk);
			pxt4_error_inode(inode, function, line, blk,
					 "invalid block");
			return -EFSCORRUPTED;
		}
	}
	return 0;
}

