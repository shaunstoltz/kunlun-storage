/*****************************************************************************

Copyright (c) 1996, 2019, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file trx/trx0undo.cc
 Transaction undo log

 Created 3/26/1996 Heikki Tuuri
 *******************************************************/

#include <stddef.h>

#include <sql_thd_internal_api.h>

#include "fsp0fsp.h"
#include "ha_prototypes.h"
#include "trx0undo.h"

#include "my_dbug.h"

#ifndef UNIV_HOTBACKUP
#include "clone0clone.h"
#include "current_thd.h"
#include "dict0dd.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "sql/sql_class.h" // THD::OWNED_SIDNO_ANONYMOUS

/* How should the old versions in the history list be managed?
   ----------------------------------------------------------
If each transaction is given a whole page for its update undo log, file
space consumption can be 10 times higher than necessary. Therefore,
partly filled update undo log pages should be reusable. But then there
is no way individual pages can be ordered so that the ordering agrees
with the serialization numbers of the transactions on the pages. Thus,
the history list must be formed of undo logs, not their header pages as
it was in the old implementation.
        However, on a single header page the transactions are placed in
the order of their serialization numbers. As old versions are purged, we
may free the page when the last transaction on the page has been purged.
        A problem is that the purge has to go through the transactions
in the serialization order. This means that we have to look through all
rollback segments for the one that has the smallest transaction number
in its history list.
        When should we do a purge? A purge is necessary when space is
running out in any of the rollback segments. Then we may have to purge
also old version which might be needed by some consistent read. How do
we trigger the start of a purge? When a transaction writes to an undo log,
it may notice that the space is running out. When a read view is closed,
it may make some history superfluous. The server can have an utility which
periodically checks if it can purge some history.
        In a parallellized purge we have the problem that a query thread
can remove a delete marked clustered index record before another query
thread has processed an earlier version of the record, which cannot then
be done because the row cannot be constructed from the clustered index
record. To avoid this problem, we will store in the update and delete mark
undo record also the columns necessary to construct the secondary index
entries which are modified.
        We can latch the stack of versions of a single clustered index record
by taking a latch on the clustered index page. As long as the latch is held,
no new versions can be added and no versions removed by undo. But, a purge
can still remove old versions from the bottom of the stack. */

/* How to protect rollback segments, undo logs, and history lists with
   -------------------------------------------------------------------
latches?
-------
The contention of the trx_sys_t::mutex should be minimized. When a transaction
does its first insert or modify in an index, an undo log is assigned for it.
Then we must have an x-latch to the rollback segment header.
        When the transaction does more modifys or rolls back, the undo log is
protected with undo_mutex in the transaction.
        When the transaction commits, its insert undo log is either reset and
cached for a fast reuse, or freed. In these cases we must have an x-latch on
the rollback segment page. The update undo log is put to the history list. If
it is not suitable for reuse, its slot in the rollback segment is reset. In
both cases, an x-latch must be acquired on the rollback segment.
        The purge operation steps through the history list without modifying
it until a truncate operation occurs, which can remove undo logs from the end
of the list and release undo log segments. In stepping through the list,
s-latches on the undo log pages are enough, but in a truncate, x-latches must
be obtained on the rollback segment and individual pages. */
#endif /* !UNIV_HOTBACKUP */

/** Initializes the fields in an undo log segment page. */
static void trx_undo_page_init(
    page_t *undo_page, /*!< in: undo log segment page */
    ulint type,        /*!< in: undo log segment type */
    mtr_t *mtr);       /*!< in: mtr */

#ifndef UNIV_HOTBACKUP
/** Creates and initializes an undo log memory object.
@param[in]   rseg     rollback segment memory object
@param[in]   id       slot index within rseg
@param[in]   type     type of the log: TRX_UNDO_INSERT or TRX_UNDO_UPDATE
@param[in]   trx_id   id of the trx for which the undo log is created
@param[in]   xid      X/Open XA transaction identification
@param[in]   page_no  undo log header page number
@param[in]   offset   undo log header byte offset on page
@return own: the undo log memory object */
static trx_undo_t *trx_undo_mem_create(trx_rseg_t *rseg, ulint id, ulint type,
                                       trx_id_t trx_id, const XID *xid,
                                       page_no_t page_no, ulint offset);
#endif /* !UNIV_HOTBACKUP */
/** Initializes a cached insert undo log header page for new use. NOTE that this
 function has its own log record type MLOG_UNDO_HDR_REUSE. You must NOT change
 the operation of this function!
 @return undo log header byte offset on page */
static ulint trx_undo_insert_header_reuse(
    page_t *undo_page, /*!< in/out: insert undo log segment
                       header page, x-latched */
    trx_id_t trx_id,   /*!< in: transaction id */
    mtr_t *mtr);       /*!< in: mtr */

static void init_xa_gtid_slots(trx_t *trx, trx_ulogf_t *undo_header, trx_undo_t *undo, mtr_t *mtr);
trx_t *&thd_to_trx(THD *thd);
extern int ddc_mode;

#ifndef UNIV_HOTBACKUP
/** Gets the previous record in an undo log from the previous page.
 @return undo log record, the page s-latched, NULL if none */
static trx_undo_rec_t *trx_undo_get_prev_rec_from_prev_page(
    trx_undo_rec_t *rec, /*!< in: undo record */
    page_no_t page_no,   /*!< in: undo log header page number */
    ulint offset,        /*!< in: undo log header offset on page */
    bool shared,         /*!< in: true=S-latch, false=X-latch */
    mtr_t *mtr)          /*!< in: mtr */
{
  space_id_t space;
  page_no_t prev_page_no;
  page_t *prev_page;
  page_t *undo_page;

  undo_page = page_align(rec);

  prev_page_no = flst_get_prev_addr(
                     undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr)
                     .page;

  if (prev_page_no == FIL_NULL) {
    return (NULL);
  }

  space = page_get_space_id(undo_page);

  bool found;
  const page_size_t &page_size = fil_space_get_page_size(space, &found);

  ut_ad(found);

  buf_block_t *block = buf_page_get(page_id_t(space, prev_page_no), page_size,
                                    shared ? RW_S_LATCH : RW_X_LATCH, mtr);

  buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

  prev_page = buf_block_get_frame(block);

  return (trx_undo_page_get_last_rec(prev_page, page_no, offset));
}

/** Gets the previous record in an undo log.
 @return undo log record, the page s-latched, NULL if none */
trx_undo_rec_t *trx_undo_get_prev_rec(
    trx_undo_rec_t *rec, /*!< in: undo record */
    page_no_t page_no,   /*!< in: undo log header page number */
    ulint offset,        /*!< in: undo log header offset on page */
    bool shared,         /*!< in: true=S-latch, false=X-latch */
    mtr_t *mtr)          /*!< in: mtr */
{
  trx_undo_rec_t *prev_rec;

  prev_rec = trx_undo_page_get_prev_rec(rec, page_no, offset);

  if (prev_rec) {
    return (prev_rec);
  }

  /* We have to go to the previous undo log page to look for the
  previous record */

  return (
      trx_undo_get_prev_rec_from_prev_page(rec, page_no, offset, shared, mtr));
}

/** Gets the next record in an undo log from the next page.
@param[in]	space		undo log header space
@param[in]	page_size	page size
@param[in]	undo_page	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset on page
@param[in]	mode		latch mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]	mtr		mini-transaction
@return undo log record, the page latched, NULL if none */
static trx_undo_rec_t *trx_undo_get_next_rec_from_next_page(
    space_id_t space, const page_size_t &page_size, const page_t *undo_page,
    page_no_t page_no, ulint offset, ulint mode, mtr_t *mtr) {
  const trx_ulogf_t *log_hdr;
  page_no_t next_page_no;
  page_t *next_page;
  ulint next;

  if (page_no == page_get_page_no(undo_page)) {
    log_hdr = undo_page + offset;
    next = mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG);

    if (next != 0) {
      return (NULL);
    }
  }

  next_page_no = flst_get_next_addr(
                     undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr)
                     .page;
  if (next_page_no == FIL_NULL) {
    return (NULL);
  }

  const page_id_t next_page_id(space, next_page_no);

  if (mode == RW_S_LATCH) {
    next_page = trx_undo_page_get_s_latched(next_page_id, page_size, mtr);
  } else {
    ut_ad(mode == RW_X_LATCH);
    next_page = trx_undo_page_get(next_page_id, page_size, mtr);
  }

  return (trx_undo_page_get_first_rec(next_page, page_no, offset));
}

/** Gets the next record in an undo log.
 @return undo log record, the page s-latched, NULL if none */
trx_undo_rec_t *trx_undo_get_next_rec(
    trx_undo_rec_t *rec, /*!< in: undo record */
    page_no_t page_no,   /*!< in: undo log header page number */
    ulint offset,        /*!< in: undo log header offset on page */
    mtr_t *mtr)          /*!< in: mtr */
{
  space_id_t space;
  trx_undo_rec_t *next_rec;

  next_rec = trx_undo_page_get_next_rec(rec, page_no, offset);

  if (next_rec) {
    return (next_rec);
  }

  space = page_get_space_id(page_align(rec));

  bool found;
  const page_size_t &page_size = fil_space_get_page_size(space, &found);

  ut_ad(found);

  return (trx_undo_get_next_rec_from_next_page(
      space, page_size, page_align(rec), page_no, offset, RW_S_LATCH, mtr));
}

