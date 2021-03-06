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
*//*! \file
** <L5_PRIVATE L5_HEADER >
** \author  ok_sasha
**  \brief  Linux driver mmap internal interfaces
**   \date  2007/06
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_include_ci_internal */

#ifndef __LINUX_CHAR_INTERNAL_H__
#define __LINUX_CHAR_INTERNAL_H__

#include "efch.h"
#include <linux/mm.h>
#include <linux/fs.h>


/* Name of the char device */
#define EFAB_CHAR_NAME "sfc_char"


/*--------------------------------------------------------------------
 *
 * ci_private_char_t - holds the per file descriptor private state
 *
 *--------------------------------------------------------------------*/

typedef struct ci_private_char_s {
  ci_resource_table_t  rt;
  struct efrm_vi*      cpcp_vi;
  int                  cpcp_readable;
  wait_queue_head_t    cpcp_poll_queue;
} ci_private_char_t;


extern void ci_mm_tbl_init(void);

extern int ci_char_fop_mmap(struct file* file, struct vm_area_struct* vma);

extern int
ci_mmap_bar(struct efhw_nic* nic, off_t base, size_t len, void* opaque,
            int* map_num, unsigned long* offset, int set_wc);

extern void
ci_mmap_iopages(struct efhw_iopages* p, unsigned offset, unsigned max_bytes,
		unsigned long* bytes, void* opaque,
		int* map_num, unsigned long* p_offset);


#endif /* __LINUX_CHAR_INTERNAL_H__ */
