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

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>

#include "gfs2.h"
#include "dio.h"
#include "glock.h"
#include "log.h"
#include "lops.h"
#include "recovery.h"
#include "rgrp.h"
#include "trans.h"

static void
glock_lo_add(struct gfs2_sbd *sdp, struct gfs2_log_element *le)
{
	ENTER(G2FN_GLOCK_LO_ADD)
	struct gfs2_glock *gl;

	current_transaction->tr_touched = TRUE;

	if (!list_empty(&le->le_list))
		RET(G2FN_GLOCK_LO_ADD);

	gl = container_of(le, struct gfs2_glock, gl_le);
	if (gfs2_assert_withdraw(sdp, gfs2_glock_is_held_excl(gl)))
		RET(G2FN_GLOCK_LO_ADD);
	gfs2_glock_hold(gl);
	set_bit(GLF_DIRTY, &gl->gl_flags);

	gfs2_log_lock(sdp);
	sdp->sd_log_num_gl++;
	list_add(&le->le_list, &sdp->sd_log_le_gl);
	gfs2_log_unlock(sdp);

	RET(G2FN_GLOCK_LO_ADD);
}

static void
glock_lo_after_commit(struct gfs2_sbd *sdp, struct gfs2_ail *ai)
{
	ENTER(G2FN_GLOCK_LO_AFTER_COMMIT)
	struct list_head *head = &sdp->sd_log_le_gl;
	struct gfs2_glock *gl;

	gfs2_log_lock(sdp);

	while (!list_empty(head)) {
		gl = list_entry(head->next, struct gfs2_glock, gl_le.le_list);
		list_del_init(head->next);
		sdp->sd_log_num_gl--;
		gfs2_log_unlock(sdp);

		gfs2_assert_withdraw(sdp, gfs2_glock_is_held_excl(gl));
		gfs2_glock_put(gl);

		gfs2_log_lock(sdp);
	}
	gfs2_assert_warn(sdp, !sdp->sd_log_num_gl);

	gfs2_log_unlock(sdp);

	RET(G2FN_GLOCK_LO_AFTER_COMMIT);
}

static void
buf_lo_add(struct gfs2_sbd *sdp, struct gfs2_log_element *le)
{
	ENTER(G2FN_BUF_LO_ADD)
	struct gfs2_bufdata *bd = container_of(le, struct gfs2_bufdata, bd_le);
	struct gfs2_trans *tr;

	if (!list_empty(&bd->bd_list_tr))
		RET(G2FN_BUF_LO_ADD);

	tr = current_transaction;
	tr->tr_touched = TRUE;
	tr->tr_num_buf++;
	list_add(&bd->bd_list_tr, &tr->tr_list_buf);

	if (!list_empty(&le->le_list))
		RET(G2FN_BUF_LO_ADD);

	gfs2_trans_add_gl(bd->bd_gl);

	gfs2_meta_check(sdp, bd->bd_bh);
	gfs2_dpin(sdp, bd->bd_bh);

	gfs2_log_lock(sdp);
	sdp->sd_log_num_buf++;
	list_add(&le->le_list, &sdp->sd_log_le_buf);
	gfs2_log_unlock(sdp);

	tr->tr_num_buf_new++;

	RET(G2FN_BUF_LO_ADD);
}

static void
buf_lo_incore_commit(struct gfs2_sbd *sdp, struct gfs2_trans *tr)
{
	ENTER(G2FN_BUF_LO_INCORE_COMMIT)
	struct list_head *head = &tr->tr_list_buf;

	while (!list_empty(head)) {
		list_del_init(head->next);
		tr->tr_num_buf--;
	}
	gfs2_assert_warn(sdp, !tr->tr_num_buf);

	RET(G2FN_BUF_LO_INCORE_COMMIT);
}

