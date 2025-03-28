#include "fd_funk.h"

/* Provide the actual transaction map implementation */

#define MAP_NAME              fd_funk_txn_map
#define MAP_T                 fd_funk_txn_t
#define MAP_KEY_T             fd_funk_txn_xid_t
#define MAP_KEY               xid
#define MAP_KEY_EQ(k0,k1)     fd_funk_txn_xid_eq((k0),(k1))
#define MAP_KEY_HASH(k0,seed) fd_funk_txn_xid_hash((k0),(seed))
#define MAP_KEY_COPY(kd,ks)   fd_funk_txn_xid_copy((kd),(ks))
#define MAP_NEXT              map_next
#define MAP_HASH              map_hash
#define MAP_MAGIC             (0xf173da2ce7172db0UL) /* Firedancer trn db version 0 */
#define MAP_IMPL_STYLE        2
#define MAP_MEMOIZE           1
#include "../util/tmpl/fd_map_giant.c"

/* As described in fd_funk_txn.h, like the extensive tests in verify
   (which, by its very nature, is called on maps whose integrity is
   unclear to the caller), these have been fortified again memory
   corruption (accidental or deliberate) by doing out of bounds memory
   access detection and cycle (infinite loop) detection in all the
   various operations below.

   This is debatably overkill but, given this code is the pointy end of
   the stick for keeping transaction histories clean (i.e. cancelling a
   wrong transaction could lose information and publishing a wrong
   transaction is even worse), the overhead for the detection is
   minimal, and these operations aren't particularly performance
   critical anyway, seems a more-than-worthwhile safeguard.  Likewise,
   it also guarantees strict algo overheads of these in the face of
   corruption.

   When there is a corruption issue detected realtime, it is prima facie
   evidence of either software bug, hardware fault or compromised
   process.  In all cases, these functions will refuse to proceed
   further and abort with FD_LOG_CRIT to minimize the blast radius of
   possible corruption and give the user as much details (stack trace
   and core) to diagnose the source of the issue.  This handling is
   mostly isolated to the below macros and thus easy to modify or
   disable.

   The corruption detection works by ensuring all map indices are in
   bounds and then applying a unique tag during various map traversals
   such that operations can detected if a transaction has already been
   encountered earlier in the traversal (and thus will create a
   cycle/infinite loop) in the shortest possible algorithm overhead to
   detect such a cycle. */

#define ASSERT_IN_MAP( txn_idx ) do {                          \
    if( FD_UNLIKELY( txn_idx>=txn_max ) )                      \
      FD_LOG_CRIT(( "memory corruption detected (bad_idx)" )); \
  } while(0)

#define ASSERT_IN_PREP( txn_idx ) do {                                                           \
    if( FD_UNLIKELY( txn_idx>=txn_max ) )                                                        \
      FD_LOG_CRIT(( "memory corruption detected (bad_idx)" ));                                   \
    if( FD_UNLIKELY( !fd_funk_txn_map_query( map, fd_funk_txn_xid( &map[ txn_idx ] ), NULL ) ) ) \
      FD_LOG_CRIT(( "memory corruption detected (not in prep)" ));                               \
  } while(0)

#define ASSERT_UNTAGGED( txn_idx ) do {                      \
    if( FD_UNLIKELY( map[ txn_idx ].tag==tag ) )             \
      FD_LOG_CRIT(( "memory corruption detected (cycle)" )); \
  } while(0)

fd_funk_txn_t *
fd_funk_txn_prepare( fd_funk_t *               funk,
                     fd_funk_txn_t *           parent,
                     fd_funk_txn_xid_t const * xid,
                     int                       verbose ) {

  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return NULL;
  }
  fd_funk_check_write( funk );

  fd_funk_txn_t * map = fd_funk_txn_map( funk, fd_funk_wksp( funk ) );

  if( FD_UNLIKELY( fd_funk_txn_map_is_full( map ) ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "too many transactions in preparation" ));
    return NULL;
  }

  ulong txn_max = funk->txn_max;

  ulong  parent_idx;
  uint * _child_head_cidx;
  uint * _child_tail_cidx;

  if( FD_LIKELY( !parent ) ) { /* opt for incr pub */

    parent_idx = FD_FUNK_TXN_IDX_NULL;

    _child_head_cidx = &funk->child_head_cidx;
    _child_tail_cidx = &funk->child_tail_cidx;

  } else {

    parent_idx = (ulong)(parent - map);

    if( FD_UNLIKELY( (parent_idx>=txn_max) /* Out of map */ | (parent!=(map+parent_idx)) /* Bad alignment */ ) ) {
      if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "parent is not a funk transaction" ));
      return NULL;
    }

    if( FD_UNLIKELY( !fd_funk_txn_map_query( map, fd_funk_txn_xid( parent ), NULL ) ) ) {
      if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "parent is not in preparation" ));
      return NULL;
    }

    _child_head_cidx = &parent->child_head_cidx;
    _child_tail_cidx = &parent->child_tail_cidx;

  }

  if( FD_UNLIKELY( !xid ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL xid" ));
    return NULL;
  }

  if( FD_UNLIKELY( fd_funk_txn_xid_eq_root( xid ) ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "xid is the root" ));
    return NULL;
  }

  if( FD_UNLIKELY( fd_funk_txn_xid_eq( xid, funk->last_publish ) ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "xid is the last published" ));
    return NULL;
  }

  if( FD_UNLIKELY( fd_funk_txn_map_query( map, xid, NULL ) ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "id already a transaction" ));
    return NULL;
  }

  /* Get a new transaction from the map */

  fd_funk_txn_t * txn     = fd_funk_txn_map_insert( map, xid );
  ulong           txn_idx = (ulong)(txn - map);
  ASSERT_IN_MAP( txn_idx );

  /* Join the family */

  ulong sibling_prev_idx = fd_funk_txn_idx( *_child_tail_cidx );

  int first_born = fd_funk_txn_idx_is_null( sibling_prev_idx );
  if( FD_UNLIKELY( !first_born ) ) ASSERT_IN_PREP( sibling_prev_idx ); /* opt for non-compete */

  txn->parent_cidx       = fd_funk_txn_cidx( parent_idx           );
  txn->child_head_cidx   = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );
  txn->child_tail_cidx   = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );
  txn->sibling_prev_cidx = fd_funk_txn_cidx( sibling_prev_idx     );
  txn->sibling_next_cidx = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );
  txn->stack_cidx        = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );
  txn->tag               = 0UL;

  txn->rec_head_idx = FD_FUNK_REC_IDX_NULL;
  txn->rec_tail_idx = FD_FUNK_REC_IDX_NULL;

  /* TODO: consider branchless impl */
  if( FD_LIKELY( first_born ) ) *_child_head_cidx                         = fd_funk_txn_cidx( txn_idx ); /* opt for non-compete */
  else                          map[ sibling_prev_idx ].sibling_next_cidx = fd_funk_txn_cidx( txn_idx );

  *_child_tail_cidx = fd_funk_txn_cidx( txn_idx );

  return txn;
}

