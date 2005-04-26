/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#ifndef __LOG_DOT_H__
#define __LOG_DOT_H__

/**
 * gfs2_log_lock - acquire the right to mess with the log manager
 * @sdp: the filesystem
 *
 */

static __inline__ void
gfs2_log_lock(struct gfs2_sbd *sdp)
{
	spin_lock(&sdp->sd_log_lock);
}

/**
 * gfs2_log_unlock - release the right to mess with the log manager
 * @sdp: the filesystem
 *
 */

static __inline__ void
gfs2_log_unlock(struct gfs2_sbd *sdp)
{
	spin_unlock(&sdp->sd_log_lock);
}

/**
 * log_incr_head - Increment journal head (next block to fill in journal)
 * @sdp: The GFS2 superblock
 *
 */

static __inline__ void
gfs2_log_pointers_init(struct gfs2_sbd *sdp, unsigned int value)
{
	if (++value == sdp->sd_jdesc->jd_blocks) {
		value = 0;
		sdp->sd_log_wraps++;
	}
	sdp->sd_log_head = sdp->sd_log_tail = value;
}

void gfs2_lock_for_flush(struct gfs2_sbd *sdp);
void gfs2_unlock_from_flush(struct gfs2_sbd *sdp);

unsigned int gfs2_struct2blk(struct gfs2_sbd *sdp, unsigned int nstruct,
			    unsigned int ssize);

void gfs2_ail1_start(struct gfs2_sbd *sdp, int flags);
int gfs2_ail1_empty(struct gfs2_sbd *sdp, int flags);

int gfs2_log_reserve(struct gfs2_sbd *sdp, unsigned int blks);
void gfs2_log_release(struct gfs2_sbd *sdp, unsigned int blks);

struct gfs2_log_buf *gfs2_log_get_buf(struct gfs2_sbd *sdp);
struct gfs2_log_buf *gfs2_log_fake_buf(struct gfs2_sbd *sdp, struct buffer_head *real);

#define gfs2_log_flush(sdp) gfs2_log_flush_i((sdp), NULL)
#define gfs2_log_flush_glock(gl) gfs2_log_flush_i((gl)->gl_sbd, (gl))
void gfs2_log_flush_i(struct gfs2_sbd *sdp, struct gfs2_glock *gl);
void gfs2_log_commit(struct gfs2_sbd *sdp, struct gfs2_trans *trans);

void gfs2_log_shutdown(struct gfs2_sbd *sdp);

#endif /* __LOG_DOT_H__ */
