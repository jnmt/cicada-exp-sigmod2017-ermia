#include "sm-log-alloc.h"
#include "stopwatch.h"

using namespace RCU;

namespace {

    uint64_t
    get_starting_byte_offset(sm_log_recover_mgr *lm)
    {
        auto dlsn = lm->get_durable_mark();
        auto *sid = lm->get_segment(dlsn.segment());
        return sid->offset(dlsn);
    }

    extern "C"
    void*
    log_write_daemon_thunk(void *arg)
    {
        ((sm_log_alloc_mgr*) arg)->_log_write_daemon();
        return NULL;
    }

} // end anonymous namespace

/* We have to find the end of the log files on disk before
   constructing the log buffer in memory. It's also a convenient time
   to do the rest of recovery, because it prevents any attempt at
   forward processing before recovery completes. 
 */
sm_log_alloc_mgr::sm_log_alloc_mgr(char const *dname, size_t segment_size,
                                   sm_log_recover_function *rfn, void *rfn_arg,
                                   size_t bufsz)
    : _lm(dname, segment_size, rfn, rfn_arg)
    , _logbuf(bufsz, get_starting_byte_offset(&_lm))
    , _durable_lsn_offset(_lm.get_durable_mark().offset())
    , _waiting_for_durable(0)
    , _waiting_for_dmark(0)
    , _write_daemon_wait_count(0)
    , _write_daemon_kick_count(0)
    , _write_daemon_should_stop(false)
{

    // prime the block list
    log_allocation *x = rcu_alloc();
    x->lsn_offset = x->next_lsn_offset = _lm.get_durable_mark().offset();
    bool success = _block_list.push(x);
    ASSERT(success);
    _block_list.remove_fast(x);

    // fire up the log writing daemon
    _write_daemon_mutex.lock();
    DEFER(_write_daemon_mutex.unlock());
    
    int err = pthread_create(&_write_daemon_tid, NULL,
                             &log_write_daemon_thunk, this);
    THROW_IF(err, os_error, err, "Unable to start log writer daemon thread");

}

sm_log_alloc_mgr::~sm_log_alloc_mgr()
{
    {
        _write_daemon_mutex.lock();
        DEFER(_write_daemon_mutex.unlock());
        
        _write_daemon_should_stop = true;
        _kick_log_write_daemon();
    }
    
    int err = pthread_join(_write_daemon_tid, NULL);
    THROW_IF(err, os_error, err, "Unable to join log writer daemon thread");
}

uint64_t
sm_log_alloc_mgr::cur_lsn_offset()
{
    return _block_list.peek_raw(0)->next_lsn_offset;
}

uint64_t
sm_log_alloc_mgr::dur_lsn_offset()
{
    return volatile_read(_durable_lsn_offset);
}

void
sm_log_alloc_mgr::wait_for_durable(uint64_t dlsn_offset)
{
    while (dur_lsn_offset() < dlsn_offset) {
        _write_daemon_mutex.lock();
        DEFER(_write_daemon_mutex.unlock());
        if (_waiting_for_durable < dlsn_offset)
            _waiting_for_durable = dlsn_offset;
        
        _kick_log_write_daemon();
        _write_complete_cond.wait(_write_daemon_mutex);
    }
}

void
sm_log_alloc_mgr::update_durable_mark(uint64_t lsn_offset)
{
    wait_for_durable(lsn_offset);
    _write_daemon_mutex.lock();
    DEFER(_write_daemon_mutex.unlock());
    while (_lm.get_durable_mark().offset() < lsn_offset) {
        if (_waiting_for_dmark < lsn_offset)
            _waiting_for_dmark = lsn_offset;
        
        _kick_log_write_daemon();
        _write_complete_cond.wait(_write_daemon_mutex);
    }
}

