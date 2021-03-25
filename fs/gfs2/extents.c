#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/gfs2_ondisk.h>

#include "gfs2.h"
#include "incore.h"
#include "inode.h"
#include "meta_io.h"
#include "trans.h"
#include "rgrp.h"
#include "glock.h"
#include "super.h"
#include "quota.h"
#include "extents.h"

/*
 * - calculate and check checksum
 */

/*
 * Extent-based inodes
 *
 * Inodes that have the GFS2_DIF_EXTENTS flag set use a b+-tree instead of
 * indirection blocks for mapping from offsets to disk blocks.  Those trees
 * consist of [0 .. height - 1] levels of index nodes, followed by the
 * leaf nodes.  The extent data is stored in the leaf nodes only.  All paths
 * in the tree have equal length.
 *
 * The entries in the index and leaf nodes are ordered by @ei_start and
 * @ex_start, respectively.  There may be holes between extents, but extents
 * won't overlap, and they won't cross resource groups.
 */

static u16
eh_entries(struct gfs2_extent_header *eh)
{
	return be16_to_cpu(eh->eh_entries);
}

static u64
ei_start(struct gfs2_extent_idx *ei)
{
	return be64_to_cpu(ei->ei_start);
}

static u64
ei_leaf(struct gfs2_extent_idx *ei)
{
	return be64_to_cpu(ei->ei_leaf);
}

static u64
ex_start(struct gfs2_extent *ex)
{
	return be64_to_cpu(ex->ex_start);
}

static u64
ex_addr(struct gfs2_extent *ex)
{
	return be64_to_cpu(ex->ex_addr);
}
static u16
ex_len(struct gfs2_extent *ex)
{
	return be16_to_cpu(ex->ex_len);
}

static u64
ex_last(struct gfs2_extent *ex)
{
	return ex_start(ex) + ex_len(ex) - 1;
}

static struct gfs2_extent_idx *
first_index(struct gfs2_extent_header *eh)
{
	return (struct gfs2_extent_idx *)(eh + 1);
}

static struct gfs2_extent_idx *
last_index(struct gfs2_extent_header *eh)
{
	struct gfs2_extent_idx *i = first_index(eh);

	return i + eh_entries(eh) - 1;
}

static struct gfs2_extent *
first_extent(struct gfs2_extent_header *eh)
{
	return (struct gfs2_extent *)(eh + 1);
}

static struct gfs2_extent *
last_extent(struct gfs2_extent_header *eh)
{
	struct gfs2_extent *ex = first_extent(eh);

	return ex + eh_entries(eh) - 1;
}

/*
 * The leaf path component is a list of extents; all other path components
 * are a list of indexes.
 */
struct gfs2_extent_pc {
	struct buffer_head *pc_bh;
	struct gfs2_extent_header *pc_eh;
	union {
		struct gfs2_extent_idx *pc_ei;
		struct gfs2_extent *pc_ex;
	};
};

/*
 * The path components in struct gfs2_extent_path are stored in "reverse"
 * order: the leaf is stored first, and when the tree grows at the root,
 * this happens at the end of the array.
 */
struct gfs2_extent_path {
	unsigned int p_height;
	struct gfs2_extent_pc p_pc[];
};

static struct gfs2_extent_pc *
first_path_component(struct gfs2_extent_path *path)
{
	return path->p_pc + path->p_height - 1;
}

static struct gfs2_extent_pc *
last_path_component(struct gfs2_extent_path *path)
{
	return path->p_pc;
}

static struct gfs2_extent *
path_extent(struct gfs2_extent_path *path)
{
	return last_path_component(path)->pc_ex;
}

#if 0
static struct buffer_head *
path_dibh(struct gfs2_extent_path *path)
{
	return first_path_component(path)->pc_bh;
}
#endif

void gfs2_free_ext_path(struct gfs2_extent_path *path)
{
	struct gfs2_extent_pc *pc;

	if (!path)
		return;

	pc = path->p_pc;
	if (path->p_height > 1)
		pc += path->p_height - 1;
	while (pc >= last_path_component(path)) {
		brelse(pc->pc_bh);
		pc->pc_bh = NULL;
		pc--;
	}
	kfree(path);
}

static int
path_iterate(struct gfs2_extent_path *path, unsigned int n)
{
	struct gfs2_extent_pc *pc = last_path_component(path);

	pc->pc_ex += n;
	if (pc->pc_ex > last_extent(pc->pc_eh)) {
		if (path->p_height != 1)
			return -EIO;  /* FIXME */
		pc->pc_ex = NULL;
	}
	return 0;
}