static void
buf_lo_build_entry(struct gfs2_sbd *sdp)
{
	ENTER(G2FN_BUF_LO_BUILD_ENTRY)
	struct gfs2_log_descriptor ld;
	struct gfs2_log_buf *lb;
	struct list_head *tmp, *head;
        struct gfs2_bufdata *bd;

	if (!sdp->sd_log_num_buf)
		RET(G2FN_BUF_LO_BUILD_ENTRY);

	lb = gfs2_log_get_buf(sdp);
	memset(&ld, 0, sizeof(struct gfs2_log_descriptor));
	ld.ld_header.mh_magic = GFS2_MAGIC;
	ld.ld_header.mh_type = GFS2_METATYPE_LD;
	ld.ld_header.mh_blkno = lb->lb_bh.b_blocknr;
	ld.ld_header.mh_format = GFS2_FORMAT_LD;
	ld.ld_type = GFS2_LOG_DESC_METADATA;
	ld.ld_length = sdp->sd_log_num_buf + 1;
	ld.ld_data1 = sdp->sd_log_num_buf;
	gfs2_log_descriptor_out(&ld, lb->lb_bh.b_data);

	gfs2_log_lock(sdp);

	for (head = &sdp->sd_log_le_buf, tmp = head->next;
	     tmp != head;
	     tmp = tmp->next) {
		bd = list_entry(tmp, struct gfs2_bufdata, bd_le.le_list);
		gfs2_log_unlock(sdp);

		gfs2_log_fake_buf(sdp, bd->bd_bh);

		gfs2_log_lock(sdp);
	}

	gfs2_log_unlock(sdp);

	RET(G2FN_BUF_LO_BUILD_ENTRY);
}

static void
buf_lo_after_commit(struct gfs2_sbd *sdp, struct gfs2_ail *ai)
{
	ENTER(G2FN_BUF_LO_AFTER_COMMIT)
	struct list_head *head = &sdp->sd_log_le_buf;
	struct gfs2_bufdata *bd;

	gfs2_log_lock(sdp);

	while (!list_empty(head)) {
		bd = list_entry(head->next, struct gfs2_bufdata, bd_le.le_list);
		list_del_init(head->next);
		sdp->sd_log_num_buf--;
		gfs2_log_unlock(sdp);

		gfs2_dunpin(sdp, bd->bd_bh, ai);

		gfs2_log_lock(sdp);
	}
	gfs2_assert_warn(sdp, !sdp->sd_log_num_buf);

	gfs2_log_unlock(sdp);

	RET(G2FN_BUF_LO_AFTER_COMMIT);
}

static void
buf_lo_before_scan(struct gfs2_jdesc *jd, struct gfs2_log_header *head,
		   int pass)
{
	ENTER(G2FN_BUF_LO_BEFORE_SCAN)
       	struct gfs2_sbd *sdp = jd->jd_inode->i_sbd;

	if (pass != 0)
		RET(G2FN_BUF_LO_BEFORE_SCAN);

	sdp->sd_found_blocks = 0;
	sdp->sd_replayed_blocks = 0;

	RET(G2FN_BUF_LO_BEFORE_SCAN);
}

static int
buf_lo_scan_elements(struct gfs2_jdesc *jd, unsigned int start,
		     struct gfs2_log_descriptor *ld, int pass)
{
	ENTER(G2FN_BUF_LO_SCAN_ELEMENTS)
	struct gfs2_sbd *sdp = jd->jd_inode->i_sbd;
	struct gfs2_glock *gl = jd->jd_inode->i_gl;
	unsigned int blks = ld->ld_data1;
	struct buffer_head *bh_log, *bh_ip;
	uint64_t blkno;
	int error = 0;

	if (pass != 1 || ld->ld_type != GFS2_LOG_DESC_METADATA)
		RETURN(G2FN_BUF_LO_SCAN_ELEMENTS, 0);

	gfs2_replay_incr_blk(sdp, &start);

	for (; blks; gfs2_replay_incr_blk(sdp, &start), blks--) {
		error = gfs2_replay_read_block(jd, start, &bh_log);
		if (error)
			RETURN(G2FN_BUF_LO_SCAN_ELEMENTS, error);

		blkno = ((struct gfs2_meta_header *)bh_log->b_data)->mh_blkno;
		blkno = gfs2_64_to_cpu(blkno);

		sdp->sd_found_blocks++;

		if (gfs2_revoke_check(sdp, blkno, start)) {
			brelse(bh_log);
			continue;
		}

		bh_ip = gfs2_dgetblk(gl, blkno);
		memcpy(bh_ip->b_data, bh_log->b_data, bh_log->b_size);
		set_buffer_uptodate(bh_ip);

		if (gfs2_meta_check(sdp, bh_ip))
			error = -EIO;
		else
			error = gfs2_dwrite(sdp, bh_ip, DIO_DIRTY);

		brelse(bh_log);
		brelse(bh_ip);

		if (error)
			break;

		sdp->sd_replayed_blocks++;
	}