/* Allocating a log block is a multi-step process.

   1. Ensure there is sufficient space in the log file for the new
      block. We have to ensure there is always enough log space to
      reclaim at least one segment, or the log could become "wedged"
      (where log reclamation cannot proceed because the log is
      full). Sequence number allocation is not easily undone, so it's
      better to prevent this particular problem than to cure it.
   
   2. Acquire a sequence number by incrementing the log counter. The
      result is almost an LSN, but lacks log segment information.

   3. Identify the block's log segment. Most of the time this is as
      simple as looking up the currently active segment (and verifying
      that it contains the obtained sequence number), but segment
      boundaries complicate things. Due to the way we install new log
      segments, each segment change involves a pattern like the
      following:

      | ... segment i | dead zone | segment i+1 ... |
          |   A   |   B   |   C   |   D   |   E   |

      Block A is the common case discussed already, and does not
      overlap with the segment change. Block B overflows the segment
      and is thus unusable; the owner of that block is responsible to
      "close" the segment by logging a "segment change" record (really
      just a skip record) so that recovery proceeds to the new segment
      rather than truncating the log. Block C lost the race to install
      a new segment, and ended up in the "dead zone" between the two
      segments; that block does not map to any physical location in
      the log and must be discarded. Block D won the race to install
      the new segment, and thus becomes the first block of the new
      segment. Block E lost the segment-change race, but was lucky to
      have a predecessor win. It becomes a valid block in the new
      segment once the dust settles.

   4. Wait for buffer space to become available. A fixed-size buffer
      holds a sliding window of the log, with space for new records
      becoming available as old ones reach disk. Assuming the log
      cannot become wedged, it's just a matter of time until the
      buffer space is ready.
               
 */