static int
path_iterate_done(struct gfs2_extent_path *path)
{
	struct gfs2_extent_pc *pc = last_path_component(path);

	pc->pc_ex = NULL;
	return 0;
}

static struct gfs2_extent_idx *
search_index(struct gfs2_extent_header *eh, u64 block)
{
	struct gfs2_extent_idx *l, *m, *r;

	if (eh->eh_entries == 0)
		return NULL;  /* tree invalid */

	l = first_index(eh) + 1;
	r = last_index(eh);
	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < ei_start(m))
			r = m - 1;
		else
			l = m + 1;
	}
	return l - 1;
}

static struct gfs2_extent *
search_extent(struct gfs2_extent_header *eh, u64 block)
{
	struct gfs2_extent *l, *m, *r;

	if (!eh->eh_entries)
		return NULL;  /* empty tree */

	l = first_extent(eh) + 1;
	r = last_extent(eh);
	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < ex_start(m))
			r = m - 1;
		else
			l = m + 1;
	}
	return l - 1;
}

static unsigned int
pc_size(struct gfs2_extent_pc *pc)
{
	struct buffer_head *bh = pc->pc_bh;
	void *end = bh->b_data + bh->b_size;

	return end - (void *)(pc->pc_eh + 1);
}

static unsigned int
max_extents(struct gfs2_extent_pc *pc)
{
	return pc_size(pc) / sizeof(struct gfs2_extent);
}

#if 0
static unsigned int
max_indexes(struct gfs2_extent_pc *pc)
{
	return pc_size(pc) / sizeof(struct gfs2_extent_idx);
}
#endif

static bool
leaf_needs_splitting(struct gfs2_extent_path *path)
{
	struct gfs2_extent_pc *pc = last_path_component(path);

	return eh_entries(pc->pc_eh) == max_extents(pc);
}

static int
verify_inode(struct gfs2_inode *ip, struct buffer_head *bh)
{
	struct gfs2_extent_header *eh =
			(void *)bh->b_data + sizeof(struct gfs2_dinode);
	u32 size = bh->b_size - sizeof(struct gfs2_dinode);
	u32 entries = eh_entries(eh);

	if (eh->eh_pad)
		return -EIO;
	if (ip->i_height == 1) {
		if (entries > size / sizeof(struct gfs2_extent))
			return -EIO;
	} else {
		if (entries > size / sizeof(struct gfs2_extent_idx))
			return -EIO;
	}
	return 0;
}

static int
verify_block(u32 mtype, struct buffer_head *bh)
{
	struct gfs2_meta_header *mh = (void *)bh->b_data;
	struct gfs2_extent_header *eh = (void *)(mh + 1);
	u32 size = bh->b_size - sizeof(struct gfs2_meta_header);
	u32 entries = eh_entries(eh);

	if (eh->eh_pad)
		return -EIO;
	switch(mtype) {
	case GFS2_METATYPE_XL:
		if (entries > size / sizeof(struct gfs2_extent))
			return -EIO;
		break;
	case GFS2_METATYPE_XI:
		if (entries > size / sizeof(struct gfs2_extent_idx))
			return -EIO;
		break;
	}
	return 0;
}

/**
 * find_extent - look up the path to an extent
 *
 * Upon success, a new path into the extent tree is returned.  In this path,
 * pc_ex of the last path component is NULL if the tree is empty, and
 * points to the closest extent starting before or at @block otherwise.
 *
 * Return: an ERR_PTR upon failure.
 */