	RETURN(G2FN_BUF_LO_SCAN_ELEMENTS, error);
}

static void
buf_lo_after_scan(struct gfs2_jdesc *jd, int error, int pass)
{
	ENTER(G2FN_BUF_LO_AFTER_SCAN)
	struct gfs2_sbd *sdp = jd->jd_inode->i_sbd;

	if (error) {
		gfs2_sync_buf(jd->jd_inode->i_gl, DIO_START | DIO_WAIT);
		RET(G2FN_BUF_LO_AFTER_SCAN);
	}
	if (pass != 1)
		RET(G2FN_BUF_LO_AFTER_SCAN);

	gfs2_sync_buf(jd->jd_inode->i_gl, DIO_START | DIO_WAIT);

	printk("GFS2: fsid=%s: jid=%u: Replayed %u of %u blocks\n",
	       sdp->sd_fsname, jd->jd_jid,
	       sdp->sd_replayed_blocks,
	       sdp->sd_found_blocks);

	RET(G2FN_BUF_LO_AFTER_SCAN);
}

static void
revoke_lo_add(struct gfs2_sbd *sdp, struct gfs2_log_element *le)
{
	ENTER(G2FN_REVOKE_LO_ADD)
	struct gfs2_trans *tr;

	tr = current_transaction;
	tr->tr_touched = TRUE;
	tr->tr_num_revoke++;

	gfs2_log_lock(sdp);
	sdp->sd_log_num_revoke++;
	list_add(&le->le_list, &sdp->sd_log_le_revoke);
	gfs2_log_unlock(sdp);

	RET(G2FN_REVOKE_LO_ADD);
}

static void
revoke_lo_build_entry(struct gfs2_sbd *sdp)
{
	ENTER(G2FN_REVOKE_LO_BUILD_ENTRY)
	struct gfs2_log_descriptor ld;
	struct gfs2_meta_header *mh = &ld.ld_header;
	struct gfs2_log_buf *lb;
	unsigned int offset;
	struct list_head *head = &sdp->sd_log_le_revoke;
        struct gfs2_revoke *rv;

	if (!sdp->sd_log_num_revoke)
		RET(G2FN_REVOKE_LO_BUILD_ENTRY);

	lb = gfs2_log_get_buf(sdp);
	memset(&ld, 0, sizeof(struct gfs2_log_descriptor));
	ld.ld_header.mh_magic = GFS2_MAGIC;
	ld.ld_header.mh_type = GFS2_METATYPE_LD;
	ld.ld_header.mh_blkno = lb->lb_bh.b_blocknr;
	ld.ld_header.mh_format = GFS2_FORMAT_LD;
	ld.ld_type = GFS2_LOG_DESC_REVOKE;
	ld.ld_length = gfs2_struct2blk(sdp, sdp->sd_log_num_revoke, sizeof(uint64_t));
	ld.ld_data1 = sdp->sd_log_num_revoke;
	gfs2_log_descriptor_out(&ld, lb->lb_bh.b_data);
	offset = sizeof(struct gfs2_log_descriptor);

	gfs2_log_lock(sdp);

	while (!list_empty(head)) {
		rv = list_entry(head->next, struct gfs2_revoke, rv_le.le_list);
		list_del(&rv->rv_le.le_list);
		sdp->sd_log_num_revoke--;
		gfs2_log_unlock(sdp);

		if (offset + sizeof(uint64_t) > sdp->sd_sb.sb_bsize) {
			lb = gfs2_log_get_buf(sdp);
			mh->mh_type = GFS2_METATYPE_LB;
			mh->mh_blkno = lb->lb_bh.b_blocknr;
			mh->mh_format = GFS2_FORMAT_LB;
			gfs2_meta_header_out(mh, lb->lb_bh.b_data);
			offset = sizeof(struct gfs2_meta_header);
		}

		*(uint64_t *)(lb->lb_bh.b_data + offset) = cpu_to_gfs2_64(rv->rv_blkno);
		kfree(rv);

		offset += sizeof(uint64_t);

		gfs2_log_lock(sdp);
	}
	gfs2_assert_withdraw(sdp, !sdp->sd_log_num_revoke);

	gfs2_log_unlock(sdp);

       	RET(G2FN_REVOKE_LO_BUILD_ENTRY);
}