log_allocation *
sm_log_alloc_mgr::allocate(uint32_t nrec, size_t payload_bytes)
{
#warning TODO: protocol to prevent log from becoming wedged
    /* ^^^

       In any logging scheme that uses checkpoints to reclaim log
       space, a catch-22 lies in wait for the unwary implementor: the
       checkpoint must be logged, so the log will become permanently
       wedged if we allow it to completely fill. The solution is to
       reserve some amount of log space for an "emergency" checkpoint
       that can reclaim at least one segment and avert disaster. In
       our case, checkpointing doesn't actually let us reclaim space,
       but the segment-recovery protocol we use has the same problem.

       Unfortunately, our single-CAS scheme for acquiring a LSN offset
       means we can't easily detect that the log is almost full until
       after we've already acquired an LSN and made the problem worse.

       The (as yet unimplemented) solution to this quandary is for
       each thread to check whether its newly-acquired block lands in
       a "red zone" near the end of the log capacity. If so, it must
       discard the record, abort, and block until space has been
       reclaimed. The red zone has to be large enough that every
       transaction-executing thread in the system could make a
       maximum-sized request and still leave room for a checkpoint.
     */

#warning TODO: prevent reclamation of uncommitted overflow records
    /* ^^^

       Before the system can reclaim a log segment, it must ensure
       that the segment contains no uncommitted overflow blocks. The
       simplest way is to wait for all in-flight transactions to end,
       if we know write transactions will end reasonably
       soon. Alternatively, we could track the oldest uncommitted LSN
       generated by each transaction (a loose lower bound should
       suffice) and only do the wait if that bound impinges on the
       segment we're trying to reclaim.
     */

    
    ASSERT (is_aligned(payload_bytes));
    typedef std::pair<size_t&, rcu_block_list&> cb_arg;
    auto cb = [](cb_arg *arg, log_allocation *n, log_allocation *)->void {
        log_allocation *prev = arg->second.peek_raw(0);
        uint64_t offset = n->lsn_offset = prev->next_lsn_offset;
        n->next_lsn_offset = offset + arg->first;
    };
    
    /* Step #1: join the log list to obtain an LSN offset.

       All we need here is the LSN offset for the new block; we don't
       yet know what segment (if any) actually contains that offset.
     */
 start_over:
    log_allocation *x = rcu_alloc();
    size_t nbytes = log_block::size(nrec, payload_bytes);
    cb_arg arg{nbytes, _block_list};
    bool inserted = _block_list.push_callback(x, cb, &arg);
    DIE_IF(not inserted, "Attempted log insert after shutdown");

    /* We are now the proud owners of an LSN offset range, most likely
       backed by space on disk. If the rest of the insert protocol
       succeeds, the caller becomes responsible for releasing the
       block properly. However, a hole in the log will result if any
       unexpected exception interrupts the allocation protocol.

       Why? DEFER will delete the node from the block list on abnormal
       return, but leaving the corresponding physical log space
       uninitialized would effectively truncate the log at that
       point. An abnormal return means we *can't* write the log record
       to disk for whatever reason, so we DIE instead to be safe.
    */
    DEFER_UNLESS(node_used, _block_list.remove_fast(x));
    DEFER_UNLESS(normal_return,
                 DIE("Log allocation did not complete normally. "
                     "Terminating execution to avoid losing committed work."));

    /* Step #2: assign the range to a segment 
     */
    auto rval = _lm.assign_segment(x->lsn_offset, x->next_lsn_offset);
    auto *sid = rval.sid;
    if (not sid) {
        /* Invalid offset (not inside any segment) must be discarded
           because it has no physical location on disk where we could
           write a log entry. DEFER releases the node.
         */
        normal_return = true;
        goto start_over;
    }
    
    LSN lsn = sid->make_lsn(x->lsn_offset);
    
    /* Step #3: claim buffer space (wait if it's not yet available).

       Save copies of the request parameters in case our block went
       past-end and we have to retry.
     */
    auto tmp_nbytes = nbytes;
    auto tmp_nrec = nrec;
    auto tmp_payload_bytes = payload_bytes;
    if (not rval.full_size) {
        /* Block didn't fit in the available space. Adjust the request
           parameters so we create an empty log block.
         */
        uint64_t newsz = sid->end_offset - x->lsn_offset;
        ASSERT(newsz < nbytes);
        tmp_nbytes = newsz;
        tmp_nrec = 0;
        tmp_payload_bytes = 0;
    }

 grab_buffer:
    char *buf = _logbuf.write_buf(sid->buf_offset(lsn), tmp_nbytes);
    if (not buf) {
        /* Unavailable write buffer space is due to unconsumed reads,
           which in turn are really just due to non-durable
           log. Figure out which durable LSN corresponds to the buffer
           space we need, and wait for it. The nonlinear mapping
           between buffer offsets and LSN offsets means we may guess
           high, but that's harmless.
         */
        uint64_t needed = lsn.offset() - _logbuf.window_size();
        _write_daemon_mutex.lock();
        DEFER(_write_daemon_mutex.unlock());

        if(_waiting_for_durable < needed)
            _waiting_for_durable = needed;
        
        _kick_log_write_daemon();
        _write_complete_cond.wait(_write_daemon_mutex);
        goto grab_buffer;
    }

    log_block *b = x->block = (log_block*) buf;
    b->lsn = lsn;
    b->nrec = tmp_nrec;
    fill_skip_record(&b->records[tmp_nrec], rval.next_lsn, tmp_payload_bytes, false);
    node_used = true;
    normal_return = true;

    if (not rval.full_size) {
        /* Discard the undersized block */
        discard(x);
        goto start_over;
    }

    // success!
    return x;
}

void
sm_log_alloc_mgr::release(log_allocation *x)
{
    /* Short and sweet for the common case */
    _block_list.remove_fast(x);

    /* Hopefully the log daemon is already awake, but be ready to give
       it a kick if need be.
     */
    if (_write_daemon_kick_count < _write_daemon_wait_count) {
        _write_daemon_mutex.lock();
        DEFER(_write_daemon_mutex.unlock());

        _kick_log_write_daemon();
    }
}