/* fd_funk_txn_cancel_childless cancels a transaction that is known
   to be childless.  Callers have already validated our input arguments.
   Assumes that cancelling in the app can't fail but that could be
   straightforward to support by giving this an error and plumbing
   through to abort the overall cancel operation when it hits a error. */

static void
fd_funk_txn_cancel_childless( fd_funk_t *     funk,
                              fd_funk_txn_t * map,
                              ulong           txn_max,
                              ulong           txn_idx ) {

  fd_funk_check_write( funk );

  /* Remove all records used by this transaction.  Note that we don't
     need to bother doing all the individual removal operations as we
     are removing the whole list.  We do reset the record transaction
     idx with NULL though we can detect cycles as soon as possible
     and abort. */

  fd_wksp_t *     wksp    = fd_funk_wksp   ( funk );
  fd_alloc_t *    alloc   = fd_funk_alloc  ( funk, wksp );
  fd_funk_rec_t * rec_map = fd_funk_rec_map( funk, wksp );
  ulong           rec_max = funk->rec_max;

  ulong rec_idx = map[ txn_idx ].rec_head_idx;
  while( !fd_funk_rec_idx_is_null( rec_idx ) ) {

    if( FD_UNLIKELY( rec_idx>=rec_max ) ) FD_LOG_CRIT(( "memory corruption detected (bad idx)" ));
    if( FD_UNLIKELY( fd_funk_txn_idx( rec_map[ rec_idx ].txn_cidx )!=txn_idx ) )
      FD_LOG_CRIT(( "memory corruption detected (cycle or bad idx)" ));

    ulong next_idx = rec_map[ rec_idx ].next_idx;
    rec_map[ rec_idx ].txn_cidx = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );

    fd_funk_val_flush( &rec_map[ rec_idx ], alloc, wksp );
    fd_funk_rec_map_remove( rec_map, fd_funk_rec_pair( &rec_map[ rec_idx ] ) );

    rec_idx = next_idx;
  }

  /* Leave the family */

  ulong sibling_prev_idx = fd_funk_txn_idx( map[ txn_idx ].sibling_prev_cidx );
  ulong sibling_next_idx = fd_funk_txn_idx( map[ txn_idx ].sibling_next_cidx );

  /* TODO: Consider branchless impl */

  if( FD_LIKELY( fd_funk_txn_idx_is_null( sibling_prev_idx ) ) ) { /* opt for non-compete */
    ulong parent_idx = fd_funk_txn_idx( map[ txn_idx ].parent_cidx );
    if( FD_LIKELY( fd_funk_txn_idx_is_null( parent_idx ) ) ) { /* No older sib and is a funk child, opt for incr pub */
      funk->child_head_cidx = fd_funk_txn_cidx( sibling_next_idx );
    } else { /* No older sib and has parent */
      ASSERT_IN_PREP( parent_idx );
      map[ parent_idx ].child_head_cidx = fd_funk_txn_cidx( sibling_next_idx );
    }
  } else { /* Has older sib */
    ASSERT_IN_PREP( sibling_prev_idx );
    map[ sibling_prev_idx ].sibling_next_cidx = fd_funk_txn_cidx( sibling_next_idx );
  }

  if( FD_LIKELY( fd_funk_txn_idx_is_null( sibling_next_idx ) ) ) { /* opt for non-compete */
    ulong parent_idx = fd_funk_txn_idx( map[ txn_idx ].parent_cidx );
    if( FD_LIKELY( fd_funk_txn_idx_is_null( parent_idx ) ) ) { /* No younger sib and is a funk child, opt for incr pub */
      funk->child_tail_cidx = fd_funk_txn_cidx( sibling_prev_idx );
    } else { /* No younger sib and has parent */
      ASSERT_IN_PREP( parent_idx );
      map[ parent_idx ].child_tail_cidx = fd_funk_txn_cidx( sibling_prev_idx );
    }
  } else { /* Has younger sib */
    ASSERT_IN_PREP( sibling_next_idx );
    map[ sibling_next_idx ].sibling_prev_cidx = fd_funk_txn_cidx( sibling_prev_idx );
  }

  fd_funk_txn_map_remove( map, fd_funk_txn_xid( &map[ txn_idx ] ) );
}

/* fd_funk_txn_cancel_family cancels a transaction and all its
   descendants in a tree-depth-first-ordered sense from youngest to
   oldest.  Callers have already validated our input arguments.  Returns
   the number of transactions canceled. */