/** Gets the first record in an undo log.
@param[out]	modifier_trx_id	the modifier trx identifier.
@param[in]	space		undo log header space
@param[in]	page_size	page size
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset on page
@param[in]	mode		latching mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]	mtr		mini-transaction
@return undo log record, the page latched, NULL if none */
trx_undo_rec_t *trx_undo_get_first_rec(trx_id_t *modifier_trx_id,
                                       space_id_t space,
                                       const page_size_t &page_size,
                                       page_no_t page_no, ulint offset,
                                       ulint mode, mtr_t *mtr) {
  page_t *undo_page;
  trx_undo_rec_t *rec;

  const page_id_t page_id(space, page_no);

  if (mode == RW_S_LATCH) {
    undo_page = trx_undo_page_get_s_latched(page_id, page_size, mtr);
  } else {
    undo_page = trx_undo_page_get(page_id, page_size, mtr);
  }

  if (modifier_trx_id != nullptr) {
    trx_ulogf_t *undo_header = undo_page + offset;
    *modifier_trx_id = mach_read_from_8(undo_header + TRX_UNDO_TRX_ID);
  }

  rec = trx_undo_page_get_first_rec(undo_page, page_no, offset);

  if (rec) {
    return (rec);
  }

  return (trx_undo_get_next_rec_from_next_page(space, page_size, undo_page,
                                               page_no, offset, mode, mtr));
}

/*============== UNDO LOG FILE COPY CREATION AND FREEING ==================*/

/** Writes the mtr log entry of an undo log page initialization. */
UNIV_INLINE
void trx_undo_page_init_log(page_t *undo_page, /*!< in: undo log page */
                            ulint type,        /*!< in: undo log type */
                            mtr_t *mtr)        /*!< in: mtr */
{
  mlog_write_initial_log_record(undo_page, MLOG_UNDO_INIT, mtr);

  mlog_catenate_ulint_compressed(mtr, type);
}
#else /* !UNIV_HOTBACKUP */
#define trx_undo_page_init_log(undo_page, type, mtr) ((void)0)
#endif /* !UNIV_HOTBACKUP */

/** Parses the redo log entry of an undo log page initialization.
 @return end of log record or NULL */
byte *trx_undo_parse_page_init(const byte *ptr,     /*!< in: buffer */
                               const byte *end_ptr, /*!< in: buffer end */
                               page_t *page,        /*!< in: page or NULL */
                               mtr_t *mtr)          /*!< in: mtr or NULL */
{
  ulint type;

  type = mach_parse_compressed(&ptr, end_ptr);

  if (ptr == NULL) {
    return (NULL);
  }

  if (page) {
    trx_undo_page_init(page, type, mtr);
  }

  return (const_cast<byte *>(ptr));
}

/** Initializes the fields in an undo log segment page. */
static void trx_undo_page_init(
    page_t *undo_page, /*!< in: undo log segment page */
    ulint type,        /*!< in: undo log segment type */
    mtr_t *mtr)        /*!< in: mtr */
{
  trx_upagef_t *page_hdr;

  page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_TYPE, type);

  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START,
                  TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE,
                  TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);

  fil_page_set_type(undo_page, FIL_PAGE_UNDO_LOG);

  trx_undo_page_init_log(undo_page, type, mtr);
}

#ifndef UNIV_HOTBACKUP
/** Creates a new undo log segment in file.
 @return DB_SUCCESS if page creation OK possible error codes are:
 DB_TOO_MANY_CONCURRENT_TRXS DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((warn_unused_result)) dberr_t trx_undo_seg_create(
    trx_rseg_t *rseg MY_ATTRIBUTE((unused)), /*!< in: rollback segment */
    trx_rsegf_t *rseg_hdr, /*!< in: rollback segment header, page
                          x-latched */
    ulint type,            /*!< in: type of the segment: TRX_UNDO_INSERT or
                           TRX_UNDO_UPDATE */
    ulint *id,             /*!< out: slot index within rseg header */
    page_t **undo_page,
    /*!< out: segment header page x-latched, NULL
    if there was an error */
    mtr_t *mtr) /*!< in: mtr */
{
  ulint slot_no = ULINT_UNDEFINED;
  space_id_t space;
  buf_block_t *block;
  trx_upagef_t *page_hdr;
  trx_usegf_t *seg_hdr;
  ulint n_reserved;
  bool success;
  dberr_t err = DB_SUCCESS;

  ut_ad(mtr != NULL);
  ut_ad(id != NULL);
  ut_ad(rseg_hdr != NULL);
  ut_ad(mutex_own(&(rseg->mutex)));

#ifdef UNIV_DEBUG
  if (!srv_inject_too_many_concurrent_trxs)
#endif
  {
    slot_no = trx_rsegf_undo_find_free(rseg_hdr, mtr);
  }
  if (slot_no == ULINT_UNDEFINED) {
    ib::error(ER_IB_MSG_1212)
        << "Cannot find a free slot for an undo log."
           " You may have too many active transactions running concurrently."
           " Please add more rollback segments or undo tablespaces.";

    return (DB_TOO_MANY_CONCURRENT_TRXS);
  }

  space = page_get_space_id(page_align(rseg_hdr));

  success = fsp_reserve_free_extents(&n_reserved, space, 2, FSP_UNDO, mtr);
  if (!success) {
    return (DB_OUT_OF_FILE_SPACE);
  }

  /* Allocate a new file segment for the undo log */
  block = fseg_create_general(space, 0, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
                              TRUE, mtr);

  fil_space_release_free_extents(space, n_reserved);

  if (block == NULL) {
    /* No space left */

    return (DB_OUT_OF_FILE_SPACE);
  }

  buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

  *undo_page = buf_block_get_frame(block);

  page_hdr = *undo_page + TRX_UNDO_PAGE_HDR;
  seg_hdr = *undo_page + TRX_UNDO_SEG_HDR;

  trx_undo_page_init(*undo_page, type, mtr);

  mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE,
                   TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE, MLOG_2BYTES, mtr);

  mlog_write_ulint(seg_hdr + TRX_UNDO_LAST_LOG, 0, MLOG_2BYTES, mtr);

  flst_init(seg_hdr + TRX_UNDO_PAGE_LIST, mtr);

  flst_add_last(seg_hdr + TRX_UNDO_PAGE_LIST, page_hdr + TRX_UNDO_PAGE_NODE,
                mtr);

  trx_rsegf_set_nth_undo(rseg_hdr, slot_no, page_get_page_no(*undo_page), mtr);
  *id = slot_no;

  MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);

  return (err);
}

/** Writes the mtr log entry of an undo log header initialization. */
UNIV_INLINE
void trx_undo_header_create_log(
    const page_t *undo_page, /*!< in: undo log header page */
    trx_id_t trx_id,         /*!< in: transaction id */
    mtr_t *mtr)              /*!< in: mtr */
{
  mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_CREATE, mtr);

  mlog_catenate_ull_compressed(mtr, trx_id);
}
#else /* !UNIV_HOTBACKUP */
#define trx_undo_header_create_log(undo_page, trx_id, mtr) ((void)0)
#endif /* !UNIV_HOTBACKUP */

/** Creates a new undo log header in file. NOTE that this function has its own
 log record type MLOG_UNDO_HDR_CREATE. You must NOT change the operation of
 this function!
 @return header byte offset on page */
static ulint trx_undo_header_create(
    page_t *undo_page, /*!< in/out: undo log segment
                       header page, x-latched; it is
                       assumed that there is
                       TRX_UNDO_LOG_HDR_SIZE bytes
                       free space on it */
    trx_id_t trx_id,   /*!< in: transaction id */
    mtr_t *mtr)        /*!< in: mtr */
{
  trx_upagef_t *page_hdr;
  trx_usegf_t *seg_hdr;
  trx_ulogf_t *log_hdr;
  ulint prev_log;
  ulint free;
  ulint new_free;

  ut_ad(mtr && undo_page);

  page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
  seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

  free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);

  log_hdr = undo_page + free;

  new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

  ut_a(free + TRX_UNDO_LOG_HDR_SIZE < UNIV_PAGE_SIZE - 100);

  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);

  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);

  mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

  prev_log = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);

  if (prev_log != 0) {
    trx_ulogf_t *prev_log_hdr;

    prev_log_hdr = undo_page + prev_log;

    mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, free);
  }

  mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, free);

  log_hdr = undo_page + free;

  mach_write_to_2(log_hdr + TRX_UNDO_DEL_MARKS, TRUE);

  mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
  mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

  mach_write_to_1(log_hdr + TRX_UNDO_FLAGS, 0);
  mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

  mach_write_to_2(log_hdr + TRX_UNDO_NEXT_LOG, 0);
  mach_write_to_2(log_hdr + TRX_UNDO_PREV_LOG, prev_log);

  /* Write the log record about the header creation */
  trx_undo_header_create_log(undo_page, trx_id, mtr);

  return (free);
}

#ifndef UNIV_HOTBACKUP
/** Write X/Open XA Transaction Identification (XID) to undo log header */
static void trx_undo_write_xid(
    trx_ulogf_t *log_hdr, /*!< in: undo log header */
    const XID *xid,       /*!< in: X/Open XA Transaction Identification */
    mtr_t *mtr)           /*!< in: mtr */
{
  mlog_write_ulint(log_hdr + TRX_UNDO_XA_FORMAT,
                   static_cast<ulint>(xid->get_format_id()), MLOG_4BYTES, mtr);

  mlog_write_ulint(log_hdr + TRX_UNDO_XA_TRID_LEN,
                   static_cast<ulint>(xid->get_gtrid_length()), MLOG_4BYTES,
                   mtr);

  mlog_write_ulint(log_hdr + TRX_UNDO_XA_BQUAL_LEN,
                   static_cast<ulint>(xid->get_bqual_length()), MLOG_4BYTES,
                   mtr);

  mlog_write_string(log_hdr + TRX_UNDO_XA_XID,
                    reinterpret_cast<const byte *>(xid->get_data()),
                    XIDDATASIZE, mtr);
}

/*
 * dzw: The original official impl could cause a serious performance issue
 * for XA txns, because XA COMMIT/XA ROLLBACK stmts would have to wait for
 * background gtid-persistor thread to flush the gtid to mysql.gtid_executed
 * table to spare its slot on undo log.
 *
 * Now we always alloc 2 gtid slots in update_undo segment of each txn, the 1st
 * slot is always stored with gtid, the 2nd slot is only used if the txn is a
 * XA txn branch that does 2PC, thus no need to wait for the persistor to
 * complete persisting the gtid.
 * */
