// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/pxt4/fsync.c
 *
 *  Copyright (C) 1993  Stephen Tweedie (sct@redhat.com)
 *  from
 *  Copyright (C) 1992  Remy Card (card@masi.ibp.fr)
 *                      Laboratoire MASI - Institut Blaise Pascal
 *                      Universite Pierre et Marie Curie (Paris VI)
 *  from
 *  linux/fs/minix/truncate.c   Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  pxt4fs fsync primitive
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 *  Removed unnecessary code duplication for little endian machines
 *  and excessive __inline__s.
 *        Andi Kleen, 1997
 *
 * Major simplications and cleanup - we only need to do the metadata, because
 * we can depend on generic_block_fdatasync() to sync the data blocks.
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>

#include "pxt4.h"
#include "pxt4_jbd3.h"

#include <trace/events/pxt4.h>

/*
 * If we're not journaling and this is a just-created file, we have to
 * sync our parent directory (if it was freshly created) since
 * otherwise it will only be written by writeback, leaving a huge
 * window during which a crash may lose the file.  This may apply for
 * the parent directory's parent as well, and so on recursively, if
 * they are also freshly created.
 */
static int pxt4_sync_parent(struct inode *inode)
{
	struct dentry *dentry = NULL;
	struct inode *next;
	int ret = 0;

	if (!pxt4_test_inode_state(inode, PXT4_STATE_NEWENTRY))
		return 0;
	inode = igrab(inode);
	while (pxt4_test_inode_state(inode, PXT4_STATE_NEWENTRY)) {
		pxt4_clear_inode_state(inode, PXT4_STATE_NEWENTRY);
		dentry = d_find_any_alias(inode);
		if (!dentry)
			break;
		next = igrab(d_inode(dentry->d_parent));
		dput(dentry);
		if (!next)
			break;
		iput(inode);
		inode = next;
		/*
		 * The directory inode may have gone through rmdir by now. But
		 * the inode itself and its blocks are still allocated (we hold
		 * a reference to the inode so it didn't go through
		 * pxt4_evict_inode()) and so we are safe to flush metadata
		 * blocks and the inode.
		 */
		ret = sync_mapping_buffers(inode->i_mapping);
		if (ret)
			break;
		ret = sync_inode_metadata(inode, 1);
		if (ret)
			break;
	}
	iput(inode);
	return ret;
}

static int pxt4_fsync_nojournal(struct inode *inode, bool datasync,
				bool *needs_barrier)
{
	int ret, err;

	ret = sync_mapping_buffers(inode->i_mapping);
	if (!(inode->i_state & I_DIRTY_ALL))
		return ret;
	if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return ret;

	err = sync_inode_metadata(inode, 1);
	if (!ret)
		ret = err;

	if (!ret)
		ret = pxt4_sync_parent(inode);
	if (test_opt(inode->i_sb, BARRIER))
		*needs_barrier = true;

	return ret;
}

static int pxt4_fsync_journal(struct inode *inode, bool datasync,
			     bool *needs_barrier)
{
	struct pxt4_inode_info *ei = PXT4_I(inode);
	journal_t *journal = PXT4_SB(inode->i_sb)->s_journal;
	tid_t commit_tid = datasync ? ei->i_datasync_tid : ei->i_sync_tid;

	if (journal->j_flags & JBD3_BARRIER &&
	    !jbd3_trans_will_send_data_barrier(journal, commit_tid))
		*needs_barrier = true;

	return jbd3_complete_transaction(journal, commit_tid);
}

/*
 * akpm: A new design for pxt4_sync_file().
 *
 * This is only called from sys_fsync(), sys_fdatasync() and sys_msync().
 * There cannot be a transaction open by this task.
 * Another task could have dirtied this inode.  Its data can be in any
 * state in the journalling system.
 *
 * What we do is just kick off a commit and wait on it.  This will snapshot the
 * inode to disk.
 */
int pxt4_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret = 0, err;
	bool needs_barrier = false;
	struct inode *inode = file->f_mapping->host;
	struct pxt4_sb_info *sbi = PXT4_SB(inode->i_sb);

	if (unlikely(pxt4_forced_shutdown(sbi)))
		return -EIO;

	J_ASSERT(pxt4_journal_current_handle() == NULL);

	trace_pxt4_sync_file_enter(file, datasync);

	if (sb_rdonly(inode->i_sb)) {
		/* Make sure that we read updated s_mount_flags value */
		smp_rmb();
		if (sbi->s_mount_flags & PXT4_MF_FS_ABORTED)
			ret = -EROFS;
		goto out;
	}

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	/*
	 * data=writeback,ordered:
	 *  The caller's filemap_fdatawrite()/wait will sync the data.
	 *  Metadata is in the journal, we wait for proper transaction to
	 *  commit here.
	 *
	 * data=journal:
	 *  filemap_fdatawrite won't do anything (the buffers are clean).
	 *  pxt4_force_commit will write the file data into the journal and
	 *  will wait on that.
	 *  filemap_fdatawait() will encounter a ton of newly-dirtied pages
	 *  (they were dirtied by commit).  But that's OK - the blocks are
	 *  safe in-journal, which is all fsync() needs to ensure.
	 */
	if (!sbi->s_journal)
		ret = pxt4_fsync_nojournal(inode, datasync, &needs_barrier);
	else if (pxt4_should_journal_data(inode))
		ret = pxt4_force_commit(inode->i_sb);
	else
		ret = pxt4_fsync_journal(inode, datasync, &needs_barrier);

	if (needs_barrier) {
		err = blkdev_issue_flush(inode->i_sb->s_bdev, GFP_KERNEL, NULL);
		if (!ret)
			ret = err;
	}
out:
	err = file_check_and_advance_wb_err(file);
	if (ret == 0)
		ret = err;
	trace_pxt4_sync_file_exit(inode, ret);
	return ret;
}