static ulong
fd_funk_txn_cancel_family( fd_funk_t *     funk,
                           fd_funk_txn_t * map,
                           ulong           txn_max,
                           ulong           tag,
                           ulong           txn_idx ) {
  ulong cancel_cnt = 0UL;

  map[ txn_idx ].tag = tag;

  ulong parent_stack_idx = FD_FUNK_TXN_IDX_NULL;

  for(;;) {

    /* At this point, txn_idx appears to be valid and has been tagged. */

    ulong youngest_idx = fd_funk_txn_idx( map[ txn_idx ].child_tail_cidx );
    if( FD_LIKELY( fd_funk_txn_idx_is_null( youngest_idx ) ) ) { /* txn is is childless, opt for incr pub */

      fd_funk_txn_cancel_childless( funk, map, txn_max, txn_idx ); /* If this can fail, return cancel_cnt here on fail */
      cancel_cnt++;

      txn_idx = parent_stack_idx;                                  /* Pop the parent stack */
      if( FD_LIKELY( fd_funk_txn_idx_is_null( txn_idx ) ) ) break; /* If stack is empty, we are done, opt for incr pub */
      parent_stack_idx = fd_funk_txn_idx( map[ txn_idx ].stack_cidx );
      continue;
    }

    /* txn has at least one child and the youngest is youngest_idx.  Tag
       the youngest child, push txn onto the parent stack and recurse
       into the youngest child. */

    ASSERT_IN_PREP ( youngest_idx );
    ASSERT_UNTAGGED( youngest_idx );
    map[ youngest_idx ].tag = tag;

    map[ txn_idx ].stack_cidx = fd_funk_txn_cidx( parent_stack_idx );
    parent_stack_idx          = txn_idx;

    txn_idx = youngest_idx;
  }

  return cancel_cnt;
}

ulong
fd_funk_txn_cancel( fd_funk_t *     funk,
                    fd_funk_txn_t * txn,
                    int             verbose ) {

  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return 0UL;
  }

  fd_funk_txn_t * map = fd_funk_txn_map( funk, fd_funk_wksp( funk ) );

  ulong txn_max = funk->txn_max;

  ulong txn_idx = (ulong)(txn - map);

  if( FD_UNLIKELY( (txn_idx>=txn_max) /* Out of map (incl NULL) */ | (txn!=(map+txn_idx)) /* Bad alignment */ ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not a funk transaction" ));
    return 0UL;
  }

  if( FD_UNLIKELY( !fd_funk_txn_map_query( map, fd_funk_txn_xid( txn ), NULL ) ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not in preparation" ));
    return 0UL;
  }

  return fd_funk_txn_cancel_family( funk, map, txn_max, funk->cycle_tag++, txn_idx );
}

/* fd_funk_txn_oldest_sibling returns the index of the oldest sibling
   in txn_idx's family.  Callers have already validated our input
   arguments.  The caller should validate the return index. */

static inline ulong
fd_funk_txn_oldest_sibling( fd_funk_t *     funk,
                            fd_funk_txn_t * map,
                            ulong           txn_max,
                            ulong           txn_idx ) {

  ulong parent_idx = fd_funk_txn_idx( map[ txn_idx ].parent_cidx );

  if( FD_LIKELY( fd_funk_txn_idx_is_null( parent_idx ) ) ) return fd_funk_txn_idx( funk->child_head_cidx ); /* opt for incr pub */

  ASSERT_IN_PREP( parent_idx );

  return fd_funk_txn_idx( map[ parent_idx ].child_head_cidx );
}

/* fd_funk_txn_cancel_sibling_list cancels siblings from sibling_idx down
   to the youngest sibling inclusive in the order from youngest to
   sibling_idx.  Callers have already validated our input arguments
   except sibling_idx.  Returns the number of cancelled transactions
   (should be at least 1).  If any sibling is skip_idx, it will be not
   be cancelled but still tagged as visited.  Passing
   FD_FUNK_TXN_IDX_NULL for skip_idx will cancel all siblings from
   sibling_idx to the youngest inclusive. */

static ulong
fd_funk_txn_cancel_sibling_list( fd_funk_t *     funk,
                                 fd_funk_txn_t * map,
                                 ulong           txn_max,
                                 ulong           tag,
                                 ulong           sibling_idx,
                                 ulong           skip_idx ) {

  ulong cancel_stack_idx = FD_FUNK_TXN_IDX_NULL;

  /* Push siblings_idx and its younger siblings inclusive (skipping
     sibling skip_idx if encounter) onto the cancel stack from oldest to
     youngest (such that we cancel youngest to oldest). */

  for(;;) {

    /* At this point, sibling_idx is a sibling we might want to add to
       the sibling stack.  Validate and tag it. */

    ASSERT_IN_PREP ( sibling_idx );
    ASSERT_UNTAGGED( sibling_idx );
    map[ sibling_idx ].tag = tag;

    if( FD_UNLIKELY( sibling_idx!=skip_idx ) ) { /* Not skip_idx so push onto the cancel stack, opt for non-compete */
      map[ sibling_idx ].stack_cidx = fd_funk_txn_cidx( cancel_stack_idx );
      cancel_stack_idx = sibling_idx;
    }

    ulong younger_idx = fd_funk_txn_idx( map[ sibling_idx ].sibling_next_cidx );
    if( FD_LIKELY( fd_funk_txn_idx_is_null( younger_idx ) ) ) break; /* opt for non-compete */
    sibling_idx = younger_idx;

  }

  /* Cancel all transactions and their descendants on the cancel stack */

  ulong cancel_cnt = 0UL;

  while( !fd_funk_txn_idx_is_null( cancel_stack_idx ) ) { /* TODO: peel first iter to make more predictable? */
    ulong sibling_idx = cancel_stack_idx;
    cancel_stack_idx  = fd_funk_txn_idx( map[ sibling_idx ].stack_cidx );

    cancel_cnt += fd_funk_txn_cancel_family( funk, map, txn_max, tag, sibling_idx );
  }

  return cancel_cnt;
}

ulong
fd_funk_txn_cancel_siblings( fd_funk_t *     funk,
                             fd_funk_txn_t * txn,
                             int             verbose ) {

  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return 0UL;
  }

  fd_funk_txn_t * map = fd_funk_txn_map( funk, fd_funk_wksp( funk ) );

  ulong txn_max = funk->txn_max;

  ulong txn_idx = (ulong)(txn - map);

  if( FD_UNLIKELY( (txn_idx>=txn_max) /* Out of map (incl NULL) */ | (txn!=(map+txn_idx)) /* Bad alignment */ ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not a funk transaction" ));
    return 0UL;
  }

  ulong oldest_idx = fd_funk_txn_oldest_sibling( funk, map, txn_max, txn_idx );

  return fd_funk_txn_cancel_sibling_list( funk, map, txn_max, funk->cycle_tag++, oldest_idx, txn_idx );
}

ulong
fd_funk_txn_cancel_children( fd_funk_t *     funk,
                             fd_funk_txn_t * txn,
                             int             verbose ) {

  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return 0UL;
  }

  fd_funk_txn_t * map = fd_funk_txn_map( funk, fd_funk_wksp( funk ) );

  ulong txn_max = funk->txn_max;

  ulong oldest_idx;

  if( FD_LIKELY( txn == NULL ) ) {
    oldest_idx = fd_funk_txn_idx( funk->child_head_cidx ); /* opt for non-compete */
  } else {
    ulong txn_idx = (ulong)(txn - map);
    if( FD_UNLIKELY( (txn_idx>=txn_max) /* Out of map */ | (txn!=(map+txn_idx)) /* Bad alignment */ ) ) {
      if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not a funk transaction" ));
      return 0UL;
    }

    if( FD_UNLIKELY( !fd_funk_txn_map_query( map, fd_funk_txn_xid( txn ), NULL ) ) ) {
      if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not in preparation" ));
      return 0UL;
    }

    oldest_idx = fd_funk_txn_idx( txn->child_head_cidx );
  }

  if( fd_funk_txn_idx_is_null( oldest_idx ) ) {
    return 0UL;
  }

  return fd_funk_txn_cancel_sibling_list( funk, map, txn_max, funk->cycle_tag++, oldest_idx, FD_FUNK_TXN_IDX_NULL );
}

