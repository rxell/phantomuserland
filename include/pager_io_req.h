/**
 *
 * Phantom OS
 *
 * Copyright (C) 2005-2009 Dmitry Zavalishin, dz@dz.ru
 *
 * (block dev) IO request structure.
 *
 *
**/

/**
 *
 * Historically was used by paging only.
 * 
 * Maybe it is reasonable now to break it into the pager req and
 * regular io req?
 *
**/

#ifndef PAGER_IO_REQUEST_H
#define PAGER_IO_REQUEST_H

#include <phantom_types.h>
#include <queue.h>
#include <errno.h>
#include <string.h>
#include <kernel/pool.h>

#include <spinlock.h>

#define IO_RQ_SLEEP 1

typedef struct pager_io_request
{
    physaddr_t          phys_page;        	// physmem address
    disk_page_no_t      disk_page;        	// disk address in pages - as pager requested (ignored by io code)

    // TODO mustdie - together w. pager's queue
    struct pager_io_request *  next_page;       // used for pager or some other queue

    // Used internally by disk partitions support/driver code
    long                blockNo;         	// disk sector (usually 512-byte) no - this is what real io code looks at
    int                 nSect;                 	// no of disk sectors to be transferred

    unsigned char       flag_pagein;            // Read
    unsigned char       flag_pageout;           // Write

    unsigned char       flag_ioerror; 		// BUG - not used yet - TODO replace by rc below

    unsigned char       flag_urgent;  		// BUG - not used yet

//#if IO_RQ_SLEEP
    // If true - calling thread will be put onsleep until IO is done 
    unsigned char       flag_sleep;             // BUG - not used yet
    int                 sleep_tid; 		// Thread which was put asleep due to flag_sleep - filled by io code
    hal_spinlock_t      lock;
//#endif

    void                (*pager_callback)( struct pager_io_request *req, int write );

    hal_spinlock_t      chain_lock;             // disk io chaining lock
    /* The following two are modified under the chain_lock */
    unsigned char       flag_chained;           // This request is in disk io queue
    queue_chain_t       disk_chain;             // Disk io q chain

    errno_t             rc;                     // Driver return code

    pool_handle_t       phandle;                // This partition is to be released after io is done - see

    //struct disk_q	q;			// Disk q itself, used to start next req

    int                 unit;                   // Used on devices that have one q but multiple units that can't work at once

    int                 parts;                  // n physical reqs per this rq (used in driver code only, see virtio disk)

} pager_io_request;


static __inline__ void
pager_io_request_init( pager_io_request *me )
{ 
    memset(me, 0, sizeof(*me));
    me->flag_pagein     = 0;
    me->flag_pageout    = 0;
    me->flag_ioerror    = 0;
    me->flag_urgent     = 0;

    me->rc              = 0;

    me->blockNo         = 0;
    me->nSect           = 0;

    me->pager_callback  = 0;
//#if IO_RQ_SLEEP
    me->flag_sleep      = 0;
    me->sleep_tid       = 0;

    hal_spin_init( &(me->lock));

    hal_spin_init(&me->chain_lock);
    me->flag_chained    = 0;

    me->phandle         = -1;
    //me->q               = 0;
    me->unit            = 0;
//#endif
}


static inline bool io_request_is_complete(pager_io_request *rq)
{
    return (rq->flag_pageout == 0) && (rq->flag_pagein == 0);
}

void pager_io_request_done( pager_io_request *me );


#endif // PAGER_IO_REQUEST_H