static void
revoke_lo_before_scan(struct gfs2_jdesc *jd, struct gfs2_log_header *head,
		      int pass)
{
	ENTER(G2FN_REVOKE_LO_BEFORE_SCAN)
       	struct gfs2_sbd *sdp = jd->jd_inode->i_sbd;

       	if (pass != 0)
		RET(G2FN_REVOKE_LO_BEFORE_SCAN);

	sdp->sd_found_revokes = 0;
	sdp->sd_replay_tail = head->lh_tail;

       	RET(G2FN_REVOKE_LO_BEFORE_SCAN);
}

static int
revoke_lo_scan_elements(struct gfs2_jdesc *jd, unsigned int start,
			struct gfs2_log_descriptor *ld, int pass)
{
	ENTER(G2FN_REVOKE_LO_SCAN_ELEMENTS)
	struct gfs2_sbd *sdp = jd->jd_inode->i_sbd;
	unsigned int blks = ld->ld_length;
	unsigned int revokes = ld->ld_data1;
	struct buffer_head *bh;
	unsigned int offset;
	uint64_t blkno;
	int first = TRUE;
	int error;

	if (pass != 0 || ld->ld_type != GFS2_LOG_DESC_REVOKE)
		RETURN(G2FN_REVOKE_LO_SCAN_ELEMENTS, 0);

	offset = sizeof(struct gfs2_log_descriptor);

	for (; blks; gfs2_replay_incr_blk(sdp, &start), blks--) {
		error = gfs2_replay_read_block(jd, start, &bh);
		if (error)
			RETURN(G2FN_REVOKE_LO_SCAN_ELEMENTS, error);

		if (!first)
			gfs2_metatype_check(sdp, bh, GFS2_METATYPE_LB);

		while (offset + sizeof(uint64_t) <= sdp->sd_sb.sb_bsize) {
			blkno = *(uint64_t *)(bh->b_data + offset);
			blkno = gfs2_64_to_cpu(blkno);

			error = gfs2_revoke_add(sdp, blkno, start);
			if (error < 0)
				RETURN(G2FN_REVOKE_LO_SCAN_ELEMENTS, error);
			else if (error)
				sdp->sd_found_revokes++;

			if (!--revokes)
				break;
			offset += sizeof(uint64_t);
		}

		brelse(bh);
		offset = sizeof(struct gfs2_meta_header);
		first = FALSE;
	}

       	RETURN(G2FN_REVOKE_LO_SCAN_ELEMENTS, 0);
}

static void
revoke_lo_after_scan(struct gfs2_jdesc *jd, int error, int pass)
{
	ENTER(G2FN_REVOKE_LO_AFTER_SCAN)
       	struct gfs2_sbd *sdp = jd->jd_inode->i_sbd;

	if (error) {
		gfs2_revoke_clean(sdp);
		RET(G2FN_REVOKE_LO_AFTER_SCAN);
	}
       	if (pass != 1)
		RET(G2FN_REVOKE_LO_AFTER_SCAN);

	printk("GFS2: fsid=%s: jid=%u: Found %u revoke tags\n",
	       sdp->sd_fsname, jd->jd_jid,
	       sdp->sd_found_revokes);

	gfs2_revoke_clean(sdp);

       	RET(G2FN_REVOKE_LO_AFTER_SCAN);
}

static void
rg_lo_add(struct gfs2_sbd *sdp, struct gfs2_log_element *le)
{
	ENTER(G2FN_RG_LO_ADD)
	struct gfs2_rgrpd *rgd;

	current_transaction->tr_touched = TRUE;

	if (!list_empty(&le->le_list))
		RET(G2FN_RG_LO_ADD);

	rgd = container_of(le, struct gfs2_rgrpd, rd_le);
	gfs2_rgrp_bh_hold(rgd);

	gfs2_log_lock(sdp);
	sdp->sd_log_num_rg++;
	list_add(&le->le_list, &sdp->sd_log_le_rg);
	gfs2_log_unlock(sdp);	

	RET(G2FN_RG_LO_ADD);
}

