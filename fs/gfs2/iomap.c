#include <linux/gfs2_ondisk.h>
#include <linux/iomap.h>

#include "gfs2.h"
#include "incore.h"
#include "iomap.h"
#include "bmap.h"
#include "glock.h"
#include "rgrp.h"
#include "trans.h"
#include "log.h"
#include "aops.h"
#include "quota.h"
#include "trace_gfs2.h"
#include "meta_io.h"
#include "dir.h"

/**
 * gfs2_unstuffer_page - unstuff a stuffed inode into a block cached by a page
 * @ip: the inode
 * @dibh: the dinode buffer
 * @block: the block number that was allocated
 * @page: The (optional) page. This is looked up if @page is NULL
 *
 * Returns: errno
 */

static int gfs2_unstuffer_page(struct gfs2_inode *ip, struct buffer_head *dibh,
			       u64 block, struct page *page)
{
	struct inode *inode = &ip->i_inode;

	if (!PageUptodate(page)) {
		void *kaddr = kmap(page);
		u64 dsize = i_size_read(inode);

		memcpy(kaddr, dibh->b_data + sizeof(struct gfs2_dinode), dsize);
		memset(kaddr + dsize, 0, PAGE_SIZE - dsize);
		kunmap(page);

		SetPageUptodate(page);
	}

	if (gfs2_is_jdata(ip)) {
		struct buffer_head *bh;

		if (!page_has_buffers(page))
			create_empty_buffers(page, BIT(inode->i_blkbits),
					     BIT(BH_Uptodate));

		bh = page_buffers(page);
		if (!buffer_mapped(bh))
			map_bh(bh, inode->i_sb, block);

		set_buffer_uptodate(bh);
		gfs2_trans_add_data(ip->i_gl, bh);
	} else {
		set_page_dirty(page);
		gfs2_ordered_add_inode(ip);
	}

	return 0;
}

static int __gfs2_unstuff_inode(struct gfs2_inode *ip, struct page *page)
{
	struct buffer_head *bh, *dibh;
	struct gfs2_dinode *di;
	u64 block = 0;
	int isdir = gfs2_is_dir(ip);
	int error;

	error = gfs2_meta_inode_buffer(ip, &dibh);
	if (error)
		return error;

	if (i_size_read(&ip->i_inode)) {
		/* Get a free block, fill it with the stuffed data,
		   and write it out to disk */

		unsigned int n = 1;
		error = gfs2_alloc_blocks(ip, &block, &n, 0, NULL);
		if (error)
			goto out_brelse;
		if (isdir) {
			gfs2_trans_remove_revoke(GFS2_SB(&ip->i_inode), block, 1);
			error = gfs2_dir_get_new_buffer(ip, block, &bh);
			if (error)
				goto out_brelse;
			gfs2_buffer_copy_tail(bh, sizeof(struct gfs2_meta_header),
					      dibh, sizeof(struct gfs2_dinode));
			brelse(bh);
		} else {
			error = gfs2_unstuffer_page(ip, dibh, block, page);
			if (error)
				goto out_brelse;
		}
	}

	/*  Set up the pointer to the new block  */

	gfs2_trans_add_meta(ip->i_gl, dibh);
	di = (struct gfs2_dinode *)dibh->b_data;
	gfs2_buffer_clear_tail(dibh, sizeof(struct gfs2_dinode));

	if (i_size_read(&ip->i_inode)) {
		*(__be64 *)(di + 1) = cpu_to_be64(block);
		gfs2_add_inode_blocks(&ip->i_inode, 1);
		di->di_blocks = cpu_to_be64(gfs2_get_inode_blocks(&ip->i_inode));
	}

	ip->i_height = 1;
	di->di_height = cpu_to_be16(1);

out_brelse:
	brelse(dibh);
	return error;
}

/**
 * gfs2_unstuff_dinode - Unstuff a dinode when the data has grown too big
 * @ip: The GFS2 inode to unstuff
 *
 * This routine unstuffs a dinode and returns it to a "normal" state such
 * that the height can be grown in the traditional way.
 *
 * Returns: errno
 */

