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

/* Stuff that connects the oof module and the rest of onload. */

#include <onload/oof_interface.h>
#include <onload/oof_onload.h>
#include <ci/internal/ip.h>
#include <onload/tcp_helper.h>
#include <onload/tcp_driver.h>
#include <onload/debug.h>
#include <onload/cplane.h>
#include "tcp_filters_internal.h"
#include <onload/driverlink_filter.h>

#define skf_to_ep(skf)  CI_CONTAINER(tcp_helper_endpoint_t, oofilter, (skf))
#define skf_to_ni(skf)  (&skf_to_ep(skf)->thr->netif)


static void
oof_onload_on_cplane_ipadd(ci_ip_addr_net_t net_ip,
                           ci_ip_addrset_t  net_ipset,
                           ci_ip_addr_net_t net_bcast,
                           ci_ifid_t ifindex,
                           void* arg)
{
  efab_tcp_driver_t* on_drv = arg;

  if( net_ip )
    oof_manager_addr_add(on_drv->filter_manager, net_ip, ifindex);
}


static void
oof_onload_on_cplane_ipdel(ci_ip_addr_net_t net_ip,
                           ci_ip_addrset_t  net_ipset,
                           ci_ip_addr_net_t net_bcast,
                           ci_ifid_t ifindex,
                           void* arg)
{
  efab_tcp_driver_t* on_drv = arg;

  if( net_ip )
    oof_manager_addr_del(on_drv->filter_manager, net_ip, ifindex);
}

static void
oof_do_deferred_work_fn(struct work_struct *data)
{
  oof_do_deferred_work(container_of(data, efab_tcp_driver_t,
                                    filter_work_item)->filter_manager);
}

int
oof_onload_ctor(efab_tcp_driver_t* on_drv, unsigned local_addr_max)
{
  ci_assert(on_drv->filter_manager == NULL);
  on_drv->filter_manager = oof_manager_alloc(local_addr_max, on_drv);
  if( on_drv->filter_manager == NULL )
    return -ENOMEM;
  INIT_WORK(&on_drv->filter_work_item, oof_do_deferred_work_fn);

  on_drv->filter_manager_cp_handle =
    cicpos_ipif_callback_register(&on_drv->cplane_handle,
                                  oof_onload_on_cplane_ipadd,
                                  oof_onload_on_cplane_ipdel, on_drv);
  if( on_drv->filter_manager_cp_handle == 0 ) {
    ci_log("%s: cicpos_ipif_callback_register failed", __FUNCTION__);
    oof_manager_free(on_drv->filter_manager);
    on_drv->filter_manager = NULL;
    return -ENODEV;
  }

  return 0;
}


void
oof_onload_dtor(efab_tcp_driver_t* on_drv)
{
  if( on_drv->filter_manager == NULL )
    return;

  cicpos_ipif_callback_deregister(&on_drv->cplane_handle,
                                  on_drv->filter_manager_cp_handle);
  oof_manager_free(on_drv->filter_manager);
}


/**********************************************************************
 * Callbacks from oof to onload.
 */

struct tcp_helper_resource_s*
oof_cb_socket_stack(struct oof_socket* skf)
{
  return skf_to_ep(skf)->thr;
}


struct tcp_helper_cluster_s*
oof_cb_stack_thc(struct tcp_helper_resource_s* skf_stack)
{
  return skf_stack->thc;
}


int
oof_cb_socket_id(struct oof_socket* skf)
{
  return OO_SP_FMT(skf_to_ep(skf)->id);
}


int
oof_cb_stack_id(struct tcp_helper_resource_s* stack)
{
  return stack ? NI_ID(&stack->netif) : -1;
}


/* Fixme: most callers of oof_cb_sw_filter_insert do not check rc. */
int
oof_cb_sw_filter_insert(struct oof_socket* skf, unsigned laddr, int lport,
                        unsigned raddr, int rport, int protocol)
{
  ci_netif* ni = skf_to_ni(skf);
  return ci_netif_filter_insert(ni, OO_SP_FROM_INT(ni, skf_to_ep(skf)->id),
                                laddr, lport, raddr, rport, protocol);
}