static struct gfs2_extent_path *
find_extent(struct gfs2_inode *ip, u64 block)
{
	struct gfs2_extent_path *path;
	struct gfs2_extent_pc *pc, *last_pc;
	u32 mtype = GFS2_METATYPE_XI;
	int ret;

	if (!gfs2_has_extents(ip) || ip->i_height < 1)
		return ERR_PTR(-EINVAL);

	path = kzalloc(flex_array_size(path, p_pc, ip->i_height), GFP_NOFS);
	if (!path)
		return ERR_PTR(-ENOMEM);
	path->p_height = ip->i_height;
	pc = first_path_component(path);
	last_pc = last_path_component(path);
	ret = gfs2_meta_inode_buffer(ip, &pc->pc_bh);
	if (ret)
		goto fail;
	ret = verify_inode(ip, pc->pc_bh);
	if (ret)
		goto fail;
	pc->pc_eh = (void *)pc->pc_bh->b_data + sizeof(struct gfs2_dinode);
	while (pc != last_pc) {
		struct gfs2_extent_idx *ei;

		ei = search_index(pc->pc_eh, block);
		if (!ei) {
			ret = -EIO;
			goto fail;
		}
		pc->pc_ei = ei;
		pc--;
		if (pc == last_pc)
			mtype = GFS2_METATYPE_XL;
		ret = gfs2_meta_buffer(ip, mtype, ei_leaf(ei), &pc->pc_bh);
		if (ret)
			goto fail;
		ret = verify_block(mtype, pc->pc_bh);
		if (ret)
			goto fail;
		pc->pc_eh = (void *)pc->pc_bh->b_data + sizeof(struct gfs2_meta_header);
	}
	last_pc->pc_ex = search_extent(last_pc->pc_eh, block);
	return path;

fail:
	gfs2_free_ext_path(path);
	return ERR_PTR(ret);
}

static bool extent_includes(struct gfs2_extent *ex, u64 block)
{
	u64 start = ex_start(ex);

	return block >= start && block < start + ex_len(ex);
}

static bool
extents_can_be_merged(struct gfs2_extent *left, struct gfs2_extent *right)
{
	u16 len = ex_len(left);

	if (ex_start(left) + len != ex_start(right))
		return false;
	if (ex_addr(left) + len != ex_addr(right))
		return false;
	if (left->ex_flags != right->ex_flags)
		return false;
	return true;
}

static void
remove_extent_at(struct gfs2_extent *ex, struct gfs2_extent_header *eh)
{
	struct gfs2_extent *last_ex = last_extent(eh);

	memmove(ex, ex + 1, (last_ex - ex) * sizeof(*ex));
	memset(last_ex, 0, sizeof(*last_ex));
	be16_add_cpu(&eh->eh_entries, -1);
}

static void
insert_extent_at(struct gfs2_extent *ex, struct gfs2_extent_header *eh)
{
	struct gfs2_extent *last_ex = last_extent(eh);

	memmove(ex + 1, ex, (last_ex - ex + 1) * sizeof(*ex));
	be16_add_cpu(&eh->eh_entries, 1);
}

/**
 * insert_extent - 
 *
 * non-overlapping
 */
static int
insert_extent(struct gfs2_inode *ip, struct gfs2_extent_path *path,
	      struct gfs2_extent *new_ex)
{
	struct gfs2_extent_pc *pc;
	struct gfs2_extent *ex, *last_ex;
	struct gfs2_extent_header *eh;

	if (path->p_height != 1)
		return -EIO;

	pc = last_path_component(path);
	eh = pc->pc_eh;
	ex = pc->pc_ex;
	last_ex = last_extent(eh);
	if (!ex) {
		/* empty tree */
		BUG_ON(eh->eh_entries);
		*first_extent(eh) = *new_ex;
		eh->eh_entries = cpu_to_be16(1);
		return 0;
	}

	if (ex_start(ex) < ex_start(new_ex)) {
		if (extents_can_be_merged(ex, new_ex)) {
			/* append */
			be16_add_cpu(&ex->ex_len, ex_len(new_ex));
			goto merge_right;
		}

		if (ex != last_ex && extents_can_be_merged(new_ex, ex + 1)) {
			ex++;
			goto prepend;
		}
		ex++;
	} else {
		if (extents_can_be_merged(new_ex, ex))
			goto prepend;
	}

	/* insert */
	if (leaf_needs_splitting(path))
		return -ENOSPC;
	insert_extent_at(ex, eh);
	*ex = *new_ex;
	return 0;

prepend:
	ex->ex_start = new_ex->ex_start;
	ex->ex_addr = new_ex->ex_addr;
	be16_add_cpu(&ex->ex_len, ex_len(new_ex));
	return 0;

merge_right:
	if (ex < last_ex && extents_can_be_merged(ex, ex + 1)) {
		be16_add_cpu(&ex->ex_len, ex_len(ex + 1));
		remove_extent_at(ex + 1, eh);
	}
	return 0;
}

