#ifndef HEADER_fd_src_disco_shred_fd_shred_h
#define HEADER_fd_src_disco_shred_fd_shred_h

#include "../fd_disco_base.h"
#include "../../tango/ip/fd_ip.h"

/* There's nothing deep about this max, but I just find it easier to
   have a max and use statically sized arrays than alloca. */
#define MAX_BANK_CNT 64UL

/* MAX_SHRED_DESTS indicates the maximum number of destinations (i.e. a
   pubkey -> ip, port) that the shred tile can keep track of. */
#define MAX_SHRED_DESTS 50000UL

#define FD_SHRED_TILE_SCRATCH_ALIGN 128UL

struct fd_shred_tile_args {
  ulong fec_resolver_depth;
  ulong fec_resolver_done_depth;
  ulong bank_cnt;

  uchar const * src_mac;
  uint src_ip;

  ushort shred_version;

  fd_cnc_t * cnc;
  ulong      pid;

  fd_rng_t * rng;
  long lazy;
  ulong cr_max;

  fd_ip_t * ip;

  fd_mvcc_t * cluster_nodes_mvcc;

  uchar const * shred_signing_key;

  union {
    struct {
      fd_frag_meta_t const * from_net_mcache;
      fd_frag_meta_t const * bank_shred_mcache[ MAX_BANK_CNT ];
    };
    fd_frag_meta_t const * all_in_mcaches[ MAX_BANK_CNT+1 ];
  };
  union {
    struct {
      ulong          * from_net_fseq;
      ulong          * bank_shred_fseq[ MAX_BANK_CNT ];
    };
    ulong * all_in_fseqs[ MAX_BANK_CNT+1 ];
  };

  uchar          * bank_shred_dcache[ MAX_BANK_CNT ];

  fd_frag_meta_t * to_net_mcache;
  uchar          * to_net_dcache;
  ulong          * to_net_fseq;

  fd_frag_meta_t * shred_store_mcache;
  uchar          * shred_store_dcache;
  ulong          * shred_store_fseq;
};
typedef struct fd_shred_tile_args fd_shred_tile_args_t;

int
fd_shred_tile( fd_shred_tile_args_t * args,
               void                 * scratch );

ulong
fd_shred_tile_scratch_align( void );

ulong
fd_shred_tile_scratch_footprint( ulong bank_cnt,
                                 ulong shred_store_mcache_depth,
                                 ulong fec_resolver_depth,
                                 ulong fec_resolver_done_depth );

#endif /* HEADER_fd_src_disco_shred_fd_shred_h */