int gfs2_unstuff_dinode(struct gfs2_inode *ip)
{
	struct inode *inode = &ip->i_inode;
	struct page *page;
	int error;

	down_write(&ip->i_rw_mutex);
	page = grab_cache_page(inode->i_mapping, 0);
	error = -ENOMEM;
	if (!page)
		goto out;
	error = __gfs2_unstuff_inode(ip, page);
	unlock_page(page);
	put_page(page);
out:
	up_write(&ip->i_rw_mutex);
	return error;
}

static struct folio *
gfs2_iomap_get_folio(struct iomap_iter *iter, loff_t pos, unsigned len)
{
	struct inode *inode = iter->inode;
	unsigned int blockmask = i_blocksize(inode) - 1;
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	unsigned int blocks;
	struct folio *folio;
	int status;

	blocks = ((pos & blockmask) + len + blockmask) >> inode->i_blkbits;
	status = gfs2_trans_begin(sdp, RES_DINODE + blocks, 0);
	if (status)
		return ERR_PTR(status);

	folio = iomap_get_folio(iter, pos, len);
	if (IS_ERR(folio))
		gfs2_trans_end(sdp);
	return folio;
}

static void gfs2_iomap_put_folio(struct inode *inode, loff_t pos,
				 unsigned copied, struct folio *folio)
{
	struct gfs2_trans *tr = current->journal_info;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);

	if (!gfs2_is_stuffed(ip))
		gfs2_trans_add_databufs(ip, folio, offset_in_folio(folio, pos),
					copied);

	folio_unlock(folio);
	folio_put(folio);

	if (tr->tr_num_buf_new)
		__mark_inode_dirty(inode, I_DIRTY_DATASYNC);

	gfs2_trans_end(sdp);
}

static const struct iomap_folio_ops gfs2_iomap_folio_ops = {
	.get_folio = gfs2_iomap_get_folio,
	.put_folio = gfs2_iomap_put_folio,
};

static int gfs2_iomap_begin_write(struct inode *inode, loff_t pos,
				  loff_t length, unsigned flags,
				  struct iomap *iomap,
				  struct metapath *mp)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	bool unstuff;
	int ret;

	unstuff = gfs2_is_stuffed(ip) &&
		  pos + length > gfs2_max_stuffed_size(ip);

	if (unstuff || iomap->type == IOMAP_HOLE) {
		unsigned int data_blocks, ind_blocks;
		struct gfs2_alloc_parms ap = {};
		unsigned int rblocks;
		struct gfs2_trans *tr;

		gfs2_write_calc_reserv(ip, iomap->length, &data_blocks,
				       &ind_blocks);
		ap.target = data_blocks + ind_blocks;
		ret = gfs2_quota_lock_check(ip, &ap);
		if (ret)
			return ret;

		ret = gfs2_inplace_reserve(ip, &ap);
		if (ret)
			goto out_qunlock;

		rblocks = RES_DINODE + ind_blocks;
		if (gfs2_is_jdata(ip))
			rblocks += data_blocks;
		if (ind_blocks || data_blocks)
			rblocks += RES_STATFS + RES_QUOTA;
		if (inode == sdp->sd_rindex)
			rblocks += 2 * RES_STATFS;
		rblocks += gfs2_rg_blocks(ip, data_blocks + ind_blocks);

		ret = gfs2_trans_begin(sdp, rblocks,
				       iomap->length >> inode->i_blkbits);
		if (ret)
			goto out_trans_fail;

		if (unstuff) {
			ret = gfs2_unstuff_dinode(ip);
			if (ret)
				goto out_trans_end;
			release_metapath(mp);
			ret = __gfs2_iomap_get(inode, iomap->offset,
					       iomap->length, flags, iomap, mp);
			if (ret)
				goto out_trans_end;
		}

		if (iomap->type == IOMAP_HOLE) {
			ret = __gfs2_iomap_alloc(inode, iomap, mp);
			if (ret) {
				gfs2_trans_end(sdp);
				gfs2_inplace_release(ip);
				punch_hole(ip, iomap->offset, iomap->length);
				goto out_qunlock;
			}
		}

		tr = current->journal_info;
		if (tr->tr_num_buf_new)
			__mark_inode_dirty(inode, I_DIRTY_DATASYNC);

		gfs2_trans_end(sdp);
	}

	if (gfs2_is_stuffed(ip) || gfs2_is_jdata(ip))
		iomap->folio_ops = &gfs2_iomap_folio_ops;
	return 0;

out_trans_end:
	gfs2_trans_end(sdp);