void trx_undo_gtid_flush_prepare(trx_t *trx) {
#if 0
  /* Only relevant for prepared transaction. */
  if (!trx_state_eq(trx, TRX_STATE_PREPARED)) {
    return;
  }
  /* Only external transactions have GTID for XA PREPARE. */
  if (trx_is_mysql_xa(trx)) {
    return;
  }
  /* Wait for XA Prepare GTID to flush. */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  gtid_persistor.wait_flush(true, false, false, nullptr);
#endif
}

dberr_t trx_undo_gtid_add_update_undo(trx_t *trx, bool prepare, bool rollback) {
  ut_ad(!(prepare && rollback));
  /* Check if GTID persistence is needed. */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  bool alloc = gtid_persistor.trx_check_set(trx, prepare, rollback);

  if (!alloc) {
    return (DB_SUCCESS);
  }

  /* For GTID persistence we need update undo segment. */
  auto undo_ptr = &trx->rsegs.m_redo;
  dberr_t db_err = DB_SUCCESS;
  if (!undo_ptr->update_undo) {
    ut_ad(!rollback);
    mutex_enter(&trx->undo_mutex);
    db_err = trx_undo_assign_undo(trx, undo_ptr, TRX_UNDO_UPDATE);
    mutex_exit(&trx->undo_mutex);
  }
  /* In rare cases we might find no available update undo segment for insert
  only transactions. It is still fine to return error at prepare stage.
  Cannot do it earlier as GTID information is not known before. Keep the
  debug assert to know if it really happens ever. */
  if (db_err != DB_SUCCESS) {
    ut_ad(false);
    trx->persists_gtid = false;
    ib::error(ER_IB_CLONE_GTID_PERSIST)
        << "Could not allocate undo segment"
        << " slot for persisting GTID. DB Error: " << db_err;
  }
  return (db_err);
}

void trx_undo_gtid_set(trx_t *trx, trx_undo_t *undo) {
  /* Reset GTID flag */
  undo->flag &= ~TRX_UNDO_FLAG_GTID;

  if (!trx->persists_gtid) {
    return;
  }

  /* Verify that we have allocated for GTID */
  if (!undo->gtid_allocated) {
    ut_ad(false);
    ib::error(ER_IB_CLONE_GTID_PERSIST)
        << "Could not persist GTID as space for GTID is not allocated.";
    return;
  }
  undo->flag |= TRX_UNDO_FLAG_GTID;
}

extern ulint srv_num_aborted_txns_no_gtid;
extern bool opt_bin_log;
void trx_undo_gtid_read_and_persist(trx_ulogf_t *undo_header, trx_undo_t *undo) {
  /* Check if undo log has GTID. */
  auto flag = mach_read_ulint(undo_header + TRX_UNDO_FLAGS, MLOG_1BYTE);
  if ((flag & TRX_UNDO_FLAG_GTID) == 0) {
    return;
  }
  /* Extract and add GTID information of the transaction to the persister. */
  Gtid_desc gtid_desc, gtid_desc2;

  /* Get GTID format version. */
  gtid_desc2.m_version = gtid_desc.m_version = static_cast<uint32_t>(
      mach_read_from_1(undo_header + TRX_UNDO_LOG_GTID_VERSION));

  auto pgtid1 = undo_header + TRX_UNDO_LOG_GTID;
  auto pgtid2 = undo_header + TRX_UNDO_LOG_GTID2;

  /*
   * dzw:
   * For XA txns(either 1PC(COP) or 2PC), both of its gtid slots were inited
   * with 0s, and the 1st slot is filled with valid gtid *after* the txn is
   * marked prepared, so it's possible that a crash could leave some PREPARED
   * XA txns with no valid gtids, and they must be aborted.
   *
   * For ordinary txns, the 2nd slot is always 0 but the 1st one is never 0,
   * it's filled with valid gtid during innodb commit.
   *
   * if binlogging is not enabled, then gtid won't ever be generated or written
   * to innodb update undo log, thus we should not abort txns in this case. we
   * could have also checked 'log_slave_updates' but a slave can also execute
   * txns and write binlogs if it's opt_bin_log is on regardless of its
   * log_slave_updates. so if log_slave_updates is off and opt_bin_log is on,
   * a slave could abort prepared txns which have no gtid after a restart.
   *
   * we assume log_bin is static after a db instance is created, this is the
   * usually how mysql is used. otherwise we could abort prepared XA txns which
   * correctly prepared before mysqld restarted with log_bin enabled.
   * */
  if (*pgtid1 == '\0' && opt_bin_log)
  {
    if (undo && undo->state == TRX_UNDO_PREPARED) {
      undo->state = TRX_UNDO_ACTIVE;
      srv_num_aborted_txns_no_gtid++;
#ifdef UNIV_DEBUG
      char xidsbuf[XID::ser_buf_size];
      sql_print_information("Innodb recovery: XA transaction %s found prepared but has no gtid, it's marked for abort.",
          undo->xid.serialize(xidsbuf));
#endif
    }

    return;
  }

  /* Get GTID information string. */
  memcpy(&gtid_desc.m_info[0], pgtid1, TRX_UNDO_LOG_GTID_LEN);
  /* Mark GTID valid. */
  gtid_desc.m_is_set = true;

  /* GTID2 may not have been written yet for a PREPARED XA txn and it will
   * never exist for a one phase XA txn.
   */
  if (*pgtid2 != '\0') {
    memcpy(&gtid_desc2.m_info[0], pgtid2, TRX_UNDO_LOG_GTID_LEN);
    gtid_desc2.m_is_set = true;
  }

  /* Get GTID persister */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();

  /* No concurrency is involved during recovery but satisfy
  the interface requirement. */
  trx_sys_mutex_enter();
  gtid_persistor.add(gtid_desc);
  if (*pgtid2 != '\0')
    gtid_persistor.add(gtid_desc2);
  trx_sys_mutex_exit();
}


/*
 * Initialize gtid slots for external XA txns, init them with '\0' in order to
 * distinguish filled slots from those unfilled.
 * */
static void
init_xa_gtid_slots(trx_t *trx, trx_ulogf_t *undo_header, trx_undo_t *undo,
                   mtr_t *mtr) {

  ut_ad(undo->state == TRX_UNDO_PREPARED);
  unsigned char *pgtid1 = undo_header + TRX_UNDO_LOG_GTID;
  unsigned char *pgtid2 = undo_header + TRX_UNDO_LOG_GTID2;
  byte zeros[1] = {0};
  if (!(undo->flag & TRX_UNDO_FLAG_GTID) || trx_is_mysql_xa(trx))
    return;

  /*
   * The 1st byte of each gtid slot is tested to see if gtid stored into
   * the slot.
   * */
  if (*pgtid1 != '\0')
    mlog_write_string(pgtid1, zeros, 1, mtr);
  if (*pgtid2 != '\0')
    mlog_write_string(pgtid2, zeros, 1, mtr);
}

void trx_undo_gtid_write(trx_t *trx, trx_ulogf_t *undo_header, trx_undo_t *undo,
                         mtr_t *mtr) {
  if ((undo->flag & TRX_UNDO_FLAG_GTID) == 0) {
    return;
  }

  /* Reset GTID flag */
  undo->flag &= ~TRX_UNDO_FLAG_GTID;

  /* We must have allocated for GTID but add a safe check. */
  if (!undo->gtid_allocated) {
    ut_ad(false);
    return;
  }

  Gtid_desc gtid_desc;
  auto &gtid_persistor = clone_sys->get_gtid_persistor();

  gtid_persistor.get_gtid_info(trx, gtid_desc);

  if (gtid_desc.m_is_set) {
    /* Persist GTID version */
    mlog_write_ulint(undo_header + TRX_UNDO_LOG_GTID_VERSION,
                     gtid_desc.m_version, MLOG_1BYTE, mtr);
    /* Persist fixed length GTID
     * dzw:
     * for an TRX_UNDO_ACTIVE undo, store gtid to 1st gtid slot,
     * for a TRX_UNDO_PREPARED undo, store gtid to 2nd gtid slot.
     * */
    ut_ad((trx->state == TRX_STATE_ACTIVE && !trx_is_mysql_xa(trx)) ||
          trx->state == TRX_STATE_PREPARED);
    auto pgtid1 = undo_header + TRX_UNDO_LOG_GTID;
    const bool prepared_external_trx = 
      (trx->state == TRX_STATE_PREPARED && !trx_is_mysql_xa(trx) && *pgtid1 != '\0');
    /* Persist GTID version, don't write again for external XA txn. */
    if (!prepared_external_trx)
      mlog_write_ulint(undo_header + TRX_UNDO_LOG_GTID_VERSION,
                       gtid_desc.m_version, MLOG_1BYTE, mtr);
    else {
      ut_ad(*pgtid1 != '\0');
    }

    ulint gtid_offset =
      (prepared_external_trx ? TRX_UNDO_LOG_GTID2 : TRX_UNDO_LOG_GTID);
    ut_ad(TRX_UNDO_LOG_GTID_LEN == GTID_INFO_SIZE);
    mlog_write_string(undo_header + gtid_offset, &gtid_desc.m_info[0],
                      TRX_UNDO_LOG_GTID_LEN, mtr);
    undo->flag |= TRX_UNDO_FLAG_GTID;
  }
  mlog_write_ulint(undo_header + TRX_UNDO_FLAGS, undo->flag, MLOG_1BYTE, mtr);
}

/** Read X/Open XA Transaction Identification (XID) from undo log header */
static void trx_undo_read_xid(
    trx_ulogf_t *log_hdr, /*!< in: undo log header */
    XID *xid)             /*!< out: X/Open XA Transaction Identification */
{
  xid->set_format_id(
      static_cast<long>(mach_read_from_4(log_hdr + TRX_UNDO_XA_FORMAT)));

  xid->set_gtrid_length(
      static_cast<long>(mach_read_from_4(log_hdr + TRX_UNDO_XA_TRID_LEN)));

  xid->set_bqual_length(
      static_cast<long>(mach_read_from_4(log_hdr + TRX_UNDO_XA_BQUAL_LEN)));

  xid->set_data(log_hdr + TRX_UNDO_XA_XID, XIDDATASIZE);
}

