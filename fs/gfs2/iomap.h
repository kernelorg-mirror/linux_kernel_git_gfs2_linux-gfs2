#ifndef __IOMAP_DOT_H__
#define __IOMAP_DOT_H__

#include <linux/iomap.h>

struct gfs2_inode;
struct gfs2_sbd;
struct gfs2_jdesc;

int gfs2_unstuff_dinode(struct gfs2_inode *ip);
int gfs2_block_map(struct inode *inode, sector_t lblock,
		   struct buffer_head *bh, int create);
int gfs2_iomap_get(struct inode *inode, loff_t pos, loff_t length,
		   struct iomap *iomap);
int gfs2_iomap_alloc(struct inode *inode, loff_t pos, loff_t length,
		     struct iomap *iomap);
int gfs2_get_extent(struct inode *inode, u64 lblock, u64 *dblock,
		    unsigned int *extlen);
int gfs2_alloc_extent(struct inode *inode, u64 lblock, u64 *dblock,
		      unsigned *extlen, bool *new);
int gfs2_block_zero_range(struct inode *inode, loff_t from,
			  unsigned int length);

int gfs2_map_journal_extents(struct gfs2_sbd *sdp, struct gfs2_jdesc *jd);
void gfs2_free_journal_extents(struct gfs2_jdesc *jd);

extern const struct iomap_ops gfs2_iomap_ops;
extern const struct iomap_writeback_ops gfs2_writeback_ops;

#endif  /* __IOMAP_DOT_H__ */