out_trans_fail:
	gfs2_inplace_release(ip);
out_qunlock:
	gfs2_quota_unlock(ip);
	return ret;
}

static int gfs2_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			    unsigned flags, struct iomap *iomap,
			    struct iomap *srcmap)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct metapath mp = { .mp_aheight = 1, };
	int ret;

	if (gfs2_is_jdata(ip))
		iomap->flags |= IOMAP_F_BUFFER_HEAD;

	trace_gfs2_iomap_start(ip, pos, length, flags);
	ret = __gfs2_iomap_get(inode, pos, length, flags, iomap, &mp);
	if (ret)
		goto out_unlock;

	switch(flags & (IOMAP_WRITE | IOMAP_ZERO)) {
	case IOMAP_WRITE:
		if (flags & IOMAP_DIRECT) {
			/*
			 * Silently fall back to buffered I/O for stuffed files
			 * or if we've got a hole (see gfs2_file_direct_write).
			 */
			if (iomap->type != IOMAP_MAPPED)
				ret = -ENOTBLK;
			goto out_unlock;
		}
		break;
	case IOMAP_ZERO:
		if (iomap->type == IOMAP_HOLE)
			goto out_unlock;
		break;
	default:
		goto out_unlock;
	}

	ret = gfs2_iomap_begin_write(inode, pos, length, flags, iomap, &mp);

out_unlock:
	release_metapath(&mp);
	trace_gfs2_iomap_end(ip, iomap, ret);
	return ret;
}

static int gfs2_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			  ssize_t written, unsigned flags, struct iomap *iomap)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);

	switch (flags & (IOMAP_WRITE | IOMAP_ZERO)) {
	case IOMAP_WRITE:
		if (flags & IOMAP_DIRECT)
			return 0;
		break;
	case IOMAP_ZERO:
		 if (iomap->type == IOMAP_HOLE)
			 return 0;
		 break;
	default:
		 return 0;
	}

	if (!gfs2_is_stuffed(ip))
		gfs2_ordered_add_inode(ip);

	if (inode == sdp->sd_rindex)
		adjust_fs_space(inode);

	gfs2_inplace_release(ip);

	if (ip->i_qadata && ip->i_qadata->qa_qd_num)
		gfs2_quota_unlock(ip);

	if (length != written && (iomap->flags & IOMAP_F_NEW)) {
		/* Deallocate blocks that were just allocated. */
		loff_t hstart = round_up(pos + written, i_blocksize(inode));
		loff_t hend = iomap->offset + iomap->length;

		if (hstart < hend) {
			truncate_pagecache_range(inode, hstart, hend - 1);
			punch_hole(ip, hstart, hend - hstart);
		}
	}

	if (unlikely(!written))
		return 0;

	if (iomap->flags & IOMAP_F_SIZE_CHANGED)
		mark_inode_dirty(inode);
	set_bit(GLF_DIRTY, &ip->i_gl->gl_flags);
	return 0;
}

const struct iomap_ops gfs2_iomap_ops = {
	.iomap_begin = gfs2_iomap_begin,
	.iomap_end = gfs2_iomap_end,
};

/**
 * gfs2_block_map - Map one or more blocks of an inode to a disk block
 * @inode: The inode
 * @lblock: The logical block number
 * @bh_map: The bh to be mapped
 * @create: True if its ok to alloc blocks to satify the request
 *
 * The size of the requested mapping is defined in bh_map->b_size.
 *
 * Clears buffer_mapped(bh_map) and leaves bh_map->b_size unchanged
 * when @lblock is not mapped.  Sets buffer_mapped(bh_map) and
 * bh_map->b_size to indicate the size of the mapping when @lblock and
 * successive blocks are mapped, up to the requested size.
 *
 * Sets buffer_boundary() if a read of metadata will be required
 * before the next block can be mapped. Sets buffer_new() if new
 * blocks were allocated.
 *
 * Returns: errno
 */