/* Cancel all outstanding transactions */

ulong
fd_funk_txn_cancel_all( fd_funk_t *     funk,
                        int             verbose ) {
  return fd_funk_txn_cancel_children( funk, NULL, verbose );
}

/* fd_funk_txn_update applies the record updates in transaction txn_idx
   to another transaction or the parent transaction.  Callers have
   already validated our input arguments.

   On entry, the head/tail of the destination records are at
   *_dst_rec_head_idx / *_dst_rec_tail_idx.  All transactions on this
   list will have transaction id dst_xid and vice versa.  That is, this
   is the record list the last published transaction or txn_idx's
   in-prep parent transaction.

   On exit, the head/tail of the updated records is at
   *_dst_rec_head_idx / *_dst_rec_tail_idx.  As before, all transactions
   on this list will have transaction id dst_xid and vice versa.
   Transaction txn_idx will have an _empty_ record list.

   Updates in the transaction txn_idx are processed from oldest to
   youngest.  If an update erases an existing record in dest, the record
   to erase is removed from the destination records without perturbing
   the order of remaining destination records.  If an update is to
   update an existing record, the destination record value is updated
   and the order of the destination records is unchanged.  If an update
   is to create a new record, the record is appended to the list of
   existing values as youngest without changing the order of existing
   values.  If an update erases a record in an in-prep parent, the
   erasure will be moved into the parent as the youngest without
   changing the order of existing values. */

