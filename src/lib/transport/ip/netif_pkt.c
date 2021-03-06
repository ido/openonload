/*
** Copyright 2005-2014  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/**************************************************************************\
** <L5_PRIVATE L5_SOURCE>
**   Copyright: (c) Level 5 Networks Limited.
**      Author: djr
**     Started: 2005/03/02
** Description: Packet buffer management.
** </L5_PRIVATE>
\**************************************************************************/

/*! \cidoxg_lib_transport_ip */
#include "ip_internal.h"

#if !defined(__KERNEL__)
#include <onload/mmap.h>
#include <sys/shm.h>

pthread_mutex_t citp_pkt_map_lock = PTHREAD_MUTEX_INITIALIZER;

ci_ip_pkt_fmt* __ci_netif_pkt(ci_netif* ni, unsigned id)
{
  int rc;
  ci_ip_pkt_fmt* pkt = 0;
  unsigned setid = id >> CI_CFG_PKTS_PER_SET_S;
  void *p;

  ci_assert(id != (unsigned)(-1));

  pthread_mutex_lock(&citp_pkt_map_lock);
  /* Recheck the condition now we have the lock */
  if( PKT_BUFSET_U_MMAPPED(ni, setid) )
    /* The mapping appeared while we were waiting for the lock */
    goto got_pkt_out;

#if CI_CFG_PKTS_AS_HUGE_PAGES
  if( ni->pkt_shm_id != NULL && ni->pkt_shm_id[setid] >= 0 ) {
    p = shmat(ni->pkt_shm_id[setid], NULL, 0);
    if( p == (void *)-1) {
      if( errno == EACCES ) {
        ci_log("Failed to mmap packet buffer for [%s] with errno=EACCES.\n"
               "Probably, you are using this stack from processes with "
               "different UIDs.\n"
               "Try either allowing user stack sharing: EF_SHARE_WITH=-1\n"
               "or turn off huge pages: EF_USE_HUGE_PAGES=0\n",
               ni->state->pretty_name);
      }
      else {
        ci_log("%s: shmat(0x%x) failed for pkt set %d (%d)", __FUNCTION__,
               ni->pkt_shm_id[setid], setid, -errno);
      }
      goto out;
    }
  }
  else
#endif
  {
    rc = oo_resource_mmap(ci_netif_get_driver_handle(ni),
                          CI_NETIF_MMAP_ID_PKTSET(setid),
                          CI_CFG_PKT_BUF_SIZE * PKTS_PER_SET,
                          &p);
    if( rc < 0 ) {
      ci_log("%s: oo_resource_mmap for pkt set %d failed (%d)",
             __FUNCTION__, setid, rc);
      goto out;
    }
  }
  ci_assert(p);
  ni->pkt_sets[setid] = p;

 got_pkt_out:
  pkt = (ci_ip_pkt_fmt*) __PKT_BUF(ni, id);

 out:
  pthread_mutex_unlock(&citp_pkt_map_lock);
  if( CI_UNLIKELY(pkt == NULL) )
    ci_log("Failed to map packets!  Crashing...");
  return pkt;
}

#endif


ci_ip_pkt_fmt* ci_netif_pkt_alloc_slow(ci_netif* ni, int for_tcp_tx)
{
  /* This is the slow path of ci_netif_pkt_alloc() and
  ** ci_netif_pkt_alloc_rtrans().  Either free pool is empty, or we have
  ** too few packets available to permit a retrans allocation.
  */
  ci_ip_pkt_fmt* pkt;

  ci_assert(ci_netif_is_locked(ni));
  ci_assert_equiv(ni->state->n_freepkts == 0,
                  OO_PP_IS_NULL(ni->state->freepkts));

#if ! CI_CFG_PP_IS_PTR
  /* TCP TX path is always allowed to allocate from the non-blocking pool
   * because those packet buffers are already allocated to TX.
   */
  if( for_tcp_tx || ni->state->pkt_sets_n == ni->state->pkt_sets_max )
    if( (pkt = ci_netif_pkt_alloc_nonb(ni)) != NULL ) {
      --ni->state->n_async_pkts;
      CITP_STATS_NETIF_INC(ni, pkt_nonb_steal);
      pkt->flags &= ~CI_PKT_FLAG_NONB_POOL;
      return pkt;
    }
#endif

  if( for_tcp_tx )
    if(CI_UNLIKELY( ! ci_netif_pkt_tx_may_alloc(ni) ))
      return NULL;

  while( ni->state->pkt_sets_n < ni->state->pkt_sets_max ) {
    int old_n_freepkts = ni->state->n_freepkts;
    int rc = ci_tcp_helper_more_bufs(ni);
    if( rc != 0 )
      break;
    CHECK_FREEPKTS(ni);
    if( old_n_freepkts == ni->state->n_freepkts )
      ci_assert_equal(ni->state->pkt_sets_n, ni->state->pkt_sets_max);
    if( OO_PP_NOT_NULL(ni->state->freepkts) )
      break;
  }

  if( OO_PP_IS_NULL(ni->state->freepkts) ) {
      return NULL;
  }

  return ci_netif_pkt_get(ni);
}