int gfs2_block_map(struct inode *inode, sector_t lblock,
		   struct buffer_head *bh_map, int create)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	loff_t pos = (loff_t)lblock << inode->i_blkbits;
	loff_t length = bh_map->b_size;
	struct iomap iomap = { };
	int ret;

	clear_buffer_mapped(bh_map);
	clear_buffer_new(bh_map);
	clear_buffer_boundary(bh_map);
	trace_gfs2_bmap(ip, bh_map, lblock, create, 1);

	if (!create)
		ret = gfs2_iomap_get(inode, pos, length, &iomap);
	else
		ret = gfs2_iomap_alloc(inode, pos, length, &iomap);
	if (ret)
		goto out;

	if (iomap.length > bh_map->b_size) {
		iomap.length = bh_map->b_size;
		iomap.flags &= ~IOMAP_F_GFS2_BOUNDARY;
	}
	if (iomap.addr != IOMAP_NULL_ADDR)
		map_bh(bh_map, inode->i_sb, iomap.addr >> inode->i_blkbits);
	bh_map->b_size = iomap.length;
	if (iomap.flags & IOMAP_F_GFS2_BOUNDARY)
		set_buffer_boundary(bh_map);
	if (iomap.flags & IOMAP_F_NEW)
		set_buffer_new(bh_map);

out:
	trace_gfs2_bmap(ip, bh_map, lblock, create, ret);
	return ret;
}

int gfs2_get_extent(struct inode *inode, u64 lblock, u64 *dblock,
		    unsigned int *extlen)
{
	unsigned int blkbits = inode->i_blkbits;
	struct iomap iomap = { };
	unsigned int len;
	int ret;

	ret = gfs2_iomap_get(inode, lblock << blkbits, *extlen << blkbits,
			     &iomap);
	if (ret)
		return ret;
	if (iomap.type != IOMAP_MAPPED)
		return -EIO;
	*dblock = iomap.addr >> blkbits;
	len = iomap.length >> blkbits;
	if (len < *extlen)
		*extlen = len;
	return 0;
}

int gfs2_alloc_extent(struct inode *inode, u64 lblock, u64 *dblock,
		      unsigned int *extlen, bool *new)
{
	unsigned int blkbits = inode->i_blkbits;
	struct iomap iomap = { };
	unsigned int len;
	int ret;

	ret = gfs2_iomap_alloc(inode, lblock << blkbits, *extlen << blkbits,
			       &iomap);
	if (ret)
		return ret;
	if (iomap.type != IOMAP_MAPPED)
		return -EIO;
	*dblock = iomap.addr >> blkbits;
	len = iomap.length >> blkbits;
	if (len < *extlen)
		*extlen = len;
	*new = iomap.flags & IOMAP_F_NEW;
	return 0;
}

/*
 * NOTE: Never call gfs2_block_zero_range with an open transaction because it
 * uses iomap write to perform its actions, which begin their own transactions
 * (iomap_begin, get_folio, etc.)
 */
int gfs2_block_zero_range(struct inode *inode, loff_t from,
			  unsigned int length)
{
	BUG_ON(current->journal_info);
	return iomap_zero_range(inode, from, length, NULL, &gfs2_iomap_ops);
}

int gfs2_iomap_get(struct inode *inode, loff_t pos, loff_t length,
		   struct iomap *iomap)
{
	struct metapath mp = { .mp_aheight = 1, };
	int ret;

	ret = __gfs2_iomap_get(inode, pos, length, 0, iomap, &mp);
	release_metapath(&mp);
	return ret;
}

int gfs2_iomap_alloc(struct inode *inode, loff_t pos, loff_t length,
		     struct iomap *iomap)
{
	struct metapath mp = { .mp_aheight = 1, };
	int ret;

	ret = __gfs2_iomap_get(inode, pos, length, IOMAP_WRITE, iomap, &mp);
	if (!ret && iomap->type == IOMAP_HOLE)
		ret = __gfs2_iomap_alloc(inode, iomap, &mp);
	release_metapath(&mp);
	return ret;
}

/**
 * gfs2_free_journal_extents - Free cached journal bmap info
 * @jd: The journal
 *
 */

void gfs2_free_journal_extents(struct gfs2_jdesc *jd)
{
	struct gfs2_journal_extent *jext;

	while(!list_empty(&jd->extent_list)) {
		jext = list_first_entry(&jd->extent_list, struct gfs2_journal_extent, list);
		list_del(&jext->list);
		kfree(jext);
	}
}

/**
 * gfs2_add_jextent - Add or merge a new extent to extent cache
 * @jd: The journal descriptor
 * @lblock: The logical block at start of new extent
 * @dblock: The physical block at start of new extent
 * @blocks: Size of extent in fs blocks
 *
 * Returns: 0 on success or -ENOMEM
 */