static void
fd_funk_txn_update( ulong *                   _dst_rec_head_idx, /* Pointer to the dst list head */
                    ulong *                   _dst_rec_tail_idx, /* Pointer to the dst list tail */
                    ulong                     dst_txn_idx,       /* Transaction index of the merge destination */
                    fd_funk_txn_xid_t const * dst_xid,           /* dst xid */
                    ulong                     txn_idx,           /* Transaction index of the records to merge */
                    ulong                     rec_max,           /* ==funk->rec_max */
                    fd_funk_txn_t *           txn_map,           /* ==fd_funk_rec_map( funk, wksp ) */
                    fd_funk_rec_t *           rec_map,           /* ==fd_funk_rec_map( funk, wksp ) */
                    fd_alloc_t *              alloc,             /* ==fd_funk_alloc( funk, wksp ) */
                    fd_wksp_t *               wksp ) {           /* ==fd_funk_wksp( funk ) */
  /* We don't need to do all the individual removal pointer updates
     as we are removing the whole list from txn_idx.  */

  ulong rec_idx = txn_map[ txn_idx ].rec_head_idx;
  while( !fd_funk_rec_idx_is_null( rec_idx ) ) {
    /* Validate rec_idx */

    if( FD_UNLIKELY( rec_idx>=rec_max ) ) FD_LOG_CRIT(( "memory corruption detected (bad idx)" ));
    if( FD_UNLIKELY( fd_funk_txn_idx( rec_map[ rec_idx ].txn_cidx )!=txn_idx ) )
      FD_LOG_CRIT(( "memory corruption detected (cycle or bad idx)" ));

    fd_funk_rec_t * rec = &rec_map[ rec_idx ];
    ulong next_rec_idx = rec->next_idx;

    /* See if (dst_xid,key) already exists. Remove it if it does, and then clean up the corpse.
       We can take advantage of the ordering property that children
       come before parents in the hash chain, and all elements with
       the same record key have the same hash. */
    ulong * next = &rec->map_next;
    for(;;) {
      ulong ele_idx = fd_funk_rec_map_private_unbox_idx( *next );
      if( fd_funk_rec_map_private_is_null( ele_idx ) ) break;
      fd_funk_rec_t * ele = rec_map + ele_idx;

      if( FD_LIKELY( rec->map_hash == ele->map_hash ) &&
          FD_LIKELY( fd_funk_rec_key_eq( rec->pair.key, ele->pair.key ) ) &&
          FD_LIKELY( fd_funk_txn_xid_eq( dst_xid, ele->pair.xid ) ) ) {
        /* Remove from the transaction */
        ulong prev_idx = ele->prev_idx;
        ulong next_idx = ele->next_idx;
        if( fd_funk_rec_idx_is_null( prev_idx ) ) {
          *_dst_rec_head_idx = next_idx;
        } else {
          rec_map[ prev_idx ].next_idx = next_idx;
        }
        if( fd_funk_rec_idx_is_null( next_idx ) ) {
          *_dst_rec_tail_idx = prev_idx;
        } else {
          rec_map[ next_idx ].prev_idx = prev_idx;
        }
        /* Clean up value */
        fd_funk_val_flush( ele, alloc, wksp );
        ele->txn_cidx = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );
        /* Remove from record map */
        fd_funk_rec_map_private_t * priv = fd_funk_rec_map_private( rec_map );
        *next = ele->map_next;
        ele->map_next = priv->free_stack;
        priv->free_stack = (ele_idx | (1UL<<63));
        priv->key_cnt--;
        break;
      }

      next = &ele->map_next;
    }

    /* Add the new record to the transaction. We can update the xid in
       place because it is not used for hashing the element. We have
       to preserve the original element to preserve the
       newest-to-oldest ordering in the hash
       chain. fd_funk_rec_query_global relies on this subtle
       property. */

    rec->pair.xid[0] = *dst_xid;
    rec->txn_cidx = fd_funk_txn_cidx( dst_txn_idx );

    if( fd_funk_rec_idx_is_null( *_dst_rec_head_idx ) ) {
      *_dst_rec_head_idx = rec_idx;
      rec->prev_idx = FD_FUNK_REC_IDX_NULL;
    } else {
      rec_map[ *_dst_rec_tail_idx ].next_idx = rec_idx;
      rec->prev_idx = *_dst_rec_tail_idx;
    }
    *_dst_rec_tail_idx = rec_idx;
    rec->next_idx = FD_FUNK_REC_IDX_NULL;

    rec_idx = next_rec_idx;
  }

  txn_map[ txn_idx ].rec_head_idx = FD_FUNK_REC_IDX_NULL;
  txn_map[ txn_idx ].rec_tail_idx = FD_FUNK_REC_IDX_NULL;
}

/* fd_funk_txn_publish_funk_child publishes a transaction that is known
   to be a child of funk.  Callers have already validated our input
   arguments.  Returns FD_FUNK_SUCCESS on success and an FD_FUNK_ERR_*
   code on failure.  (There are currently no failure cases but the
   plumbing is there if value handling requires it at some point.) */

static int
fd_funk_txn_publish_funk_child( fd_funk_t *     funk,
                                fd_funk_txn_t * map,
                                ulong           txn_max,
                                ulong           tag,
                                ulong           txn_idx ) {

  fd_funk_check_write( funk );

  /* Apply the updates in txn to the last published transactions */

  fd_wksp_t * wksp = fd_funk_wksp( funk );
  fd_funk_txn_update( &funk->rec_head_idx, &funk->rec_tail_idx, FD_FUNK_TXN_IDX_NULL, fd_funk_root( funk ),
                      txn_idx, funk->rec_max, map, fd_funk_rec_map( funk, wksp ),
                      fd_funk_alloc( funk, wksp ), wksp );

  /* Cancel all competing transaction histories */

  ulong oldest_idx = fd_funk_txn_oldest_sibling( funk, map, txn_max, txn_idx );
  fd_funk_txn_cancel_sibling_list( funk, map, txn_max, tag, oldest_idx, txn_idx );

  /* Make all the children children of funk */

  ulong child_head_idx = fd_funk_txn_idx( map[ txn_idx ].child_head_cidx );
  ulong child_tail_idx = fd_funk_txn_idx( map[ txn_idx ].child_tail_cidx );

  ulong child_idx = child_head_idx;
  while( FD_UNLIKELY( !fd_funk_txn_idx_is_null( child_idx ) ) ) { /* opt for incr pub */

    ASSERT_IN_PREP ( child_idx );
    ASSERT_UNTAGGED( child_idx );
    map[ child_idx ].tag = tag;

    map[ child_idx ].parent_cidx = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );

    child_idx = fd_funk_txn_idx( map[ child_idx ].sibling_next_cidx );
  }

  funk->child_head_cidx = fd_funk_txn_cidx( child_head_idx );
  funk->child_tail_cidx = fd_funk_txn_cidx( child_tail_idx );

  /* Remove the mapping */

  fd_funk_txn_xid_copy( funk->last_publish, fd_funk_txn_xid( &map[ txn_idx ] ) );

  fd_funk_txn_map_remove( map, fd_funk_txn_xid( &map[ txn_idx ] ) );

  return FD_FUNK_SUCCESS;
}