static u64
path_next_block(struct gfs2_extent_path *path)
{
	struct gfs2_extent_pc *first_pc, *pc;
	struct gfs2_extent_idx *ei;
	struct gfs2_extent *ex;

	if (!path->p_height)
		return 0;  /* stuffed inode */

	pc = last_path_component(path);
	ex = pc->pc_ex;
	if (!ex)
		return 0;  /* empty tree */
	if (ex != last_extent(pc->pc_eh))
		return ex_start(ex + 1);

	first_pc = first_path_component(path);
	for (pc++; pc <= first_pc; pc++) {
		ei = pc->pc_ei;
		if (ei != last_index(pc->pc_eh))
			return ei_start(ei + 1);
	}
	return 0;
}

/**
 * __gfs2_extent_iomap_get - Map blocks from an inode to disk blocks
 * @inode: The inode
 * @pos: Starting position in bytes
 * @length: Length to map in bytes
 * @flags: iomap flags
 * @iomap: The iomap structure
 * @pathp: The extent path
 *
 * Return: errno
 */
int __gfs2_extent_iomap_get(struct inode *inode, loff_t pos,
			    loff_t length, unsigned flags,
			    struct iomap *iomap,
			    struct gfs2_extent_path **pathp)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	loff_t size = i_size_read(inode);
	struct gfs2_extent_path *path = NULL;
	struct gfs2_extent *ex = NULL;
	u64 next_block = 0;
	u64 mask;
	int ret = 0;

	if (!length)
		return -EINVAL;

	mask = (1 << inode->i_blkbits) - 1;
	iomap->offset = pos & ~mask;
	iomap->length = ((pos & mask) + length + mask) & ~mask;

	down_read(&ip->i_rw_mutex);

	if (gfs2_is_stuffed(ip)) {
		struct buffer_head *dibh;

		*pathp = path = kzalloc(flex_array_size(path, p_pc, 1), GFP_NOFS);
		if (!path) {
			ret = -ENOMEM;
			goto unlock;
		}
		ret = gfs2_meta_inode_buffer(ip, &dibh);
		if (ret)
			goto unlock;
		last_path_component(path)->pc_bh = dibh;

		if (flags & IOMAP_WRITE) {
			loff_t max_size = gfs2_max_stuffed_size(ip);

			if (pos + length > max_size)
				goto do_alloc;
			iomap->length = max_size;
		} else {
			if (pos >= size) {
				if (flags & IOMAP_REPORT) {
					ret = -ENOENT;
					goto unlock;
				}
				goto hole_found;
			}
			iomap->length = size;
		}

		iomap->addr = (ip->i_no_addr << inode->i_blkbits) +
			      sizeof(struct gfs2_dinode);
		iomap->type = IOMAP_INLINE;
		iomap->inline_data = dibh->b_data + sizeof(struct gfs2_dinode);
		goto out;
	}

	path = find_extent(ip, pos >> inode->i_blkbits);
	if (IS_ERR(path)) {
		ret = PTR_ERR(path);
		goto unlock;
	}
	*pathp = path;

	ex = last_path_component(path)->pc_ex;
	if (!ex || !extent_includes(ex, pos >> inode->i_blkbits))
		goto do_alloc;

	iomap->offset = ex_start(ex) << inode->i_blkbits;
	iomap->addr = ex_addr(ex) << inode->i_blkbits;
	iomap->length = (u64)ex_len(ex) << inode->i_blkbits;
	iomap->type = IOMAP_MAPPED;
	if (ex->ex_flags & cpu_to_be16(GFS2_EF_UNWRITTEN))
		iomap->type = IOMAP_UNWRITTEN;

out:
	iomap->bdev = inode->i_sb->s_bdev;
unlock:
	up_read(&ip->i_rw_mutex);
	return ret;

do_alloc:
	if (flags & IOMAP_REPORT) {
		if (pos >= size) {
			ret = -ENOENT;
			goto unlock;
		}
	} else if (flags & IOMAP_WRITE) {
		if (flags & IOMAP_DIRECT)
			goto hole_found;  /* (see gfs2_file_direct_write) */

		/* Limit allocations to a reasonable size.  */
		if (iomap->length > 1024 * PAGE_SIZE)
			iomap->length = 1024 * PAGE_SIZE;
	} else {
		if (pos >= size)
			goto hole_found;
	}

	if (ex) {
		/* @ex can be before or after @pos. */
		next_block = ex_start(ex);
		if (next_block < pos >> inode->i_blkbits)
			next_block = path_next_block(path);
	}
	if (next_block) {
		u64 end = next_block << inode->i_blkbits;
		if (iomap->length > end - iomap->offset)
			iomap->length = end - iomap->offset;
	}

