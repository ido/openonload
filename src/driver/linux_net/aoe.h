/*
** Copyright 2005-2013  Solarflare Communications Inc.
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

/****************************************************************************
 * Driver for Solarflare Solarstorm network controllers and boards
 * Copyright 2005-2006 Fen Systems Ltd.
 * Copyright 2006-2012 Solarflare Communications Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#ifndef EFX_AOE_H
#define EFX_AOE_H

#include "net_driver.h"
#include "efx.h"
#include "mcdi.h"
#include "mcdi_pcol.h"
#include "io.h"
#include "regs.h"
#include "nic.h"
#include "efx_ioctl.h"

#if defined(EFX_NOT_UPSTREAM) && defined( CONFIG_SFC_AOE)
bool efx_aoe_event(struct efx_nic *efx, efx_qword_t *event);
int efx_aoe_update_cpld(struct efx_nic *efx, struct efx_update_cpld *cpld);
int efx_aoe_update_keys(struct efx_nic *efx,
			struct efx_update_license *key_stats);
#else
bool efx_aoe_event(struct efx_nic *efx, efx_qword_t *event) { return false; };
int efx_aoe_update_cpld(struct efx_nic *efx, struct efx_update_cpld *cpld) { return -ENOSYS; }
int efx_aoe_update_keys(struct efx_nic *efx,
			struct efx_update_license *key_stats) {
	return -ENOSYS;
}
#endif

#endif /* EFX_AOE_H */