/** Adds space for the XA XID after an undo log old-style header.
@param[in,out]	undo_page	undo log segment header page
@param[in,out]	log_hdr		undo log header
@param[in,out]	mtr		mini transaction
@param[in]	add_gtid	add space for GTID */
static void trx_undo_header_add_space_for_xid(page_t *undo_page,
                                              trx_ulogf_t *log_hdr, mtr_t *mtr,
                                              bool add_gtid) {
  trx_upagef_t *page_hdr;
  ulint free;
  ulint new_free;

  page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

  free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);

  /* free is now the end offset of the old style undo log header */
  ut_a(free == (ulint)(log_hdr - undo_page) + TRX_UNDO_LOG_OLD_HDR_SIZE);

  ulint new_limit = add_gtid ? TRX_UNDO_LOG_HDR_SIZE : TRX_UNDO_LOG_XA_HDR_SIZE;

  new_free = free + (new_limit - TRX_UNDO_LOG_OLD_HDR_SIZE);

  /* Add space for a XID after the header, update the free offset
  fields on the undo log page and in the undo log header */

  mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_START, new_free, MLOG_2BYTES, mtr);

  mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE, new_free, MLOG_2BYTES, mtr);

  mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, new_free, MLOG_2BYTES, mtr);
}

/** Writes the mtr log entry of an undo log header reuse. */
UNIV_INLINE
void trx_undo_insert_header_reuse_log(
    const page_t *undo_page, /*!< in: undo log header page */
    trx_id_t trx_id,         /*!< in: transaction id */
    mtr_t *mtr)              /*!< in: mtr */
{
  mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_REUSE, mtr);

  mlog_catenate_ull_compressed(mtr, trx_id);
}
#else /* !UNIV_HOTBACKUP */
#define trx_undo_insert_header_reuse_log(undo_page, trx_id, mtr) ((void)0)
#endif /* !UNIV_HOTBACKUP */

/** Parse the redo log entry of an undo log page header create or reuse.
@param[in]	type	MLOG_UNDO_HDR_CREATE or MLOG_UNDO_HDR_REUSE
@param[in]	ptr	redo log record
@param[in]	end_ptr	end of log buffer
@param[in,out]	page	page frame or NULL
@param[in,out]	mtr	mini-transaction or NULL
@return end of log record or NULL */
byte *trx_undo_parse_page_header(mlog_id_t type, const byte *ptr,
                                 const byte *end_ptr, page_t *page,
                                 mtr_t *mtr) {
  trx_id_t trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

  if (ptr != NULL && page != NULL) {
    switch (type) {
      case MLOG_UNDO_HDR_CREATE:
        trx_undo_header_create(page, trx_id, mtr);
        return (const_cast<byte *>(ptr));
      case MLOG_UNDO_HDR_REUSE:
        trx_undo_insert_header_reuse(page, trx_id, mtr);
        return (const_cast<byte *>(ptr));
      default:
        break;
    }
    ut_ad(0);
  }

  return (const_cast<byte *>(ptr));
}

/** Initializes a cached insert undo log header page for new use. NOTE that this
 function has its own log record type MLOG_UNDO_HDR_REUSE. You must NOT change
 the operation of this function!
 @return undo log header byte offset on page */
static ulint trx_undo_insert_header_reuse(
    page_t *undo_page, /*!< in/out: insert undo log segment
                       header page, x-latched */
    trx_id_t trx_id,   /*!< in: transaction id */
    mtr_t *mtr)        /*!< in: mtr */
{
  trx_upagef_t *page_hdr;
  trx_usegf_t *seg_hdr;
  trx_ulogf_t *log_hdr;
  ulint free;
  ulint new_free;

  ut_ad(mtr && undo_page);

  page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
  seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

  free = TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE;

  ut_a(free + TRX_UNDO_LOG_HDR_SIZE < UNIV_PAGE_SIZE - 100);

  log_hdr = undo_page + free;

  new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

  /* Insert undo data is not needed after commit: we may free all
  the space on the page */

  ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) ==
       TRX_UNDO_INSERT);

  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);

  mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);

  mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

  log_hdr = undo_page + free;

  mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
  mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

  mach_write_to_1(log_hdr + TRX_UNDO_FLAGS, 0);
  mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

  /* Write the log record MLOG_UNDO_HDR_REUSE */
  trx_undo_insert_header_reuse_log(undo_page, trx_id, mtr);

  return (free);
}

#ifndef UNIV_HOTBACKUP
/** Tries to add a page to the undo log segment where the undo log is placed.
 @return X-latched block if success, else NULL */
buf_block_t *trx_undo_add_page(
    trx_t *trx,               /*!< in: transaction */
    trx_undo_t *undo,         /*!< in: undo log memory object */
    trx_undo_ptr_t *undo_ptr, /*!< in: assign undo log from
                              referred rollback segment. */
    mtr_t *mtr)               /*!< in: mtr which does not have
                              a latch to any undo log page;
                              the caller must have reserved
                              the rollback segment mutex */
{
  page_t *header_page;
  buf_block_t *new_block;
  page_t *new_page;
  trx_rseg_t *rseg;
  ulint n_reserved;

  ut_ad(mutex_own(&(trx->undo_mutex)));
  ut_ad(mutex_own(&(undo_ptr->rseg->mutex)));

  rseg = undo_ptr->rseg;

  if (rseg->curr_size == rseg->max_size) {
    return (NULL);
  }

  header_page = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                                  undo->page_size, mtr);

  if (!fsp_reserve_free_extents(&n_reserved, undo->space, 1, FSP_UNDO, mtr)) {
    return (NULL);
  }

  new_block = fseg_alloc_free_page_general(
      TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER + header_page,
      undo->top_page_no + 1, FSP_UP, TRUE, mtr, mtr);

  fil_space_release_free_extents(undo->space, n_reserved);

  if (new_block == NULL) {
    /* No space left */

    return (NULL);
  }

  ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
  buf_block_dbg_add_level(new_block, SYNC_TRX_UNDO_PAGE);
  undo->last_page_no = new_block->page.id.page_no();

  new_page = buf_block_get_frame(new_block);

  trx_undo_page_init(new_page, undo->type, mtr);

  flst_add_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
                new_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);
  undo->size++;
  rseg->curr_size++;

  return (new_block);
}

/** Frees an undo log page that is not the header page.
 @return last page number in remaining log */
static page_no_t trx_undo_free_page(
    trx_rseg_t *rseg,      /*!< in: rollback segment */
    ibool in_history,      /*!< in: TRUE if the undo log is in the history
                           list */
    space_id_t space,      /*!< in: space */
    page_no_t hdr_page_no, /*!< in: header page number */
    page_no_t page_no,     /*!< in: page number to free: must not be the
                           header page */
    mtr_t *mtr)            /*!< in: mtr which does not have a latch to any
                           undo log page; the caller must have reserved
                           the rollback segment mutex */
{
  page_t *header_page;
  page_t *undo_page;
  fil_addr_t last_addr;
  trx_rsegf_t *rseg_header;
  ulint hist_size;

  ut_a(hdr_page_no != page_no);
  ut_ad(mutex_own(&(rseg->mutex)));

  undo_page =
      trx_undo_page_get(page_id_t(space, page_no), rseg->page_size, mtr);

  header_page =
      trx_undo_page_get(page_id_t(space, hdr_page_no), rseg->page_size, mtr);

  flst_remove(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
              undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

  fseg_free_page(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, space,
                 page_no, false, mtr);

  last_addr =
      flst_get_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST, mtr);
  rseg->curr_size--;

  if (in_history) {
    rseg_header = trx_rsegf_get(space, rseg->page_no, rseg->page_size, mtr);

    hist_size =
        mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr);
    ut_ad(hist_size > 0);
    mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, hist_size - 1,
                     MLOG_4BYTES, mtr);
  }

  return (last_addr.page);
}

/** Frees the last undo log page.
 The caller must hold the rollback segment mutex. */
void trx_undo_free_last_page_func(
#ifdef UNIV_DEBUG
    const trx_t *trx, /*!< in: transaction */
#endif                /* UNIV_DEBUG */
    trx_undo_t *undo, /*!< in/out: undo log memory copy */
    mtr_t *mtr)       /*!< in/out: mini-transaction which does not
                      have a latch to any undo log page or which
                      has allocated the undo log page */
{
  ut_ad(mutex_own(&trx->undo_mutex));
  ut_ad(undo->hdr_page_no != undo->last_page_no);
  ut_ad(undo->size > 0);

  undo->last_page_no =
      trx_undo_free_page(undo->rseg, FALSE, undo->space, undo->hdr_page_no,
                         undo->last_page_no, mtr);

  undo->size--;
}

/** Empties an undo log header page of undo records for that undo log.
Other undo logs may still have records on that page, if it is an update
undo log.
@param[in]	space		space
@param[in]	page_size	page size
@param[in]	hdr_page_no	header page number
@param[in]	hdr_offset	header offset
@param[in,out]	mtr		mini-transaction */
static void trx_undo_empty_header_page(space_id_t space,
                                       const page_size_t &page_size,
                                       page_no_t hdr_page_no, ulint hdr_offset,
                                       mtr_t *mtr) {
  page_t *header_page;
  trx_ulogf_t *log_hdr;
  ulint end;

  header_page =
      trx_undo_page_get(page_id_t(space, hdr_page_no), page_size, mtr);

  log_hdr = header_page + hdr_offset;

  end = trx_undo_page_get_end(header_page, hdr_page_no, hdr_offset);

  mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, end, MLOG_2BYTES, mtr);
}

/** Truncates an undo log from the end. This function is used during a rollback
 to free space from an undo log. */