hole_found:
	iomap->addr = IOMAP_NULL_ADDR;
	iomap->type = IOMAP_HOLE;
	goto out;
}

/**
 * __gfs2_extent_iomap_alloc - 
 * @inode: The GFS2 inode
 * @iomap: The iomap structure
 * @path: The extent path
 *
 * Return: errno
 */
int __gfs2_extent_iomap_alloc(struct inode *inode, struct iomap *iomap,
			      struct gfs2_extent_path *path)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct buffer_head *dibh;
	struct gfs2_extent ex;
	u64 addr;
	unsigned int len;
	int ret;

	dibh = last_path_component(path)->pc_bh;
	gfs2_trans_add_meta(ip->i_gl, dibh);

	down_write(&ip->i_rw_mutex);
	len = iomap->length >> inode->i_blkbits;
	ret = gfs2_alloc_blocks(ip, &addr, &len, 0, NULL);
	if (ret)
		goto out;
	ex.ex_start = cpu_to_be64(iomap->offset >> inode->i_blkbits);
	ex.ex_addr = cpu_to_be64(addr);
	ex.ex_len = cpu_to_be16(len);
	ex.ex_flags = 0;
	ret = insert_extent(ip, path, &ex);
	if (ret) {
		struct gfs2_rgrpd *rgd = gfs2_blk2rgrpd(sdp, addr, true);
		__gfs2_free_blocks(ip, rgd, addr, len, false);
		goto out;
	}
	if (gfs2_is_jdata(ip))
		gfs2_trans_remove_revoke(sdp, addr, len);
	gfs2_add_inode_blocks(&ip->i_inode, len);
	gfs2_dinode_out(ip, dibh->b_data);

	iomap->addr = addr << inode->i_blkbits;
	iomap->flags |= IOMAP_F_NEW;
	iomap->type = IOMAP_MAPPED;
	iomap->length = (u64)len << inode->i_blkbits;

out:
	up_write(&ip->i_rw_mutex);
	return ret;
}

static int
punch_hole_remove_range(struct gfs2_inode *ip, struct gfs2_extent_path *path,
			u64 start, u16 len)
{
	struct gfs2_extent_pc *pc = last_path_component(path);
	struct gfs2_extent_header *eh = pc->pc_eh;
	struct gfs2_extent *ex = pc->pc_ex;
	u64 last = start + len - 1;
	u64 exlast = ex_last(ex);


	if (start == ex_start(ex)) {
		if (exlast == last) {
			remove_extent_at(ex, eh);
			return path_iterate(path, 0);
		}
		/* tail of extent remains */
		ex->ex_start = cpu_to_be64(last + 1);
		be64_add_cpu(&ex->ex_addr, len);
		ex->ex_len = cpu_to_be16(exlast - last);
	} else {
		struct gfs2_extent *next = ex + 1;

		if (exlast == last) {
			/* head of extent remains */
			ex->ex_len = cpu_to_be16(start - ex_start(ex));
			return path_iterate(path, 1);
		}

		/* head and tail of extent remain */
		if (leaf_needs_splitting(path))
			return -ENOSPC;
		ex->ex_len = cpu_to_be16(start - ex_start(ex));
		insert_extent_at(next, eh);
		next->ex_start = cpu_to_be64(last + 1);
		next->ex_addr = cpu_to_be64(ex_addr(ex) +
					    last + 1 - ex_start(ex));
		next->ex_len = cpu_to_be16(exlast - last);
		next->ex_flags = ex->ex_flags;
	}
	return path_iterate_done(path);
}

struct punch_hole_context {
	struct gfs2_holder rd_gh;
	struct gfs2_rgrpd *rgd;
	u16 blocks_freed;
};

static int
gfs2_punch_hole_commit(struct gfs2_inode *ip, struct punch_hole_context *ctx)
{
	struct inode *inode = &ip->i_inode;
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct buffer_head *dibh;
	int ret;

	ret = gfs2_meta_inode_buffer(ip, &dibh);
	if (ret)
		goto out;
	inode->i_mtime = inode_set_ctime_current(inode);
	gfs2_add_inode_blocks(inode, -ctx->blocks_freed);
	gfs2_trans_add_meta(ip->i_gl, dibh);
	gfs2_dinode_out(ip, dibh->b_data);
	brelse(dibh);
	gfs2_statfs_change(sdp, 0, ctx->blocks_freed, 0);
	gfs2_quota_change(ip, -ctx->blocks_freed, inode->i_uid, inode->i_gid);

out:
	up_write(&ip->i_rw_mutex);
	gfs2_trans_end(sdp);
	return ret;
}