ci_inline void __ci_dbg_poison_header(ci_ip_pkt_fmt* pkt, ci_uint32 pattern) 
{
  unsigned i;
  ci_uint32* pkt_u32 = (ci_uint32 *)oo_ether_hdr(pkt);
  ci_uint32 patn_u32 = CI_BSWAP_BE32(pattern);
  ci_uint32 len = (ETH_HLEN + ETH_VLAN_HLEN + 2) + sizeof(ci_ip4_hdr) + 
    sizeof(ci_tcp_hdr);
  for( i = 0; i < len/4; i++ )  pkt_u32[i] = patn_u32;
}


void ci_netif_pkt_free(ci_netif* ni, ci_ip_pkt_fmt* pkt)
{
  ci_assert(pkt->refcount == 0);
  ci_assert(ci_netif_is_locked(ni));

  if( OO_PP_NOT_NULL(pkt->frag_next) ) {
    ci_netif_pkt_release(ni, PKT_CHK(ni, pkt->frag_next));
    pkt->frag_next = OO_PP_NULL;
  }

  if( pkt->flags & CI_PKT_FLAG_RX )
    --ni->state->n_rx_pkts;
  __ci_netif_pkt_clean(pkt);
#if CI_CFG_POISON_BUFS
  if( NI_OPTS(ni).poison_rx_buf )
    __ci_dbg_poison_header(pkt, 0xDECEA5ED);
#endif

  if( pkt->flags & CI_PKT_FLAG_NONB_POOL ) { 
    ci_netif_pkt_free_nonb_list(ni, OO_PKT_P(pkt), pkt);
    ++ni->state->n_async_pkts;
  }
  else {
    pkt->next = ni->state->freepkts;
    ni->state->freepkts = OO_PKT_P(pkt);
    ++ni->state->n_freepkts;
    CHECK_FREEPKTS(ni);
  }
}


void ci_netif_pkt_try_to_free(ci_netif* ni, int desperation)
{
  unsigned id;

  ci_assert(ci_netif_is_locked(ni));
  ci_assert_ge(desperation, 0);
  ci_assert_le(desperation, CI_NETIF_PKT_TRY_TO_FREE_MAX_DESP);

  /* We can't put arrays into the stats, so to avoid tests here we pretend
   * that we have an array.  This assertion should give some protection
   * against changes that break our assumption.
   */
  ci_assert(&ni->state->stats.pkt_scramble2 - &ni->state->stats.pkt_scramble0
            == CI_NETIF_PKT_TRY_TO_FREE_MAX_DESP);
  CITP_STATS_NETIF(++(&ni->state->stats.pkt_scramble0)[desperation]);

  for( id = 0; id < ni->state->n_ep_bufs; ++id )
  if( oo_sock_id_is_waitable(ni, id) )
  {
    citp_waitable_obj* wo = ID_TO_WAITABLE_OBJ(ni, id);
    if( wo->waitable.state & CI_TCP_STATE_TCP_CONN )
      ci_tcp_try_to_free_pkts(ni, &wo->tcp, desperation);
#if CI_CFG_UDP
    else if( wo->waitable.state == CI_TCP_STATE_UDP )
      ci_udp_try_to_free_pkts(ni, &wo->udp, desperation);
#endif
  }
}


ci_ip_pkt_fmt* ci_netif_pkt_alloc_block(ci_netif* ni, int* p_netif_locked)
{
  int was_locked = *p_netif_locked;
  ci_ip_pkt_fmt* pkt;
  int rc;

 again:
  if( *p_netif_locked == 0 ) {
    if( (pkt = ci_netif_pkt_alloc_nonb(ni)) )
      return pkt;
    if( ! ci_netif_trylock(ni) ) {
      rc = ci_netif_lock(ni);
      if(CI_UNLIKELY( ci_netif_lock_was_interrupted(rc) ))
        return NULL;
      CITP_STATS_NETIF_INC(ni, udp_send_ni_lock_contends/*??*/);
    }
    *p_netif_locked = 1;
  }

  if( (pkt = ci_netif_pkt_tx_tcp_alloc(ni)) ) {
    ++ni->state->n_async_pkts;
    if( ! was_locked ) {
      /* We would have preferred to have gotten this from the nonblocking
       * pool.  So arrange for it to be freed to that pool.
       */
      pkt->flags = CI_PKT_FLAG_NONB_POOL;
    }
    return pkt;
  }

  *p_netif_locked = 0;
  rc = ci_netif_pkt_wait(ni, CI_SLEEP_NETIF_LOCKED);
  if( ci_netif_pkt_wait_was_interrupted(rc) )
    return NULL;
  goto again;
}

/*! \cidoxg_end */