ulong
fd_funk_txn_publish( fd_funk_t *     funk,
                     fd_funk_txn_t * txn,
                     int             verbose ) {

  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return 0UL;
  }

  fd_wksp_t * wksp = fd_funk_wksp( funk );

  fd_funk_txn_t * map = fd_funk_txn_map( funk, wksp );

  ulong txn_max = funk->txn_max;

  ulong txn_idx = (ulong)(txn - map);

  if( FD_UNLIKELY( (txn_idx>=txn_max) /* Out of map (incl NULL) */ | (txn!=(map+txn_idx)) /* Bad alignment */ ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not a funk transaction" ));
    return 0UL;
  }

  if( FD_UNLIKELY( !fd_funk_txn_map_query( map, fd_funk_txn_xid( txn ), NULL ) ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "txn is not in preparation" ));
    return 0UL;
  }

  ulong tag = funk->cycle_tag++;

  ulong publish_stack_idx = FD_FUNK_TXN_IDX_NULL;

  for(;;) {
    map[ txn_idx ].tag = tag;

    /* At this point, txn_idx is a transaction that needs to be
       published and has been tagged.  If txn is a child of funk, we are
       ready to publish txn and everything on the publish stack. */

    ulong parent_idx = fd_funk_txn_idx( map[ txn_idx ].parent_cidx );
    if( FD_LIKELY( fd_funk_txn_idx_is_null( parent_idx ) ) ) break; /* opt for incr pub */

    /* txn_idx has a parent.  Validate and tag it.  Push txn to the
       publish stack and recurse into the parent. */

    ASSERT_IN_PREP ( parent_idx );
    ASSERT_UNTAGGED( parent_idx );

    map[ txn_idx ].stack_cidx = fd_funk_txn_cidx( publish_stack_idx );
    publish_stack_idx         = txn_idx;

    txn_idx = parent_idx;
  }

  ulong publish_cnt = 0UL;

  for(;;) {

    /* At this point, all the transactions we need to publish are
       tagged, txn is the next up publish funk and publish_stack has the
       transactions to follow in order by pop.  We use a new tag for
       each publish as txn and its siblings we potentially visited in a
       previous iteration of this loop. */

    if( FD_UNLIKELY( fd_funk_txn_publish_funk_child( funk, map, txn_max, funk->cycle_tag++, txn_idx ) ) ) break;
    publish_cnt++;

    txn_idx = publish_stack_idx;
    if( FD_LIKELY( fd_funk_txn_idx_is_null( txn_idx ) ) ) break; /* opt for incr pub */
    publish_stack_idx = fd_funk_txn_idx( map[ txn_idx ].stack_cidx );
  }

  return publish_cnt;
}

int
fd_funk_txn_publish_into_parent( fd_funk_t *     funk,
                                 fd_funk_txn_t * txn,
                                 int             verbose ) {
  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return FD_FUNK_ERR_INVAL;
  }
  fd_funk_check_write( funk );

  fd_wksp_t * wksp = fd_funk_wksp( funk );

  fd_funk_txn_t * map = fd_funk_txn_map( funk, wksp );

  ulong txn_idx = (ulong)(txn - map);

  ulong oldest_idx = fd_funk_txn_oldest_sibling( funk, map, funk->txn_max, txn_idx );
  fd_funk_txn_cancel_sibling_list( funk, map, funk->txn_max, funk->cycle_tag++, oldest_idx, txn_idx );

  ulong parent_idx = fd_funk_txn_idx( txn->parent_cidx );
  if( fd_funk_txn_idx_is_null( parent_idx ) ) {
    /* Publish to root */
    if( fd_funk_txn_idx( funk->child_head_cidx ) != txn_idx || fd_funk_txn_idx( funk->child_tail_cidx ) != txn_idx )
      FD_LOG_CRIT(( "memory corruption detected (cycle or bad idx)" ));
    fd_funk_txn_update( &funk->rec_head_idx, &funk->rec_tail_idx, FD_FUNK_TXN_IDX_NULL, fd_funk_root( funk ),
                        txn_idx, funk->rec_max, map, fd_funk_rec_map( funk, wksp ),
                        fd_funk_alloc( funk, wksp ), wksp );
    /* Inherit the children */
    funk->child_head_cidx = txn->child_head_cidx;
    funk->child_tail_cidx = txn->child_tail_cidx;
  } else {
    fd_funk_txn_t * parent_txn = map + parent_idx;
    if( fd_funk_txn_idx( parent_txn->child_head_cidx ) != txn_idx || fd_funk_txn_idx( parent_txn->child_tail_cidx ) != txn_idx )
      FD_LOG_CRIT(( "memory corruption detected (cycle or bad idx)" ));
    fd_funk_txn_update( &parent_txn->rec_head_idx, &parent_txn->rec_tail_idx, parent_idx, &parent_txn->xid,
                        txn_idx, funk->rec_max, map, fd_funk_rec_map( funk, wksp ),
                        fd_funk_alloc( funk, wksp ), wksp );
    /* Inherit the children */
    parent_txn->child_head_cidx = txn->child_head_cidx;
    parent_txn->child_tail_cidx = txn->child_tail_cidx;
  }

  /* Adjust the parent pointers of the children to point to their grandparent */
  ulong child_idx = fd_funk_txn_idx( txn->child_head_cidx );
  while( FD_UNLIKELY( !fd_funk_txn_idx_is_null( child_idx ) ) ) {
    map[ child_idx ].parent_cidx = fd_funk_txn_cidx( parent_idx );
    child_idx = fd_funk_txn_idx( map[ child_idx ].sibling_next_cidx );
  }

  fd_funk_txn_map_remove( map, fd_funk_txn_xid( txn ) );

  return FD_FUNK_SUCCESS;
}

/* Unfortunately, the semantics of how to deal with the same record
   key appearing in multiple children is not defined. So, for now,
   this API is commented out. */
