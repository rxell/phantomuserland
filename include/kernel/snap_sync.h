/**
 *
 * Phantom OS
 *
 * Copyright (C) 2005-2009 Dmitry Zavalishin, dz@dz.ru
 *
 * Snapshot synchronizartion.
 *
 *
**/

#include <vm/internal_da.h>
#include <vm/alloc.h>


void phantom_thread_wait_4_snap( void );
void phantom_snapper_wait_4_threads( void );
void phantom_snapper_reenable_threads( void );

void phantom_snap_threads_interlock_init( void );

// see vm/refdec.c for use
void phantom_check_threads_pass_bytecode_instr_boundary( void );

extern volatile int     phantom_virtual_machine_snap_request;
extern volatile int     phantom_virtual_machine_stop_request; // Is one (with the phantom_virtual_machine_snap_request) when threads are asked to do harakiri


void phantom_finish_all_threads(void);
void activate_all_threads(void);

// supposed to be unused now?
void phantom_thread_sleep_worker( struct data_area_4_thread *thda );
// Can be called from SYS code only
void phantom_thread_put_asleep( struct data_area_4_thread *thda, VM_SPIN_TYPE *spin_to_unlock );
void phantom_thread_wake_up( struct data_area_4_thread *thda );


typedef struct userland_sleep
{
    int         is_sleeping;
    // mutex to interlock sema on/off/check
} userland_sleep_t;

#if NEW_SNAP_SYNC
extern volatile int * snap_catch_va;
// NB! Calling this means you're ready to snap
static inline void touch_snap_catch(void) { *snap_catch_va = 1; }

void snap_lock(void);
void snap_unlock(void);

void snap_trap(void);

#endif