void trx_undo_truncate_end_func(
#ifdef UNIV_DEBUG
    const trx_t *trx, /*!< in: transaction whose undo log it is */
#endif                /* UNIV_DEBUG */
    trx_undo_t *undo, /*!< in: undo log */
    undo_no_t limit)  /*!< in: all undo records with undo number
                      >= this value should be truncated */
{
  page_t *undo_page;
  page_no_t last_page_no;
  trx_undo_rec_t *rec;
  trx_undo_rec_t *trunc_here;
  mtr_t mtr;

  ut_ad(mutex_own(&(trx->undo_mutex)));

  ut_ad(mutex_own(&undo->rseg->mutex));

  for (;;) {
    mtr_start(&mtr);
    if (fsp_is_system_temporary(undo->rseg->space_id)) {
      mtr.set_log_mode(MTR_LOG_NO_REDO);
      ut_ad(trx->rsegs.m_noredo.rseg == undo->rseg);
    } else {
      ut_ad(trx->rsegs.m_redo.rseg == undo->rseg);
    }

    trunc_here = NULL;

    last_page_no = undo->last_page_no;

    undo_page = trx_undo_page_get(page_id_t(undo->space, last_page_no),
                                  undo->page_size, &mtr);

    rec = trx_undo_page_get_last_rec(undo_page, undo->hdr_page_no,
                                     undo->hdr_offset);
    while (rec) {
      if (trx_undo_rec_get_undo_no(rec) >= limit) {
        /* Truncate at least this record off, maybe
        more */
        trunc_here = rec;
      } else {
        goto function_exit;
      }

      rec =
          trx_undo_page_get_prev_rec(rec, undo->hdr_page_no, undo->hdr_offset);
    }

    if (last_page_no == undo->hdr_page_no) {
      goto function_exit;
    }

    ut_ad(last_page_no == undo->last_page_no);
    trx_undo_free_last_page(trx, undo, &mtr);

    mtr_commit(&mtr);
  }

function_exit:
  if (trunc_here) {
    mlog_write_ulint(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE,
                     trunc_here - undo_page, MLOG_2BYTES, &mtr);
  }

  mtr_commit(&mtr);
}

/** Truncate the head of an undo log.
NOTE that only whole pages are freed; the header page is not
freed, but emptied, if all the records there are below the limit.
@param[in,out]	rseg		rollback segment
@param[in]	hdr_page_no	header page number
@param[in]	hdr_offset	header offset on the page
@param[in]	limit		first undo number to preserve
(everything below the limit will be truncated) */
void trx_undo_truncate_start(trx_rseg_t *rseg, page_no_t hdr_page_no,
                             ulint hdr_offset, undo_no_t limit) {
  page_t *undo_page;
  trx_undo_rec_t *rec;
  trx_undo_rec_t *last_rec;
  page_no_t page_no;
  mtr_t mtr;

  ut_ad(mutex_own(&(rseg->mutex)));

  if (!limit) {
    return;
  }
loop:
  mtr_start(&mtr);

  if (fsp_is_system_temporary(rseg->space_id)) {
    mtr.set_log_mode(MTR_LOG_NO_REDO);
  }

  rec = trx_undo_get_first_rec(nullptr, rseg->space_id, rseg->page_size,
                               hdr_page_no, hdr_offset, RW_X_LATCH, &mtr);
  if (rec == NULL) {
    /* Already empty */

    mtr_commit(&mtr);

    return;
  }

  undo_page = page_align(rec);

  last_rec = trx_undo_page_get_last_rec(undo_page, hdr_page_no, hdr_offset);
  if (trx_undo_rec_get_undo_no(last_rec) >= limit) {
    mtr_commit(&mtr);

    return;
  }

  page_no = page_get_page_no(undo_page);

  if (page_no == hdr_page_no) {
    trx_undo_empty_header_page(rseg->space_id, rseg->page_size, hdr_page_no,
                               hdr_offset, &mtr);
  } else {
    trx_undo_free_page(rseg, TRUE, rseg->space_id, hdr_page_no, page_no, &mtr);
  }

  mtr_commit(&mtr);

  goto loop;
}

/** Frees an undo log segment which is not in the history list.
@param[in]	undo	undo log
@param[in]	noredo	whether the undo tablespace is redo logged */
static void trx_undo_seg_free(const trx_undo_t *undo, bool noredo) {
  trx_rseg_t *rseg;
  fseg_header_t *file_seg;
  trx_rsegf_t *rseg_header;
  trx_usegf_t *seg_header;
  ibool finished;
  mtr_t mtr;

  rseg = undo->rseg;

  do {
    mtr_start(&mtr);

    if (noredo) {
      mtr.set_log_mode(MTR_LOG_NO_REDO);
    }

    mutex_enter(&(rseg->mutex));

    seg_header = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                                   undo->page_size, &mtr) +
                 TRX_UNDO_SEG_HDR;

    file_seg = seg_header + TRX_UNDO_FSEG_HEADER;

    finished = fseg_free_step(file_seg, false, &mtr);

    if (finished) {
      /* Update the rseg header */
      rseg_header =
          trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);
      trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, &mtr);

      MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);
    }

    mutex_exit(&(rseg->mutex));
    mtr_commit(&mtr);
  } while (!finished);
}

/*========== UNDO LOG MEMORY COPY INITIALIZATION =====================*/

/** Creates and initializes an undo log memory object for a newly created
rseg. The memory object is inserted in the appropriate list in the rseg.
 @return own: the undo log memory object */
static trx_undo_t *trx_undo_mem_init(
    trx_rseg_t *rseg,  /*!< in: rollback segment memory object */
    ulint id,          /*!< in: slot index within rseg */
    page_no_t page_no, /*!< in: undo log segment page number */
    mtr_t *mtr)        /*!< in: mtr */
{
  page_t *undo_page;
  trx_upagef_t *page_header;
  trx_usegf_t *seg_header;
  trx_ulogf_t *undo_header;
  trx_undo_t *undo;
  ulint type;
  ulint state;
  trx_id_t trx_id;
  ulint offset;
  fil_addr_t last_addr;
  page_t *last_page;
  trx_undo_rec_t *rec;
  XID xid;

  ut_a(id < TRX_RSEG_N_SLOTS);

  undo_page = trx_undo_page_get(page_id_t(rseg->space_id, page_no),
                                rseg->page_size, mtr);

  page_header = undo_page + TRX_UNDO_PAGE_HDR;

  type = mtr_read_ulint(page_header + TRX_UNDO_PAGE_TYPE, MLOG_2BYTES, mtr);
  seg_header = undo_page + TRX_UNDO_SEG_HDR;

  state = mach_read_from_2(seg_header + TRX_UNDO_STATE);

  offset = mach_read_from_2(seg_header + TRX_UNDO_LAST_LOG);

  undo_header = undo_page + offset;

  trx_id = mach_read_from_8(undo_header + TRX_UNDO_TRX_ID);

  auto flag = mtr_read_ulint(undo_header + TRX_UNDO_FLAGS, MLOG_1BYTE, mtr);

  bool xid_exists = ((flag & TRX_UNDO_FLAG_XID) != 0);

  bool gtid_exists = ((flag & TRX_UNDO_FLAG_GTID) != 0);

  /* Read X/Open XA transaction identification if it exists, or
  set it to NULL. */
  xid.reset();

  if (xid_exists) {
    trx_undo_read_xid(undo_header, &xid);
  }

  mutex_enter(&(rseg->mutex));
  undo = trx_undo_mem_create(rseg, id, type, trx_id, &xid, page_no, offset);
  mutex_exit(&(rseg->mutex));

  undo->dict_operation =
      mtr_read_ulint(undo_header + TRX_UNDO_DICT_TRANS, MLOG_1BYTE, mtr);

  undo->flag = flag;
  undo->gtid_allocated = gtid_exists;

  undo->state = state;
  undo->size = flst_get_len(seg_header + TRX_UNDO_PAGE_LIST);

  /* If the log segment is being freed, the page list is inconsistent! */
  if (state == TRX_UNDO_TO_FREE) {
    goto add_to_list;
  }

  last_addr = flst_get_last(seg_header + TRX_UNDO_PAGE_LIST, mtr);

  undo->last_page_no = last_addr.page;
  undo->top_page_no = last_addr.page;

  last_page = trx_undo_page_get(page_id_t(rseg->space_id, undo->last_page_no),
                                rseg->page_size, mtr);

  rec = trx_undo_page_get_last_rec(last_page, page_no, offset);

  if (rec == NULL) {
    undo->empty = TRUE;
  } else {
    undo->empty = FALSE;
    undo->top_offset = rec - last_page;
    undo->top_undo_no = trx_undo_rec_get_undo_no(rec);
  }
add_to_list:
  if (type == TRX_UNDO_INSERT) {
    if (state != TRX_UNDO_CACHED) {
      UT_LIST_ADD_LAST(rseg->insert_undo_list, undo);
    } else {
      UT_LIST_ADD_LAST(rseg->insert_undo_cached, undo);

      MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
    }
  } else {
    ut_ad(type == TRX_UNDO_UPDATE);
    if (state != TRX_UNDO_CACHED) {
      UT_LIST_ADD_LAST(rseg->update_undo_list, undo);
      /* For XA prepared transaction and XA rolled back transaction, we
      could have GTID to be persisted. */
      if (state == TRX_UNDO_PREPARED || state == TRX_UNDO_ACTIVE) {
        trx_undo_gtid_read_and_persist(undo_header, undo);
      }
    } else {
      UT_LIST_ADD_LAST(rseg->update_undo_cached, undo);

      MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
    }
  }

  return (undo);
}

/** Initializes the undo log lists for a rollback segment memory copy. This
 function is only called when the database is started or a new rollback
 segment is created.
 @return the combined size of undo log segments in pages */