void
sm_log_alloc_mgr::discard(log_allocation *x)
{
    /* Move the skip to front, set payload size to zero, and compute
       the resulting checksum. Then release as normal.
     */
    log_block *b = x->block;
    size_t nrec = b->nrec;
    ASSERT(b->records[nrec].type == LOG_SKIP);
    b->records[0] = b->records[nrec];
    b->records[0].payload_end = 0;
    b->nrec = 0;
    b->checksum = b->full_checksum();
    release(x);
}
    
/* This guy's only job is to write released log blocks to disk. In
   steady state, new log blocks will be released during each log
   write, keeping the daemon busy most of the time. Whenever the log
   is fully durable, it sleeps. During a clean shutdown, the daemon
   will exit only after it has written everything to disk. It is the
   system's responsibility to ensure that the shutdown flag is not
   raised while new log records might still be generated.
 */
void
sm_log_alloc_mgr::_log_write_daemon()
{
    rcu_register();
    rcu_enter();
    DEFER(rcu_exit());
    
    typedef std::pair<size_t&, rcu_block_list&> cb_arg;
    auto cb = [](cb_arg *arg, log_allocation *n, log_allocation *)->void {
        log_allocation *prev = arg->second.peek_raw(0);
        arg->first = n->next_lsn_offset = n->lsn_offset = prev->next_lsn_offset;
    };

    LSN dlsn = _lm.get_durable_mark();
    auto *durable_sid = _lm.get_segment(dlsn.segment());
    ASSERT(_durable_lsn_offset == dlsn.offset());
    uint64_t durable_byte = durable_sid->buf_offset(_durable_lsn_offset);
    int active_fd = _lm.open_for_write(durable_sid);
    DEFER(os_close(active_fd));

    auto update_dmark = [&] {
        dlsn = durable_sid->make_lsn(_durable_lsn_offset);
        _lm.update_durable_mark(dlsn);
    };

    // every 100 ms or so, update the durable mark on disk
    static uint64_t const DURABLE_MARK_TIMEOUT_NS = uint64_t(100)*1000*1000;
    stopwatch_t timer;
    for (;;) {
        rcu_quiesce();
        
        stopwatch_t tmp = timer;
        auto dmark_offset = _lm.get_durable_mark().offset();
        bool can_update = dmark_offset < _durable_lsn_offset;
        bool want_update = _lm.get_durable_mark().offset() < _waiting_for_dmark;
        bool timeout = DURABLE_MARK_TIMEOUT_NS < tmp.time_ns();
        if (can_update and (want_update or timeout)) {
            update_dmark();
            timer.reset();
            if (want_update) 
                _write_complete_cond.broadcast();
        }

        /* The block list contains a fluctuating---and usually fairly
           short---set of log_allocation objects. Releasing or
           discarding a block marks it as dead (without removing it)
           and removes all dead blocks that follow it. The list is
           primed at start-up with the durable LSN (as determined by
           startup/recovery), and so is guaranteed to always contain
           at least one (perhaps dead) node that later requests can
           use to acquire a proper LSN.

           Our goal is to find the oldest (= last) live block in the
           list, and write out everything before that block's offset.

           Once we know the offset, we can look up the corresponding
           segment to obtain an LSN.
         */
        uint64_t cur_offset = cur_lsn_offset();

        // Find the offset of the oldest live allocation
        uint64_t oldest_offset = cur_offset;
        for (auto &x : _block_list)
            oldest_offset = x.lsn_offset;

        if (oldest_offset == _durable_lsn_offset) {
            _write_daemon_mutex.lock();
            DEFER(_write_daemon_mutex.unlock());

            /* Before blocking: did somebody ask to update the durable
               mark, and we are able to do so?
             */
            auto dmark_offset = _lm.get_durable_mark().offset();
            if (dmark_offset < _waiting_for_dmark and _waiting_for_dmark <= _durable_lsn_offset)
                continue;
            
            _write_complete_cond.broadcast();

            // Nothing to write out
            if (_durable_lsn_offset == cur_offset and volatile_read(_write_daemon_should_stop)) {
                if (dmark_offset < _durable_lsn_offset)
                    update_dmark();
                
                log_allocation *x = rcu_alloc();
                cb_arg arg{cur_offset, _block_list};
                bool inserted = _block_list.push_callback(x, cb, &arg);
                ASSERT(inserted);
                DEFER_UNLESS(node_removed, _block_list.remove_fast(x));
                
                if (oldest_offset == cur_offset) {
                    node_removed = true;
                    if (_block_list.remove_and_kill(x)) {
                        DIE_IF(_durable_lsn_offset < _waiting_for_durable,
                               "Thread(s) waiting for past-end durable LSN at log shutdown");
                        DIE_IF(_durable_lsn_offset < _waiting_for_dmark,
                               "Thread(s) waiting for past-end durable mark at log shutdown");

                        return;
                    }

                    // another block slipped in, fall out and deal with it
                }
            }

            // wait for a kick (false wakeups are acceptable)
            _write_daemon_wait_count++;
            _write_daemon_cond.wait(_write_daemon_mutex);
            continue;
        }

        /* All right! We have some amount of data to write out,
           possibly spanning multiple segments. Finish writing out
           each segment before continuing on to the next.
        */
        while (_durable_lsn_offset < oldest_offset) {
            sm_log_recover_mgr::segment_id *new_sid;
            uint64_t new_offset;
            uint64_t new_byte;
            
            if (durable_sid->end_offset < oldest_offset+MIN_LOG_BLOCK_SIZE) {
                /* Watch out for segment boundaries!

                   The true end of a segment is somewhere in the last
                   MIN_LOG_BLOCK_SIZE bytes, with the exact value
                   determined by the start_offset of its
                   successor. Fortunately, any request that lands in
                   this "red zone" also ensures that the next segment
                   has been created, so we can safely access it.
                 */
                new_sid = _lm.get_segment((durable_sid->segnum+1) % NUM_LOG_SEGMENTS);
                ASSERT(new_sid);
                new_offset = new_sid->start_offset;
                new_byte = new_sid->byte_offset;
            }
            else {
                new_sid = durable_sid;
                new_offset = oldest_offset;
                new_byte = new_sid->buf_offset(oldest_offset);
            }

            ASSERT(durable_byte == _logbuf.read_begin());
            ASSERT(durable_byte < new_byte);
            ASSERT(new_byte <= _logbuf.write_end());

            /* Log insertions don't advance the buffer window because
               they tend to complete out of order. Do it for them now
               that we know the correct value to use.
             */
            _logbuf.advance_writer(new_byte);
            
            // perform the write
            uint64_t nbytes = new_byte - durable_byte;
            auto *buf = _logbuf.read_buf(durable_byte, nbytes);
            auto file_offset = durable_sid->offset(_durable_lsn_offset);
            uint64_t n = os_pwrite(active_fd, buf, nbytes, file_offset);
            THROW_IF(n < nbytes, log_file_error, "Incomplete log write");
            _logbuf.advance_reader(new_byte);

            // segment change?
            if (new_sid != durable_sid) {
                os_close(active_fd);
                active_fd = _lm.open_for_write(new_sid);
            }

            _write_daemon_mutex.lock();
            DEFER(_write_daemon_mutex.unlock());

            // wake up any waiters if the old value was smaller than the waited-for one
            if (_durable_lsn_offset < _waiting_for_durable) 
                _write_complete_cond.broadcast();
            
            // update values for next round
            durable_sid = new_sid;
            _durable_lsn_offset = new_offset;
            durable_byte = new_byte;
        }
    }
}

/* Wake up the log write daemon if it happens to be alseep.

   WARNING: caller must hold the log write mutex!
 */
void
sm_log_alloc_mgr::_kick_log_write_daemon()
{
    if (_write_daemon_kick_count < _write_daemon_wait_count) {
        _write_daemon_kick_count++;
        _write_daemon_cond.signal();
    }
}