#if 0
int
fd_funk_txn_merge_all_children( fd_funk_t *     funk,
                                fd_funk_txn_t * parent_txn,
                                int             verbose ) {
  if( FD_UNLIKELY( !funk ) ) {
    if( FD_UNLIKELY( verbose ) ) FD_LOG_WARNING(( "NULL funk" ));
    return FD_FUNK_ERR_INVAL;
  }
  fd_funk_check_write( funk );

  fd_wksp_t * wksp = fd_funk_wksp( funk );

  fd_funk_txn_t * map = fd_funk_txn_map( funk, wksp );
  ulong           txn_max = funk->txn_max;                 /* Previously verified */

  ulong parent_idx;
  fd_funk_txn_xid_t * parent_xid;
  uint * child_head_cidx;
  uint * child_tail_cidx;
  ulong * rec_head_idx;
  ulong * rec_tail_idx;
  if( parent_txn == NULL ) { /* Root */
    parent_idx = FD_FUNK_TXN_IDX_NULL;
    parent_xid = funk->root;
    child_head_cidx = &funk->child_head_cidx;
    child_tail_cidx = &funk->child_tail_cidx;
    rec_head_idx = &funk->rec_head_idx;
    rec_tail_idx = &funk->rec_tail_idx;
  } else {
    parent_idx = (ulong)(parent_txn - map);
    ASSERT_IN_PREP( parent_idx );
    parent_xid = &parent_txn->xid;
    child_head_cidx = &parent_txn->child_head_cidx;
    child_tail_cidx = &parent_txn->child_tail_cidx;
    rec_head_idx = &parent_txn->rec_head_idx;
    rec_tail_idx = &parent_txn->rec_tail_idx;
  }

  ulong child_head_idx = fd_funk_txn_idx( *child_head_cidx );
  ulong child_idx = child_head_idx;
  while( FD_UNLIKELY( !fd_funk_txn_idx_is_null( child_idx ) ) ) { /* opt for incr pub */
    /* Merge records from child into parent */

    fd_funk_txn_t * txn = &map[ child_idx ];
    if( FD_UNLIKELY( !fd_funk_txn_idx_is_null( fd_funk_txn_idx( txn->child_head_cidx ) ) ) ) {
      FD_LOG_WARNING(("cannot call fd_funk_txn_merge_all_children if parent_txn has grandchildren"));
      return FD_FUNK_ERR_TXN;
    }

    fd_funk_txn_update( rec_head_idx, rec_tail_idx, parent_idx, parent_xid,
                        child_idx, funk->rec_max, map, fd_funk_rec_map( funk, wksp ),
                        fd_funk_alloc( funk, wksp ), wksp );

    child_idx = fd_funk_txn_idx( txn->sibling_next_cidx );
    fd_funk_txn_map_remove( map, fd_funk_txn_xid( txn ) );

    /* Update the pointers as we go in case we stop in the middle
     */
    *child_head_cidx = fd_funk_txn_cidx( child_idx );
    if( FD_UNLIKELY( !fd_funk_txn_idx_is_null( child_idx ) ) ) {
      map[ child_idx ].sibling_prev_cidx = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );
    }
  }

  *child_tail_cidx = fd_funk_txn_cidx( FD_FUNK_TXN_IDX_NULL );

  return FD_FUNK_SUCCESS;
}
#endif

/* Return the first record in a transaction. Returns NULL if the
   transaction has no records yet. */

FD_FN_PURE fd_funk_rec_t const *
fd_funk_txn_first_rec( fd_funk_t *           funk,
                       fd_funk_txn_t const * txn ) {
  ulong rec_idx;
  if( FD_UNLIKELY( NULL == txn ))
    rec_idx = funk->rec_head_idx;
  else
    rec_idx = txn->rec_head_idx;
  if( fd_funk_rec_idx_is_null( rec_idx ) ) return NULL;
  fd_funk_rec_t const * rec_map = fd_funk_rec_map( funk, fd_funk_wksp( funk ) );
  return rec_map + rec_idx;
}

/* Return the next record in a transaction. Returns NULL if the
   transaction has no more records. */

FD_FN_PURE fd_funk_rec_t const *
fd_funk_txn_next_rec( fd_funk_t *           funk,
                      fd_funk_rec_t const * rec ) {
  ulong rec_idx = rec->next_idx;
  if( fd_funk_rec_idx_is_null( rec_idx ) ) return NULL;
  fd_funk_rec_t const * rec_map = fd_funk_rec_map( funk, fd_funk_wksp( funk ) );
  return rec_map + rec_idx;
}

fd_funk_txn_xid_t
fd_funk_generate_xid(void) {
  fd_funk_txn_xid_t xid;
  static FD_TL ulong seq = 0;
  xid.ul[0] =
    (fd_log_cpu_id() + 1U)*3138831853UL +
    (fd_log_thread_id() + 1U)*9180195821UL +
    (++seq)*6208101967UL;
  xid.ul[1] = ((ulong)fd_tickcount())*2810745731UL;
  return xid;
}