ulint trx_undo_lists_init(
    trx_rseg_t *rseg) /*!< in: rollback segment memory object */
{
  ulint size = 0;
  trx_rsegf_t *rseg_header;
  ulint i;
  mtr_t mtr;

  mtr_start(&mtr);

  rseg_header =
      trx_rsegf_get_new(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);

  for (i = 0; i < TRX_RSEG_N_SLOTS; i++) {
    page_no_t page_no;

    page_no = trx_rsegf_get_nth_undo(rseg_header, i, &mtr);

    /* In forced recovery: try to avoid operations which look
    at database pages; undo logs are rapidly changing data, and
    the probability that they are in an inconsistent state is
    high */

    if (page_no != FIL_NULL &&
        srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {
      trx_undo_t *undo;

      undo = trx_undo_mem_init(rseg, i, page_no, &mtr);

      size += undo->size;

      mtr_commit(&mtr);

      mtr_start(&mtr);

      rseg_header =
          trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);

      /* Found a used slot */
      MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);
    }
  }

  mtr_commit(&mtr);

  return (size);
}

/** Creates and initializes an undo log memory object.
@param[in]   rseg     rollback segment memory object
@param[in]   id       slot index within rseg
@param[in]   type     type of the log: TRX_UNDO_INSERT or TRX_UNDO_UPDATE
@param[in]   trx_id   id of the trx for which the undo log is created
@param[in]   xid      X/Open XA transaction identification
@param[in]   page_no  undo log header page number
@param[in]   offset   undo log header byte offset on page
@return own: the undo log memory object */
static trx_undo_t *trx_undo_mem_create(trx_rseg_t *rseg, ulint id, ulint type,
                                       trx_id_t trx_id, const XID *xid,
                                       page_no_t page_no, ulint offset) {
  trx_undo_t *undo;

  ut_ad(mutex_own(&(rseg->mutex)));

  ut_a(id < TRX_RSEG_N_SLOTS);

  undo = static_cast<trx_undo_t *>(ut_malloc_nokey(sizeof(*undo)));

  if (undo == NULL) {
    return (NULL);
  }

  undo->id = id;
  undo->type = type;
  undo->state = TRX_UNDO_ACTIVE;
  undo->del_marks = FALSE;
  undo->trx_id = trx_id;
  undo->xid = *xid;

  undo->dict_operation = FALSE;
  undo->flag = 0;
  undo->gtid_allocated = false;

  undo->rseg = rseg;

  undo->space = rseg->space_id;
  undo->page_size.copy_from(rseg->page_size);
  undo->hdr_page_no = page_no;
  undo->hdr_offset = offset;
  undo->last_page_no = page_no;
  undo->size = 1;

  undo->empty = TRUE;
  undo->top_page_no = page_no;
  undo->guess_block = NULL;
  undo->withdraw_clock = 0;

  return (undo);
}

/** Initializes a cached undo log object for new use. */
static void trx_undo_mem_init_for_reuse(
    trx_undo_t *undo, /*!< in: undo log to init */
    trx_id_t trx_id,  /*!< in: id of the trx for which the undo log
                      is created */
    const XID *xid,   /*!< in: X/Open XA transaction identification*/
    ulint offset)     /*!< in: undo log header byte offset on page */
{
  ut_ad(mutex_own(&((undo->rseg)->mutex)));

  ut_a(undo->id < TRX_RSEG_N_SLOTS);

  undo->state = TRX_UNDO_ACTIVE;
  undo->del_marks = FALSE;
  undo->trx_id = trx_id;
  undo->xid = *xid;

  undo->dict_operation = FALSE;
  undo->flag = 0;
  undo->gtid_allocated = false;

  undo->hdr_offset = offset;
  undo->empty = TRUE;
}

/** Frees an undo log memory copy. */
void trx_undo_mem_free(trx_undo_t *undo) /*!< in: the undo object to be freed */
{
  ut_a(undo->id < TRX_RSEG_N_SLOTS);

  ut_free(undo);
}

/** Create a new undo log in the given rollback segment.
@param[in]   trx    transaction
@param[in]   rseg   rollback segment memory copy
@param[in]   type   type of the log: TRX_UNDO_INSERT or TRX_UNDO_UPDATE
@param[in]   trx_id  id of the trx for which the undo log is created
@param[in]   xid     X/Open transaction identification
@param[in]   is_gtid if transaction has GTID
@param[out]  undo    the new undo log object, undefined if did not succeed
@param[in]   mtr     mini-transation
@retval DB_SUCCESS if successful in creating the new undo lob object,
@retval DB_TOO_MANY_CONCURRENT_TRXS
@retval DB_OUT_OF_FILE_SPACE
@retval DB_OUT_OF_MEMORY */
static MY_ATTRIBUTE((warn_unused_result)) dberr_t
    trx_undo_create(trx_t *trx, trx_rseg_t *rseg, ulint type, trx_id_t trx_id,
                    const XID *xid, bool is_gtid, trx_undo_t **undo,
                    mtr_t *mtr) {
  trx_rsegf_t *rseg_header;
  page_no_t page_no;
  ulint offset;
  ulint id;
  page_t *undo_page;
  dberr_t err;

  ut_ad(mutex_own(&(rseg->mutex)));

  if (rseg->curr_size == rseg->max_size) {
    return (DB_OUT_OF_FILE_SPACE);
  }

  rseg->curr_size++;

  rseg_header =
      trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, mtr);

  err = trx_undo_seg_create(rseg, rseg_header, type, &id, &undo_page, mtr);

  if (err != DB_SUCCESS) {
    /* Did not succeed */

    rseg->curr_size--;

    return (err);
  }

  page_no = page_get_page_no(undo_page);

  offset = trx_undo_header_create(undo_page, trx_id, mtr);

  bool add_space_gtid = (is_gtid && type == TRX_UNDO_UPDATE);
  trx_undo_header_add_space_for_xid(undo_page, undo_page + offset, mtr,
                                    add_space_gtid);

  *undo = trx_undo_mem_create(rseg, id, type, trx_id, xid, page_no, offset);
  if (*undo == NULL) {
    err = DB_OUT_OF_MEMORY;
  } else {
    (*undo)->gtid_allocated = add_space_gtid;
  }

  return (err);
}

/*================ UNDO LOG ASSIGNMENT AND CLEANUP =====================*/

/** Reuses a cached undo log.
@param[in,out]	trx	transaction
@param[in,out]	rseg	rollback segment memory object
@param[in]	type	type of the log: TRX_UNDO_INSERT or TRX_UNDO_UPDATE
@param[in]	trx_id	id of the trx for which the undo log is used
@param[in]	xid	X/Open XA transaction identification
@param[in]	is_gtid	if transaction has GTID
@param[in,out]	mtr	mini transaction
@return the undo log memory object, NULL if none cached */
static trx_undo_t *trx_undo_reuse_cached(trx_t *trx, trx_rseg_t *rseg,
                                         ulint type, trx_id_t trx_id,
                                         const XID *xid, bool is_gtid,
                                         mtr_t *mtr) {
  trx_undo_t *undo;

  ut_ad(mutex_own(&(rseg->mutex)));

  if (type == TRX_UNDO_INSERT) {
    undo = UT_LIST_GET_FIRST(rseg->insert_undo_cached);
    if (undo == NULL) {
      return (NULL);
    }

    UT_LIST_REMOVE(rseg->insert_undo_cached, undo);

    MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
  } else {
    ut_ad(type == TRX_UNDO_UPDATE);

    undo = UT_LIST_GET_FIRST(rseg->update_undo_cached);
    if (undo == NULL) {
      return (NULL);
    }

    UT_LIST_REMOVE(rseg->update_undo_cached, undo);

    MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
  }

  ut_ad(undo->size == 1);
  ut_a(undo->id < TRX_RSEG_N_SLOTS);

  auto undo_page = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                                     undo->page_size, mtr);

  bool add_space_gtid = false;
  ulint offset;

  if (type == TRX_UNDO_INSERT) {
    offset = trx_undo_insert_header_reuse(undo_page, trx_id, mtr);

    trx_undo_header_add_space_for_xid(undo_page, undo_page + offset, mtr,
                                      false);
  } else {
    ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) ==
         TRX_UNDO_UPDATE);

    offset = trx_undo_header_create(undo_page, trx_id, mtr);

    trx_undo_header_add_space_for_xid(undo_page, undo_page + offset, mtr,
                                      is_gtid);
    add_space_gtid = is_gtid;
  }

  trx_undo_mem_init_for_reuse(undo, trx_id, xid, offset);
  undo->gtid_allocated = add_space_gtid;

  return (undo);
}

/** Marks an undo log header as a header of a data dictionary operation
 transaction. */
static void trx_undo_mark_as_dict_operation(
    trx_t *trx,       /*!< in: dict op transaction */
    trx_undo_t *undo, /*!< in: assigned undo log */
    mtr_t *mtr)       /*!< in: mtr */
{
  page_t *hdr_page;

  hdr_page = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                               undo->page_size, mtr);

  mlog_write_ulint(hdr_page + undo->hdr_offset + TRX_UNDO_DICT_TRANS, TRUE,
                   MLOG_1BYTE, mtr);

  undo->dict_operation = TRUE;
}

/** Assigns an undo log for a transaction. A new undo log is created or a cached
 undo log reused.
 @return DB_SUCCESS if undo log assign successful, possible error codes
 are: DB_TOO_MANY_CONCURRENT_TRXS DB_OUT_OF_FILE_SPACE DB_READ_ONLY
 DB_OUT_OF_MEMORY */