void
oof_cb_sw_filter_remove(struct oof_socket* skf, unsigned laddr, int lport,
                        unsigned raddr, int rport, int protocol)
{
  ci_netif* ni = skf_to_ni(skf);
  ci_netif_filter_remove(ni, OO_SP_FROM_INT(ni, skf_to_ep(skf)->id),
                         laddr, lport, raddr, rport, protocol);
}


struct oof_socket*
oof_cb_sw_filter_lookup(struct tcp_helper_resource_s* stack,
                        unsigned laddr, int lport,
                        unsigned raddr, int rport, int protocol)
{
  ci_netif* ni = &stack->netif;
  int sock_id, tbl_idx;
  tbl_idx = ci_netif_filter_lookup(ni, laddr, lport, raddr, rport, protocol);
  if( tbl_idx < 0 )
    return NULL;
  sock_id = ni->filter_table->table[tbl_idx].id;
  if( ! IS_VALID_SOCK_ID(ni, sock_id) ) {
    OO_DEBUG_ERR(ci_log("%s: ERROR: %d %s "IPPORT_FMT" "IPPORT_FMT,
                        __FUNCTION__, NI_ID(ni), FMT_PROTOCOL(protocol),
                        IPPORT_ARG(laddr, lport), IPPORT_ARG(raddr, rport));
                 ci_log("--> idx=%d sock_id=%d sock_id_max=%d", tbl_idx,
                        sock_id, ni->ep_tbl_n));
    return NULL;
  }
  return &ni->ep_tbl[sock_id]->oofilter;
}


/* dlfilter callbacks are called from oof code to keep hw and dl filters
 * synchronized. */
void
oof_dl_filter_set(struct oo_hw_filter* filter, int stack_id, int protocol,
                  unsigned saddr, int sport, unsigned daddr, int dport)
{
  if( filter->dlfilter_handle != EFX_DLFILTER_HANDLE_BAD )
    efx_dlfilter_remove(efab_tcp_driver.dlfilter, filter->dlfilter_handle);
  efx_dlfilter_add(efab_tcp_driver.dlfilter, protocol,
                   daddr, dport, saddr, sport,
                   stack_id, &filter->dlfilter_handle);
}


void
oof_dl_filter_del(struct oo_hw_filter* filter)
{
  if( filter->dlfilter_handle != EFX_DLFILTER_HANDLE_BAD ) {
    efx_dlfilter_remove(efab_tcp_driver.dlfilter, filter->dlfilter_handle);
    filter->dlfilter_handle = EFX_DLFILTER_HANDLE_BAD;
  }
}


/* These two must really be the same as we compare a value that is set
 * CI_IFID_ALL with the OO_IFID_ALL constant
 */
CI_BUILD_ASSERT(CI_IFID_ALL == OO_IFID_ALL);


int 
oof_cb_get_hwport_mask(int ifindex, unsigned *hwport_mask)
{
  ci_irqlock_state_t lock_flags;
  int rc;
  cicp_lock(&CI_GLOBAL_CPLANE, &lock_flags);
  rc = cicp_get_active_hwport_mask(&CI_GLOBAL_CPLANE, ifindex, hwport_mask);
  cicp_unlock(&CI_GLOBAL_CPLANE, &lock_flags);
  return rc;
}


int
oof_cb_get_vlan_id(int ifindex, unsigned short *vlan_id)
{
  cicp_encap_t encap;
  int rc = cicp_llap_get_encapsulation(&CI_GLOBAL_CPLANE, ifindex, &encap);
  if( rc == 0 )
    *vlan_id = encap.vlan_id;

  return rc;
}


void
oof_cb_defer_work(void* owner_private)
{
  efab_tcp_driver_t* on_drv = owner_private;
  queue_work(CI_GLOBAL_WORKQUEUE, &on_drv->filter_work_item);
}
