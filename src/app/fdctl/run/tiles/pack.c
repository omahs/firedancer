#include "tiles.h"
#include "../../fdctl.h"
#include "../run.h"

#include "../../../../disco/fd_disco.h"

#include <linux/unistd.h>

#define FD_PACK_TAG (0x17ac1C711eUL)

static void
init( fd_tile_args_t * args ) {
  (void)args;

  /* calling fd_tempo_tick_per_ns requires nanosleep, it is cached with
     a FD_ONCE */
  fd_tempo_tick_per_ns( NULL );

  /* Read just the public key (last 32 bytes) */
  const uchar * tile_pod = args->wksp_pod[ 0 ];

  /* This seems like overkill for just the public key, but it's not
     really easy to load just the public key without also getting the
     private key. */
  char const * key_path = fd_pod_query_cstr( tile_pod, "identity_key_path", NULL );
  if( FD_UNLIKELY( !key_path ) ) FD_LOG_ERR(( "identity_key_path not found in pod" ));

  uchar const * pubkey = load_key_into_protected_memory( key_path, 1 /* public_key_only */ );
  args->lo_xsk = (void *)pubkey; /* FIXME: hack */
}

static void
run( fd_tile_args_t * args ) {
  const uchar * tile_pod = args->wksp_pod[ 0 ];
  const uchar * in_pod   = args->wksp_pod[ 1 ];
  const uchar * out_pod  = args->wksp_pod[ 2 ];

  ulong out_cnt = fd_pod_query_ulong( out_pod, "cnt", 0UL );
  if( FD_UNLIKELY( !out_cnt ) ) FD_LOG_ERR(( "num_tiles unset or set to zero" ));

  ulong ** out_fseq = (ulong **)fd_alloca( alignof(ulong *), sizeof(ulong *)*out_cnt );
  ulong ** out_busy = (ulong **)fd_alloca( alignof(ulong *), sizeof(ulong *)*out_cnt );
  if( FD_UNLIKELY( !out_fseq || !out_busy ) ) FD_LOG_ERR(( "fd_alloca failed" ));

  for( ulong i=0; i<out_cnt; i++ ) {
    out_fseq[i] = fd_fseq_join( fd_wksp_pod_map1( out_pod, "fseq%lu", i ) );
    out_busy[i] = fd_fseq_join( fd_wksp_pod_map1( out_pod, "busy%lu", i ) );
  }

  ulong pack_depth = fd_pod_query_ulong( tile_pod, "depth", 0UL );
  if( FD_UNLIKELY( !pack_depth ) ) FD_LOG_ERR(( "depth unset or set to zero" ));

  ulong max_txn_per_microblock = MAX_MICROBLOCK_SZ/sizeof(fd_txn_p_t);
  ulong pack_footprint = fd_pack_footprint( pack_depth, out_cnt, max_txn_per_microblock );

  FD_LOG_INFO(( "packing blocks of at most %lu transactions to %lu bank tiles", max_txn_per_microblock, out_cnt ));

  ulong * poh_slot       = fd_wksp_pod_map( tile_pod, "poh_slot"       );
  ulong * poh_reset_slot = fd_wksp_pod_map( tile_pod, "poh_reset_slot" );

#define IN_CNT 3
  const fd_frag_meta_t * in_mcache[IN_CNT] = {
    fd_mcache_join( fd_wksp_pod_map( in_pod, "mcache" ) ),
    fd_mcache_join( fd_wksp_pod_map( in_pod, "gossip-mcache" ) ),
    fd_mcache_join( fd_wksp_pod_map( in_pod, "lsched-mcache" ) ),
  };

  ulong * in_fseq[IN_CNT] = {
    fd_fseq_join( fd_wksp_pod_map( in_pod, "fseq" ) ),
    fd_fseq_join( fd_wksp_pod_map( in_pod, "gossip-fseq" ) ),
    fd_fseq_join( fd_wksp_pod_map( in_pod, "lsched-fseq" ) ),

  };

  const uchar * in_dcache[IN_CNT] = {
    fd_dcache_join( fd_wksp_pod_map( in_pod, "dcache" ) ),
    fd_dcache_join( fd_wksp_pod_map( in_pod, "gossip-dcache" ) ),
    fd_dcache_join( fd_wksp_pod_map( in_pod, "lsched-dcache" ) ),
  };

  fd_rng_t   _rng[1];
  fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, 0, 0UL ) );
  fd_pack_tile( fd_cnc_join( fd_wksp_pod_map( tile_pod, "cnc" ) ),
                (ulong)args->pid,
                IN_CNT,
                in_mcache,
                in_fseq,
                in_dcache,
                fd_pack_join( fd_pack_new( fd_wksp_alloc_laddr( fd_wksp_containing( tile_pod ), fd_pack_align(), pack_footprint, FD_PACK_TAG ), pack_depth, out_cnt, max_txn_per_microblock, rng ) ),
                fd_mcache_join( fd_wksp_pod_map( out_pod, "mcache" ) ),
                fd_dcache_join( fd_wksp_pod_map( out_pod, "dcache" ) ),
                out_cnt,
                out_fseq,
                out_busy,
                0,
                0,
                poh_slot,
                poh_reset_slot,
                (void const *)args->lo_xsk,
                rng,
                fd_alloca( FD_PACK_TILE_SCRATCH_ALIGN, FD_PACK_TILE_SCRATCH_FOOTPRINT( IN_CNT, out_cnt ) ) );
#undef IN_CNT
}

static long allow_syscalls[] = {
  __NR_write,     /* logging */
  __NR_fsync,     /* logging, WARNING and above fsync immediately */
};

static workspace_kind_t allow_workspaces[] = {
  wksp_pack,       /* the tile itself */
  wksp_dedup_pack, /* receive path */
  wksp_pack_bank,  /* send path */
};

static ulong
allow_fds( fd_tile_args_t * args,
           ulong            out_fds_sz,
           int *            out_fds ) {
  (void)args;
  if( FD_UNLIKELY( out_fds_sz < 2 ) ) FD_LOG_ERR(( "out_fds_sz %lu", out_fds_sz ));
  out_fds[ 0 ] = 2; /* stderr */
  out_fds[ 1 ] = 3; /* logfile */
  return 2;
}

fd_tile_config_t pack = {
  .name                 = "pack",
  .allow_workspaces_cnt = sizeof(allow_workspaces)/sizeof(allow_workspaces[ 0 ]),
  .allow_workspaces     = allow_workspaces,
  .allow_syscalls_cnt   = sizeof(allow_syscalls)/sizeof(allow_syscalls[ 0 ]),
  .allow_syscalls       = allow_syscalls,
  .allow_fds            = allow_fds,
  .init                 = init,
  .run                  = run,
};