dberr_t trx_undo_assign_undo(
    trx_t *trx,               /*!< in: transaction */
    trx_undo_ptr_t *undo_ptr, /*!< in: assign undo log from
                              referred rollback segment. */
    ulint type)               /*!< in: TRX_UNDO_INSERT or
                              TRX_UNDO_UPDATE */
{
  trx_rseg_t *rseg;
  trx_undo_t *undo;
  mtr_t mtr;
  dberr_t err = DB_SUCCESS;

  ut_ad(trx);

  /* In case of read-only scenario trx->rsegs.m_redo.rseg can be NULL but
  still request for assigning undo logs is valid as temporary tables
  can be updated in read-only mode.
  If there is no rollback segment assigned to trx and still there is
  object being updated there is something wrong and so this condition
  check. */
  ut_ad(trx_is_rseg_assigned(trx));

  rseg = undo_ptr->rseg;

  ut_ad(mutex_own(&(trx->undo_mutex)));

  bool no_redo = (&trx->rsegs.m_noredo == undo_ptr);

  /* If none of the undo pointers are assigned then this is
  first time transaction is allocating undo segment. */
  bool is_first =
      (undo_ptr->insert_undo == nullptr && undo_ptr->update_undo == nullptr);

  /* If any undo segment is assigned it is guaranteed that
  Innodb would persist GTID. Call it before any undo segment
  is assigned for transaction. We allocate space for GTID
  only if GTID is persisted. */
  bool is_gtid = false;
  if (!no_redo) {
    auto &gtid_persistor = clone_sys->get_gtid_persistor();
    if (is_first) {
      gtid_persistor.set_persist_gtid(trx, true);
    }
    /* Check if the undo segment needs to allocate for GTID. */
    is_gtid = gtid_persistor.persists_gtid(trx);
  }

  mtr_start(&mtr);
  if (no_redo) {
    mtr.set_log_mode(MTR_LOG_NO_REDO);
  } else {
    ut_ad(&trx->rsegs.m_redo == undo_ptr);
  }

  mutex_enter(&rseg->mutex);

  DBUG_EXECUTE_IF("ib_create_table_fail_too_many_trx",
                  err = DB_TOO_MANY_CONCURRENT_TRXS;
                  goto func_exit;);
  undo =
#ifdef UNIV_DEBUG
      srv_inject_too_many_concurrent_trxs
          ? nullptr
          :
#endif
          trx_undo_reuse_cached(trx, rseg, type, trx->id, trx->xid, is_gtid,
                                &mtr);

  if (undo == nullptr) {
    err = trx_undo_create(trx, rseg, type, trx->id, trx->xid, is_gtid, &undo,
                          &mtr);
    if (err != DB_SUCCESS) {
      goto func_exit;
    }
  }

  if (type == TRX_UNDO_INSERT) {
    UT_LIST_ADD_FIRST(rseg->insert_undo_list, undo);
    ut_ad(undo_ptr->insert_undo == NULL);
    undo_ptr->insert_undo = undo;
  } else {
    UT_LIST_ADD_FIRST(rseg->update_undo_list, undo);
    ut_ad(undo_ptr->update_undo == NULL);
    undo_ptr->update_undo = undo;
  }

  if (trx->mysql_thd && !trx->ddl_operation &&
      thd_is_dd_update_stmt(trx->mysql_thd)) {
    trx->ddl_operation = true;
  }

  if (trx->ddl_operation || trx_get_dict_operation(trx) != TRX_DICT_OP_NONE) {
    trx_undo_mark_as_dict_operation(trx, undo, &mtr);
  }

  /* For GTID persistence we might add undo segment to prepared transaction. If
  the transaction is in prepared state, we need to set XA properties. */
  if (trx_state_eq(trx, TRX_STATE_PREPARED)) {
    ut_ad(!is_first);
    undo->set_prepared(trx->xid);
  }

func_exit:
  mutex_exit(&(rseg->mutex));
  mtr_commit(&mtr);

  return (err);
}

/** Sets the state of the undo log segment at a transaction finish.
 @return undo log segment header page, x-latched */
page_t *trx_undo_set_state_at_finish(
    trx_undo_t *undo, /*!< in: undo log memory copy */
    mtr_t *mtr)       /*!< in: mtr */
{
  trx_usegf_t *seg_hdr;
  trx_upagef_t *page_hdr;
  page_t *undo_page;
  ulint state;

  ut_a(undo->id < TRX_RSEG_N_SLOTS);

  undo_page = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                                undo->page_size, mtr);

  seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
  page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

  if (undo->size == 1 && mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE) <
                             TRX_UNDO_PAGE_REUSE_LIMIT) {
    state = TRX_UNDO_CACHED;

  } else if (undo->type == TRX_UNDO_INSERT) {
    state = TRX_UNDO_TO_FREE;
  } else {
    state = TRX_UNDO_TO_PURGE;
  }

  undo->state = state;

  mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, state, MLOG_2BYTES, mtr);

  return (undo_page);
}

/**
 * dzw:
 * Write thd->owned_gtid if any into its undo log header.
 * */
void store_prepared_xa_gtid(trx_t *trx) {

  ut_ad(trx);
  trx_undo_t *undo = NULL;
  trx_usegf_t *seg_hdr;
  trx_ulogf_t *undo_header;
  page_t *undo_page;
  ulint offset;
  mtr_t mtr;
  THD *thd = trx->mysql_thd;
  ut_ad(thd);
  trx_undo_ptr_t *undo_ptr = &trx->rsegs.m_redo;
  undo = undo_ptr->update_undo;

  /*
   * If there is no update undo log, no gtid is possibly generated nor can be
   * written, and we have nothing to do.
   * */
  if (!undo) {
    ut_ad(thd->owned_gtid.is_empty() ||
          thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS);
    return;
  }

  // Check that trx is in PREPARED state and is an external XA txn.
  ut_ad(undo->state == TRX_UNDO_PREPARED && !trx_is_mysql_xa(trx));

  ut_a(undo->id < TRX_RSEG_N_SLOTS);
  
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  bool alloc = gtid_persistor.trx_check_set(trx, true/*prepare*/, false);
  if (!trx->persists_gtid)
    return;

  ut_ad(alloc && trx->persists_gtid);
  trx_rseg_t *rseg = undo_ptr->rseg;

  mtr_start_sync(&mtr);
  mutex_enter(&rseg->mutex);
  undo_page = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                                undo->page_size, &mtr);

  seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

  offset = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
  undo_header = undo_page + offset;
  /*
   * If no gtid is generated for the txn, probably because gtid_mode is < 2,
   * then clear the GTID flag.
   * */
  if (thd->owned_gtid.is_empty() ||
      thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS) {
    undo->flag &= ~(TRX_UNDO_FLAG_GTID);
    mlog_write_ulint(undo_header + TRX_UNDO_FLAGS, undo->flag, MLOG_1BYTE, &mtr);
    goto done;
  }

  undo->gtid_allocated = true;

  /* Write GTID information if there. */
  ut_ad(undo->flag & TRX_UNDO_FLAG_GTID);
  trx_undo_gtid_write(trx, undo_header, undo, &mtr);
done:
  mutex_exit(&rseg->mutex);
  mtr_commit(&mtr);
}

void add_gtid_to_persistor(trx_t *trx)
{
  THD *thd = trx->mysql_thd;
  ut_ad(thd);
  if (thd->owned_gtid.is_empty() ||
      thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS)
    return;

  /* Check and get GTID to be persisted. Do it outside trx_sys mutex. */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  Gtid_desc gtid_desc;
  gtid_persistor.get_gtid_info(trx, gtid_desc);
  /* Add GTID to be persisted to disk table, if needed. 
   * */
  if (gtid_desc.m_is_set) {
    trx_sys_mutex_enter();
    gtid_persistor.add(gtid_desc);
    trx_sys_mutex_exit();
  }
  DBUG_EXECUTE_IF("ib_crash_after_gtid_added_to_persistor", DBUG_SUICIDE(););

}

/** Set the state of the undo log segment at a XA PREPARE or XA ROLLBACK.
@param[in,out]	trx		transaction
@param[in,out]	undo		insert_undo or update_undo log
@param[in]	rollback	false=XA PREPARE, true=XA ROLLBACK
@param[in,out]	mtr		mini-transaction
@return undo log segment header page, x-latched */
page_t *trx_undo_set_state_at_prepare(trx_t *trx, trx_undo_t *undo,
                                      bool rollback, mtr_t *mtr) {
  trx_usegf_t *seg_hdr;
  trx_ulogf_t *undo_header;
  page_t *undo_page;
  ulint offset;

  ut_ad(trx && undo && mtr);

  ut_a(undo->id < TRX_RSEG_N_SLOTS);

  undo_page = trx_undo_page_get(page_id_t(undo->space, undo->hdr_page_no),
                                undo->page_size, mtr);

  seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

  offset = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
  undo_header = undo_page + offset;

  /* Write GTID information if there is, no op otherwise.
   * dzw:
   * XA PREPARE now has no valid gtid generated here.
   * */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  bool thd_check;
  MYSQL_THD trx_thd = trx->mysql_thd;
  if (gtid_persistor.has_gtid(trx, trx_thd, thd_check))
    trx_undo_gtid_write(trx, undo_header, undo, mtr);

  if (rollback) {
    ut_ad(undo->state == TRX_UNDO_PREPARED);
    mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE, MLOG_2BYTES,
                     mtr);
    return (undo_page);
  }

  ut_ad(undo->state == TRX_UNDO_ACTIVE);
  undo->set_prepared(trx->xid);

  mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, undo->state, MLOG_2BYTES, mtr);

  if (trx->mysql_thd->is_one_phase_commit()) {
    undo->flag |= TRX_UNDO_FLAG_ONE_PHASE_PREPARED;
    trx->one_phase_prepared = true;
  } else {
    undo->flag &= ~TRX_UNDO_FLAG_ONE_PHASE_PREPARED;
    trx->one_phase_prepared = false;
  }

  mlog_write_ulint(undo_header + TRX_UNDO_FLAGS, undo->flag, MLOG_1BYTE, mtr);

  trx_undo_write_xid(undo_header, &undo->xid, mtr);

  if (!rollback && undo->type == TRX_UNDO_UPDATE) {
    init_xa_gtid_slots(trx, undo_header, undo, mtr);
  }

  return (undo_page);
}

/** Adds the update undo log header as the first in the history list, and
 frees the memory object, or puts it to the list of cached update undo log
 segments. */