int
fd_funk_txn_verify( fd_funk_t * funk ) {
  fd_wksp_t *     wksp    = fd_funk_wksp( funk );          /* Previously verified */
  fd_funk_txn_t * map     = fd_funk_txn_map( funk, wksp ); /* Previously verified */
  ulong           txn_max = funk->txn_max;                 /* Previously verified */

  ulong funk_child_head_idx = fd_funk_txn_idx( funk->child_head_cidx ); /* Previously verified */
  ulong funk_child_tail_idx = fd_funk_txn_idx( funk->child_tail_cidx ); /* Previously verified */

  fd_funk_txn_xid_t const * last_publish = funk->last_publish; /* Previously verified */

# define TEST(c) do {                                                                           \
    if( FD_UNLIKELY( !(c) ) ) { FD_LOG_WARNING(( "FAIL: %s", #c )); return FD_FUNK_ERR_INVAL; } \
  } while(0)

# define IS_VALID( idx ) ( (idx==FD_FUNK_TXN_IDX_NULL) || \
                           ((idx<txn_max) && (!fd_funk_txn_xid_eq( fd_funk_txn_xid( &map[idx] ), last_publish ))) )

  TEST( !fd_funk_txn_map_verify( map ) );

  ulong free_cnt = txn_max - fd_funk_txn_map_key_cnt( map );

  /* Tag all transactions as not visited yet */

  for( ulong txn_idx=0UL; txn_idx<txn_max; txn_idx++ ) map[ txn_idx ].tag = 0UL;

  /* Visit all transactions in preparation, traversing from oldest to
     youngest. */

  ulong prep_cnt = 0UL;
  do {

    /* Push all children of funk to the stack */

    ulong stack_idx = FD_FUNK_TXN_IDX_NULL;
    ulong child_idx = funk_child_head_idx;
    while( !fd_funk_txn_idx_is_null( child_idx ) ) {

      /* Make sure valid idx, not tagged (detects cycles) and child
         knows it is a child of funk.  Then tag as visited / in-prep,
         push to stack and update prep_cnt */

      TEST( IS_VALID( child_idx ) );
      TEST( !map[ child_idx ].tag );
      TEST( fd_funk_txn_idx_is_null( fd_funk_txn_idx( map[ child_idx ].parent_cidx ) ) );
      map[ child_idx ].tag        = 1UL;
      map[ child_idx ].stack_cidx = fd_funk_txn_cidx( stack_idx );
      stack_idx                   = child_idx;
      prep_cnt++;

      ulong next_idx = fd_funk_txn_idx( map[ child_idx ].sibling_next_cidx );
      if( !fd_funk_txn_idx_is_null( next_idx ) ) TEST( fd_funk_txn_idx( map[ next_idx ].sibling_prev_cidx )==child_idx );
      child_idx = next_idx;
    }

    while( !fd_funk_txn_idx_is_null( stack_idx ) ) {

      /* Pop the next transaction to traverse */

      ulong txn_idx = stack_idx;
      stack_idx = fd_funk_txn_idx( map[ txn_idx ].stack_cidx );

      /* Push all children of txn to the stack */

      ulong child_idx = fd_funk_txn_idx( map[ txn_idx ].child_head_cidx );
      while( !fd_funk_txn_idx_is_null( child_idx ) ) {

        /* Make sure valid idx, not tagged (detects cycles) and child
           knows it is a child of txn_idx.  Then tag as visited /
           in-prep, push to stack and update prep_cnt */

        TEST( IS_VALID( child_idx ) );
        TEST( !map[ child_idx ].tag );
        TEST( fd_funk_txn_idx( map[ child_idx ].parent_cidx )==txn_idx );
        map[ child_idx ].tag        = 1UL;
        map[ child_idx ].stack_cidx = fd_funk_txn_cidx( stack_idx );
        stack_idx                   = child_idx;
        prep_cnt++;

        ulong next_idx = fd_funk_txn_idx( map[ child_idx ].sibling_next_cidx );
        if( !fd_funk_txn_idx_is_null( next_idx ) ) TEST( fd_funk_txn_idx( map[ next_idx ].sibling_prev_cidx )==child_idx );
        child_idx = next_idx;
      }
    }

  } while(0);

  TEST( (free_cnt+prep_cnt)==txn_max );

  /* Do it again with a youngest to oldest traversal to test reverse
     link integrity */

  prep_cnt = 0UL;
  do {

    /* Push all children of funk to the stack */

    ulong stack_idx = FD_FUNK_TXN_IDX_NULL;
    ulong child_idx = funk_child_tail_idx;
    while( !fd_funk_txn_idx_is_null( child_idx ) ) {

      /* Make sure valid idx, tagged as visited above (detects cycles)
         and child knows it is a child of funk.  Then tag as visited /
         in-prep, push to stack and update prep_cnt */

      TEST( IS_VALID( child_idx ) );
      TEST( map[ child_idx ].tag==1UL );
      TEST( fd_funk_txn_idx_is_null( fd_funk_txn_idx( map[ child_idx ].parent_cidx ) ) );
      map[ child_idx ].tag        = 2UL;
      map[ child_idx ].stack_cidx = fd_funk_txn_cidx( stack_idx );
      stack_idx                   = child_idx;
      prep_cnt++;

      ulong prev_idx = fd_funk_txn_idx( map[ child_idx ].sibling_prev_cidx );
      if( !fd_funk_txn_idx_is_null( prev_idx ) ) TEST( fd_funk_txn_idx( map[ prev_idx ].sibling_next_cidx )==child_idx );
      child_idx = prev_idx;
    }

    while( !fd_funk_txn_idx_is_null( stack_idx ) ) {

      /* Pop the next transaction to traverse */

      ulong txn_idx = stack_idx;
      stack_idx = fd_funk_txn_idx( map[ txn_idx ].stack_cidx );

      /* Push all children of txn to the stack */

      ulong child_idx = fd_funk_txn_idx( map[ txn_idx ].child_tail_cidx );
      while( !fd_funk_txn_idx_is_null( child_idx ) ) {

        /* Make sure valid idx, tagged as visited above (detects cycles)
           and child knows it is a child of txn_idx.  Then, tag as
           visited / in-prep, push to stack and update prep_cnt */

        TEST( IS_VALID( child_idx ) );
        TEST( map[ child_idx ].tag==1UL );
        TEST( fd_funk_txn_idx( map[ child_idx ].parent_cidx )==txn_idx );
        map[ child_idx ].tag        = 2UL;
        map[ child_idx ].stack_cidx = fd_funk_txn_cidx( stack_idx );
        stack_idx                   = child_idx;
        prep_cnt++;

        ulong prev_idx = fd_funk_txn_idx( map[ child_idx ].sibling_prev_cidx );
        if( !fd_funk_txn_idx_is_null( prev_idx ) ) TEST( fd_funk_txn_idx( map[ prev_idx ].sibling_next_cidx )==child_idx );
        child_idx = prev_idx;
      }
    }
  } while(0);

  TEST( (free_cnt+prep_cnt)==txn_max );

  TEST( fd_funk_txn_cnt( map )==prep_cnt );

# undef IS_VALID
# undef TEST

  return FD_FUNK_SUCCESS;
}

#undef ASSERT_UNTAGGED
#undef ASSERT_IN_PREP
#undef ASSERT_IN_MAP