static void
rg_lo_after_commit(struct gfs2_sbd *sdp, struct gfs2_ail *ai)
{
	ENTER(G2FN_RG_LO_AFTER_COMMIT)
	struct list_head *head = &sdp->sd_log_le_rg;
	struct gfs2_rgrpd *rgd;

	gfs2_log_lock(sdp);

	while (!list_empty(head)) {
		rgd = list_entry(head->next, struct gfs2_rgrpd, rd_le.le_list);
		list_del_init(head->next);
		sdp->sd_log_num_rg--;
		gfs2_log_unlock(sdp);

		gfs2_rgrp_repolish_clones(rgd);
		gfs2_rgrp_bh_put(rgd);

		gfs2_log_lock(sdp);
	}
	gfs2_assert_warn(sdp, !sdp->sd_log_num_rg);

	gfs2_log_unlock(sdp);

	RET(G2FN_RG_LO_AFTER_COMMIT);
}

static void
databuf_lo_add(struct gfs2_sbd *sdp, struct gfs2_log_element *le)
{
	ENTER(G2FN_DATABUF_LO_ADD)

	gfs2_log_lock(sdp);
	sdp->sd_log_num_databuf++;
	list_add(&le->le_list, &sdp->sd_log_le_databuf);
	gfs2_log_unlock(sdp);

	RET(G2FN_DATABUF_LO_ADD);
}

static void
databuf_lo_before_commit(struct gfs2_sbd *sdp)
{
	ENTER(G2FN_DATABUF_LO_BEFORE_COMMIT)
	struct list_head *head = &sdp->sd_log_le_databuf;
	LIST_HEAD(started);
	struct gfs2_databuf *db;

	gfs2_log_lock(sdp);

	while (!list_empty(head)) {
		db = list_entry(head->prev, struct gfs2_databuf, db_le.le_list);
		list_del_init(head->prev);
		sdp->sd_log_num_databuf--;
		gfs2_log_unlock(sdp);

		gfs2_dwrite(sdp, db->db_bh, DIO_START);
		list_add(&db->db_le.le_list, &started);

		gfs2_log_lock(sdp);
	}
	gfs2_assert_warn(sdp, !sdp->sd_log_num_databuf);

	gfs2_log_unlock(sdp);

	while (!list_empty(&started)) {
		db = list_entry(started.next, struct gfs2_databuf, db_le.le_list);
		list_del(started.next);
		gfs2_dwrite(sdp, db->db_bh, DIO_WAIT);
		brelse(db->db_bh);
		kfree(db);
	}

	RET(G2FN_DATABUF_LO_BEFORE_COMMIT);
}

struct gfs2_log_operations gfs2_glock_lops = {
	.lo_add = glock_lo_add,
	.lo_after_commit = glock_lo_after_commit,
	.lo_name = "glock"
};

struct gfs2_log_operations gfs2_buf_lops = {
	.lo_add = buf_lo_add,
	.lo_incore_commit = buf_lo_incore_commit,
	.lo_build_entry = buf_lo_build_entry,
	.lo_after_commit = buf_lo_after_commit,
	.lo_before_scan = buf_lo_before_scan,
	.lo_scan_elements = buf_lo_scan_elements,
	.lo_after_scan = buf_lo_after_scan,
	.lo_name = "buf"
};

struct gfs2_log_operations gfs2_revoke_lops = {
	.lo_add = revoke_lo_add,
	.lo_build_entry = revoke_lo_build_entry,
	.lo_before_scan = revoke_lo_before_scan,
	.lo_scan_elements = revoke_lo_scan_elements,
	.lo_after_scan = revoke_lo_after_scan,
	.lo_name = "revoke"
};

struct gfs2_log_operations gfs2_rg_lops = {
	.lo_add = rg_lo_add,
	.lo_after_commit = rg_lo_after_commit,
	.lo_name = "rg"
};

struct gfs2_log_operations gfs2_databuf_lops = {
	.lo_add = databuf_lo_add,
	.lo_before_commit = databuf_lo_before_commit,
	.lo_name = "databuf"
};

struct gfs2_log_operations *gfs2_log_ops[] = {
	&gfs2_glock_lops,
	&gfs2_buf_lops,
	&gfs2_revoke_lops,
	&gfs2_rg_lops,
	&gfs2_databuf_lops,
	NULL
};
