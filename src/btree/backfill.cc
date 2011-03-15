#include "btree/backfill.hpp"

#include <algorithm>
#include <vector>

// TODO make this user-configurable.
#define BACKFILLING_MAX_BREADTH_FIRST_BLOCKS 50000
#define BACKFILLING_MAX_PENDING_BLOCKS 40000

// Backfilling

// We want a backfill operation to follow a few simple rules.
//
// 1. Get as far away from the root as possible.
//
// 2. Avoid using more than K + D blocks, for some user-selected
// constant K, where D is the depth of the tree.
//
// 3. Prefetch efficiently.
//
// This code should be nice to genericize; you could reimplement rget
// if you genericized this.  There are some performance things (like
// getting the recency of all an internal node's block ids at once)
// that need to be smart but are doable.


// The Lifecyle of a block_id_t
//
// Every time we deal with a block_id_t, it goes through these states...
//
// 1. Knowledge of the block_id_t.  This is where we know about the
// block_id_t, and haven't done anything about it yet.
//
// 2. Acquiring its subtree_recency value from the serializer.  The
// block_id_t is grouped with a bunch of others in an array, and we've
// sent a request to the serializer to respond with all these
// subtree_recency values (and the original array).
//
// 3. Acquired the subtree_recency value.  The block_id_t's
// subtree_recency is known, but we still have not attempted to
// acquire the block.  (If the recency is insufficiently recent, we
// stop here.)
//
// 4. Block acquisition pending.  We have sent a request to acquire
// the block.  It has not yet successfully completed.
//
// 5I. Block acquisition complete, it's an internal node, partly
// processed children.  We hold the lock on the block, and the
// children blocks are currently being processed and have not reached
// stage 4.
//
// 6I. Live children all reached stage 4.  We can now release ownership
// of the block.  We stop here.
//
// 5L. Block acquisition complete, it's a leaf node, we may have to
// handle large values.
//
// 6L. Large values all pending or better, so we can release ownership
// of the block.  We stop here.

class backfill_state_t {
public:
    backfill_state_t(btree_slice_t *_slice, repli_timestamp _since_when, backfill_callback_t *_callback)
        : slice(_slice), since_when(_since_when), transactor(_slice->cache(), rwi_read, _since_when),
          callback(_callback), held_blocks(), num_pending_blocks(0), shutdown_mode(false) { }

    // The slice we're backfilling from.
    btree_slice_t *const slice;
    // The time from which we're backfilling.
    repli_timestamp const since_when;
    // The transaction we're using.
    transactor_t const transactor;
    // The callback which receives key/value pairs.
    backfill_callback_t *const callback;

    // Should we stop backfilling immediately?
    bool shutdown_mode;

    // Blocks we currently hold, organized by level.
    std::vector< std::vector<buf_t *> > held_blocks;
    // The number of blocks we are currently loading.
    int num_pending_blocks;

private:
    DISABLE_COPYING(backfill_state_t);
};

int num_live(const backfill_state_t& state) {
    // 8 petabytes of RAM should be enough for backfilling
    int ret = 0;
    for (int i = 0, n = state.held_blocks.size(); i < n; ++i) {
        ret += state.held_blocks[i].size();
    }
    return ret + state.num_pending_blocks;
}

void subtrees_backfill(backfill_state_t& state, buf_lock_t& parent, int level, block_id_t *block_ids, int num_block_ids);
void spawn_subtree_backfill(backfill_state_t& state, int level, block_id_t block_id, cond_t *acquisition_cond);

void spawn_btree_backfill(btree_slice_t *slice, repli_timestamp since_when, backfill_callback_t *callback) {
    backfill_state_t state(slice, since_when, callback);

    buf_lock_t buf_lock(state.transactor->transaction(), SUPERBLOCK_ID, rwi_read);
    block_id_t root_id = ptr_cast<btree_superblock_t>(buf_lock.buf()->get_data_read())->root_block;
    rassert(root_id != SUPERBLOCK_ID);

    if (root_id == NULL_BLOCK_ID) {
        // No root, so no keys in this entire shard.
        return;
    }

    subtrees_backfill(state, buf_lock, 0, &root_id, 1);
}

void subtrees_backfill(backfill_state_t& state, buf_lock_t& parent, int level, block_id_t *block_ids, int num_block_ids) {
    scoped_array<repli_timestamp> recencies(new repli_timestamp[num_block_ids]);
    get_recency_timestamps(state, block_ids, num_block_ids, recencies.get());

    // Conds activated when we first try to acquire the children.
    // TODO: Replace acquisition_conds with a counter that counts down to zero.
    scoped_array<cond_t> acquisition_conds(new cond_t[num_block_ids]);
    for (int i = 0; i < num_block_ids; ++i) {
        if (recencies[i].time >= state.since_when.time) {
            spawn_subtree_backfill(state, level, block_ids[i], &acquisition_conds[i]);
        } else {
            acquisition_conds[i].pulse();
        }
    }

    for (int i = 0; i < num_block_ids; ++i) {
        acquisition_conds[i].wait();
    }

    // The children are all pending acquisition; we can release the parent.
    parent.release();
}

void spawn_subtree_backfill(backfill_state_t& state, int level, block_id_t block_id, cond_t *acquisition_cond) {
    buf_lock_t buf_lock;
    acquire_node(buf_lock, state, block_id, acquisition_cond);

    


}