static int gfs2_add_jextent(struct gfs2_jdesc *jd, u64 lblock, u64 dblock, u64 blocks)
{
	struct gfs2_journal_extent *jext;

	if (!list_empty(&jd->extent_list)) {
		jext = list_last_entry(&jd->extent_list, struct gfs2_journal_extent, list);
		if ((jext->dblock + jext->blocks) == dblock) {
			jext->blocks += blocks;
			return 0;
		}
	}

	jext = kzalloc(sizeof(struct gfs2_journal_extent), GFP_NOFS);
	if (jext == NULL)
		return -ENOMEM;
	jext->dblock = dblock;
	jext->lblock = lblock;
	jext->blocks = blocks;
	list_add_tail(&jext->list, &jd->extent_list);
	jd->nr_extents++;
	return 0;
}

/**
 * gfs2_map_journal_extents - Cache journal bmap info
 * @sdp: The super block
 * @jd: The journal to map
 *
 * Create a reusable "extent" mapping from all logical
 * blocks to all physical blocks for the given journal.  This will save
 * us time when writing journal blocks.  Most journals will have only one
 * extent that maps all their logical blocks.  That's because gfs2.mkfs
 * arranges the journal blocks sequentially to maximize performance.
 * So the extent would map the first block for the entire file length.
 * However, gfs2_jadd can happen while file activity is happening, so
 * those journals may not be sequential.  Less likely is the case where
 * the users created their own journals by mounting the metafs and
 * laying it out.  But it's still possible.  These journals might have
 * several extents.
 *
 * Returns: 0 on success, or error on failure
 */

int gfs2_map_journal_extents(struct gfs2_sbd *sdp, struct gfs2_jdesc *jd)
{
	u64 lblock = 0;
	u64 lblock_stop;
	struct gfs2_inode *ip = GFS2_I(jd->jd_inode);
	struct buffer_head bh;
	unsigned int shift = sdp->sd_sb.sb_bsize_shift;
	u64 size;
	int rc;
	ktime_t start, end;

	start = ktime_get();
	lblock_stop = i_size_read(jd->jd_inode) >> shift;
	size = (lblock_stop - lblock) << shift;
	jd->nr_extents = 0;
	WARN_ON(!list_empty(&jd->extent_list));

	do {
		bh.b_state = 0;
		bh.b_blocknr = 0;
		bh.b_size = size;
		rc = gfs2_block_map(jd->jd_inode, lblock, &bh, 0);
		if (rc || !buffer_mapped(&bh))
			goto fail;
		rc = gfs2_add_jextent(jd, lblock, bh.b_blocknr, bh.b_size >> shift);
		if (rc)
			goto fail;
		size -= bh.b_size;
		lblock += (bh.b_size >> ip->i_inode.i_blkbits);
	} while(size > 0);

	end = ktime_get();
	fs_info(sdp, "journal %d mapped with %u extents in %lldms\n", jd->jd_jid,
		jd->nr_extents, ktime_ms_delta(end, start));
	return 0;

fail:
	fs_warn(sdp, "error %d mapping journal %u at offset %llu (extent %u)\n",
		rc, jd->jd_jid,
		(unsigned long long)(i_size_read(jd->jd_inode) - size),
		jd->nr_extents);
	fs_warn(sdp, "bmap=%d lblock=%llu block=%llu, state=0x%08lx, size=%llu\n",
		rc, (unsigned long long)lblock, (unsigned long long)bh.b_blocknr,
		bh.b_state, (unsigned long long)bh.b_size);
	gfs2_free_journal_extents(jd);
	return rc;
}

static int gfs2_map_blocks(struct iomap_writepage_ctx *wpc, struct inode *inode,
		loff_t offset)
{
	int ret;

	if (WARN_ON_ONCE(gfs2_is_stuffed(GFS2_I(inode))))
		return -EIO;

	if (offset >= wpc->iomap.offset &&
	    offset < wpc->iomap.offset + wpc->iomap.length)
		return 0;

	memset(&wpc->iomap, 0, sizeof(wpc->iomap));
	ret = gfs2_iomap_get(inode, offset, INT_MAX, &wpc->iomap);
	return ret;
}

const struct iomap_writeback_ops gfs2_writeback_ops = {
	.map_blocks		= gfs2_map_blocks,
};
