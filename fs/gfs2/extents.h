#ifndef __EXTENTS_DOT_H__
#define __EXTENTS_DOT_H__

#include <linux/fs.h>
#include <linux/iomap.h>

struct gfs2_extent_path;

extern void gfs2_free_ext_path(struct gfs2_extent_path *path);
extern int __gfs2_extent_iomap_get(struct inode *inode, loff_t pos,
				   loff_t length, unsigned flags,
				   struct iomap *iomap,
				   struct gfs2_extent_path **pathp);
extern int __gfs2_extent_iomap_alloc(struct inode *inode, struct iomap *iomap,
				     struct gfs2_extent_path *path);

extern int gfs2_extent_punch_hole(struct gfs2_inode *ip, u64 offset, u64 length);

#endif  /* __EXTENTS_DOT_H__ */