static int
punch_hole_in_extent(struct gfs2_inode *ip, struct gfs2_extent_path *path,
		     u64 start, u16 len, struct punch_hole_context *ctx)
{
	struct inode *inode = &ip->i_inode;
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct gfs2_extent *ex = path_extent(path);
	unsigned int blks, revokes;
	struct gfs2_rgrpd *rgd = ctx->rgd;
	u64 addr = ex_addr(ex) + start - ex_start(ex);
	int ret;

	if (rgd) {
		if (!rgrp_contains_block(rgd, addr))
			return -EAGAIN;
	} else {
		rgd = gfs2_blk2rgrpd(sdp, addr, true);
		if (!rgd)
			return -EIO;
		ret = gfs2_glock_nq_init(rgd->rd_gl, LM_ST_EXCLUSIVE,
					 LM_FLAG_NODE_SCOPE, &ctx->rd_gh);
		if (ret)
			return ret;
		ctx->rgd = rgd;

		/* Must be done with the rgrp glock held: */
		if (gfs2_rs_active(&ip->i_res) &&
		    rgd == ip->i_res.rs_rgd)
			gfs2_rs_deltree(&ip->i_res);
	}

	/*
	 * Extents do not span resource groups, so in the worst case, deleting
	 * an extent will affect all bitmap blocks in a resource group.
	 */
	blks = RES_DINODE + RES_RG_HDR +
	       DIV_ROUND_UP(len, sdp->sd_blocks_per_bitmap) +
	       RES_STATFS + RES_QUOTA;
	revokes = len;
	if (!gfs2_trans_grow(sdp, blks, revokes)) {
		if (current->journal_info) {
			ret = gfs2_punch_hole_commit(ip, ctx);
			if (ret)
				return ret;
			cond_resched();
		}
		printk(KERN_ERR "%s gfs2_trans_begin(%u, %u)\n", __func__, blks, revokes);
		ret = gfs2_trans_begin(sdp, blks, revokes);
		if (ret)
			return ret;
		printk(KERN_ERR "%s down_write(%lld)", __func__, ip->i_no_addr);
		down_write(&ip->i_rw_mutex);
	}

	__gfs2_free_blocks(ip, rgd, addr, len, false);
	punch_hole_remove_range(ip, path, start, len);
	ctx->blocks_freed += len;
	return 0;
}

int
gfs2_extent_punch_hole(struct gfs2_inode *ip, u64 offset, u64 length)
{
	struct punch_hole_context ctx = { .rd_gh = HOLDER_UNITIALIZED };
	struct inode *inode = &ip->i_inode;
	struct gfs2_extent_path *path;
	struct gfs2_extent *ex;
	bool extents_skipped;
	u64 start, last;
	int ret = 0;

	if (!length)
		length = -offset;
	start = (offset + i_blocksize(inode) - 1) >> inode->i_blkbits;
	last = (offset + length - 1) >> inode->i_blkbits;
	if (start > last)
		return 0;

repeat:
	extents_skipped = false;
	path = find_extent(ip, start >> inode->i_blkbits);
	if (IS_ERR(path)) {
		ret = PTR_ERR(path);
		path = NULL;
		goto out;
	}
	ex = path_extent(path);
	if (!ex)
	       goto out;
	if (ex_last(ex) < start) {
		ret = path_iterate(path, 1);
		if (ret)
			goto out;
	}

	while (ex = path_extent(path), ex && ex_start(ex) <= last) {
		u64 exlast = min(ex_last(ex), last);
		u16 exlen;

		if (start < ex_start(ex))
			start = ex_start(ex);
		exlen = exlast + 1 - start;
		ret = punch_hole_in_extent(ip, path, start, exlen, &ctx);
		if (ret) {
			if (ret != -EAGAIN)
				break;
			ret = 0;
			extents_skipped = true;
			path_iterate(path, 1);
		}
	}

out:
	gfs2_free_ext_path(path);
	if (current->journal_info) {
		int ret2 = gfs2_punch_hole_commit(ip, &ctx);
		if (!ret)
			ret = ret2;
	}
	if (gfs2_holder_initialized(&ctx.rd_gh))
		gfs2_glock_dq_uninit(&ctx.rd_gh);
	if (!ret && extents_skipped)
		goto repeat;
	return ret;
}