void trx_undo_update_cleanup(
    trx_t *trx,               /*!< in: trx owning the update
                              undo log */
    trx_undo_ptr_t *undo_ptr, /*!< in: update undo log. */
    page_t *undo_page,        /*!< in: update undo log header page,
                              x-latched */
    bool update_rseg_history_len,
    /*!< in: if true: update rseg history
    len else skip updating it. */
    ulint n_added_logs, /*!< in: number of logs added */
    mtr_t *mtr)         /*!< in: mtr */
{
  trx_rseg_t *rseg;
  trx_undo_t *undo;

  undo = undo_ptr->update_undo;
  rseg = undo_ptr->rseg;

  ut_ad(mutex_own(&(rseg->mutex)));

  trx_purge_add_update_undo_to_history(
      trx, undo_ptr, undo_page, update_rseg_history_len, n_added_logs, mtr);

  UT_LIST_REMOVE(rseg->update_undo_list, undo);

  undo_ptr->update_undo = NULL;

  if (undo->state == TRX_UNDO_CACHED) {
    UT_LIST_ADD_FIRST(rseg->update_undo_cached, undo);

    MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
  } else {
    ut_ad(undo->state == TRX_UNDO_TO_PURGE);

    trx_undo_mem_free(undo);
  }
}

/** Frees an insert undo log after a transaction commit or rollback.
Knowledge of inserts is not needed after a commit or rollback, therefore
the data can be discarded.
@param[in,out]	undo_ptr	undo log to clean up
@param[in]	noredo		whether the undo tablespace is redo logged */
void trx_undo_insert_cleanup(trx_undo_ptr_t *undo_ptr, bool noredo) {
  trx_undo_t *undo;
  trx_rseg_t *rseg;

  undo = undo_ptr->insert_undo;
  ut_ad(undo != NULL);

  rseg = undo_ptr->rseg;

  ut_ad(noredo == fsp_is_system_temporary(rseg->space_id));

  mutex_enter(&(rseg->mutex));

  UT_LIST_REMOVE(rseg->insert_undo_list, undo);
  undo_ptr->insert_undo = NULL;

  if (undo->state == TRX_UNDO_CACHED) {
    UT_LIST_ADD_FIRST(rseg->insert_undo_cached, undo);

    MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
  } else {
    ut_ad(undo->state == TRX_UNDO_TO_FREE);

    /* Delete first the undo log segment in the file */

    mutex_exit(&(rseg->mutex));

    trx_undo_seg_free(undo, noredo);

    mutex_enter(&(rseg->mutex));

    ut_ad(rseg->curr_size > undo->size);

    rseg->curr_size -= undo->size;

    trx_undo_mem_free(undo);
  }

  mutex_exit(&(rseg->mutex));
}

/** At shutdown, frees the undo logs of a PREPARED transaction. */
void trx_undo_free_prepared(trx_t *trx) /*!< in/out: PREPARED transaction */
{
  ut_ad(srv_shutdown_state.load() == SRV_SHUTDOWN_EXIT_THREADS);

  if (trx->rsegs.m_redo.update_undo) {
    ut_a(trx->rsegs.m_redo.update_undo->state == TRX_UNDO_PREPARED);
    UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->update_undo_list,
                   trx->rsegs.m_redo.update_undo);
    trx_undo_mem_free(trx->rsegs.m_redo.update_undo);

    trx->rsegs.m_redo.update_undo = NULL;
  }

  if (trx->rsegs.m_redo.insert_undo) {
    ut_a(trx->rsegs.m_redo.insert_undo->state == TRX_UNDO_PREPARED);
    UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->insert_undo_list,
                   trx->rsegs.m_redo.insert_undo);
    trx_undo_mem_free(trx->rsegs.m_redo.insert_undo);

    trx->rsegs.m_redo.insert_undo = NULL;
  }

  if (trx->rsegs.m_noredo.update_undo) {
    ut_a(trx->rsegs.m_noredo.update_undo->state == TRX_UNDO_PREPARED);

    UT_LIST_REMOVE(trx->rsegs.m_noredo.rseg->update_undo_list,
                   trx->rsegs.m_noredo.update_undo);
    trx_undo_mem_free(trx->rsegs.m_noredo.update_undo);

    trx->rsegs.m_noredo.update_undo = NULL;
  }
  if (trx->rsegs.m_noredo.insert_undo) {
    ut_a(trx->rsegs.m_noredo.insert_undo->state == TRX_UNDO_PREPARED);

    UT_LIST_REMOVE(trx->rsegs.m_noredo.rseg->insert_undo_list,
                   trx->rsegs.m_noredo.insert_undo);
    trx_undo_mem_free(trx->rsegs.m_noredo.insert_undo);

    trx->rsegs.m_noredo.insert_undo = NULL;
  }
}

bool trx_undo_truncate_tablespace(undo::Tablespace *marked_space) {
#ifdef UNIV_DEBUG
  static int truncate_fail_count;
  DBUG_EXECUTE_IF(
      "ib_undo_trunc_fail_truncate", if (++truncate_fail_count == 1) {
        ib::info() << "ib_undo_trunc_fail_truncate";
        return (false);
      });
#endif /* UNIV_DEBUG */

  bool success = true;
  space_id_t old_space_id = marked_space->id();
  space_id_t space_num = undo::id2num(old_space_id);
  Rsegs *marked_rsegs = marked_space->rsegs();

  undo::unuse_space_id(old_space_id);

  space_id_t new_space_id = undo::use_next_space_id(space_num);

  fil_space_t *space = fil_space_get(old_space_id);
  bool is_encrypted = FSP_FLAGS_GET_ENCRYPTION(space->flags);

  /* Step-1: Truncate tablespace by replacement with a new space_id. */
  success = fil_replace_tablespace(old_space_id, new_space_id,
                                   SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);
  if (!success) {
    return (success);
  }

  DBUG_EXECUTE_IF("ib_undo_trunc_empty_file",
                  ib::info(ER_IB_MSG_UNDO_TRUNC_EMPTY_FILE);
                  DBUG_SUICIDE(););

  /* This undo tablespace is unused. Lock the Rsegs before the
  file_space because SYNC_RSEGS > SYNC_FSP. */
  marked_rsegs->x_lock();

  /* Step-2: Re-initialize tablespace header.
  Avoid REDO logging as we don't want to apply the action if server
  crashes. For fix-up we have UNDO-truncate-ddl-log. */
  log_free_check();
  mtr_t mtr;
  mtr_start(&mtr);
  mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
  /* Why return value is not being checked here? */
  fsp_header_init(new_space_id, SRV_UNDO_TABLESPACE_SIZE_IN_PAGES, &mtr, false);

  /* Step-3: Add the RSEG_ARRAY page. */
  trx_rseg_array_create(new_space_id, &mtr);
  mtr_commit(&mtr);

  /* Step-4: Re-initialize rollback segment header that resides
  in truncated tablespaces. */

  DBUG_EXECUTE_IF("ib_undo_trunc_before_rsegs",
                  ib::info(ER_IB_MSG_UNDO_TRUNK_BEFORE_RSEG);
                  DBUG_SUICIDE(););

  for (auto rseg : *marked_rsegs) {
    trx_rsegf_t *rseg_header;

    log_free_check();
    mtr_start(&mtr);
    mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
    mtr_x_lock(fil_space_get_latch(new_space_id), &mtr);

    rseg->space_id = new_space_id;

    rseg->page_no = trx_rseg_header_create(new_space_id, univ_page_size,
                                           PAGE_NO_MAX, rseg->id, &mtr);

    ut_a(rseg->page_no != FIL_NULL);

    rseg_header =
        trx_rsegf_get_new(new_space_id, rseg->page_no, rseg->page_size, &mtr);

    /* Before re-initialization ensure that we free the existing
    structure. There can't be any active transactions. */
    ut_a(UT_LIST_GET_LEN(rseg->update_undo_list) == 0);
    ut_a(UT_LIST_GET_LEN(rseg->insert_undo_list) == 0);

    trx_undo_t *next_undo;

    for (trx_undo_t *undo = UT_LIST_GET_FIRST(rseg->update_undo_cached);
         undo != NULL; undo = next_undo) {
      next_undo = UT_LIST_GET_NEXT(undo_list, undo);
      UT_LIST_REMOVE(rseg->update_undo_cached, undo);
      MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
      trx_undo_mem_free(undo);
    }

    for (trx_undo_t *undo = UT_LIST_GET_FIRST(rseg->insert_undo_cached);
         undo != NULL; undo = next_undo) {
      next_undo = UT_LIST_GET_NEXT(undo_list, undo);
      UT_LIST_REMOVE(rseg->insert_undo_cached, undo);
      MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
      trx_undo_mem_free(undo);
    }

    UT_LIST_INIT(rseg->update_undo_list, &trx_undo_t::undo_list);
    UT_LIST_INIT(rseg->update_undo_cached, &trx_undo_t::undo_list);
    UT_LIST_INIT(rseg->insert_undo_list, &trx_undo_t::undo_list);
    UT_LIST_INIT(rseg->insert_undo_cached, &trx_undo_t::undo_list);

    rseg->max_size =
        mtr_read_ulint(rseg_header + TRX_RSEG_MAX_SIZE, MLOG_4BYTES, &mtr);

    /* Initialize the undo log lists according to the rseg header */
    rseg->curr_size =
        mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, &mtr) +
        1;

    mtr_commit(&mtr);

    ut_ad(rseg->curr_size == 1);
    ut_ad(rseg->trx_ref_count == 0);

    rseg->last_page_no = FIL_NULL;
    rseg->last_offset = 0;
    rseg->last_trx_no = 0;
    rseg->last_del_marks = FALSE;
  }

  /* If tablespace is to be encrypted, encrypt it now */
  if (is_encrypted && srv_undo_log_encrypt) {
    mtr_t mtr;
    mtr_start(&mtr);
    ut_d(bool ret =)
        set_undo_tablespace_encryption(nullptr, new_space_id, &mtr, false);
    /* Don't expect any error here (unless keyring plugin is uninstalled). In
    that case too, continue truncation processing of tablespace. */
    ut_ad(!ret);
    mtr_commit(&mtr);
  }

  marked_rsegs->x_unlock();

  /* Increment the space ID for this undo space now so that if anyone refers
  to this space, it is completely initialized. */
  marked_space->set_space_id(new_space_id);

  return (success);
}

#endif /* !UNIV_HOTBACKUP */
