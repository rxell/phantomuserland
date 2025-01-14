/**
 *
 * Phantom OS
 *
 * Copyright (C) 2005-2009 Dmitry Zavalishin, dz@dz.ru
 *
 * Kernel ready: yes
 * Preliminary: no
 *
 *
**/

#ifndef PVM_INTERNAL_DA_H
#define PVM_INTERNAL_DA_H


#include <vm/object.h>
#include <vm/exception.h>
//#include <drv_video_screen.h>

#include <video/window.h>

#include <hal.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <kernel/atomic.h>

//#include <kernel/timedcall.h>
#include <kernel/net_timer.h>


/** Extract (typed) object data area pointer from object pointer. */
#define pvm_object_da( o, type ) ((struct data_area_4_##type *)&(o.data->da))
/** Extract (typed) object data area pointer from object pointer. */
#define pvm_data_area( o, type ) ((struct data_area_4_##type *)&(o.data->da))

/** Num of slots in normal (noninternal) object. */
#define da_po_limit(o)	 (((o)->_da_size)/sizeof(struct pvm_object))
/** Slots access for noninternal object. */
#define da_po_ptr(da)  ((struct pvm_object *)&(da))

pvm_object_t pvm_storage_to_object(pvm_object_storage_t *st);

static inline pvm_object_t pvm_da_to_object(void *da)
{
    const int off = __offsetof(pvm_object_storage_t,da);
    pvm_object_storage_t *st = da-off;

    return pvm_storage_to_object(st);
}

void pvm_fill_syscall_interface( struct pvm_object iface, int syscall_count );




#if SMP
#warning VM spinlocks
#else

//#  define VM_SPIN_LOCK(___var) ({ hal_disable_preemption(); atomic_add(&(___var),1); assert( (___var) == 1 ); })
//#  define VM_SPIN_UNLOCK(___var) ({ atomic_add(&(___var),-1); hal_enable_preemption(); assert( (___var) == 0 ); })

#  define VM_SPIN_LOCK(___var) ({ hal_wired_spin_lock(&___var); })
#  define VM_SPIN_UNLOCK(___var) ({ hal_wired_spin_unlock(&___var); })
#  define VM_SPIN_TYPE hal_spinlock_t
#endif




/**
 *
 * Internal objects data areas.
 *
**/

struct data_area_4_array
{
    struct pvm_object      	page;
    int                 	page_size; // current page size
    int                 	used_slots; // how many slots are used now
};



struct data_area_4_call_frame
{
    struct pvm_object		istack; // integer (fast calc) stack
    struct pvm_object		ostack; // main object stack
    struct pvm_object		estack; // exception catchers

    //struct pvm_object		cs; 	// code segment, ref just for gc - OK without

    unsigned int		IP_max;	// size of code in bytes
    unsigned char *		code; 	// (byte)code itself

    unsigned int    		IP;

    struct pvm_object  		this_object;

    struct pvm_object		prev; // where to return!

    int                 	ordinal; // num of method we run

};



struct data_area_4_code
{
    unsigned int		code_size; // bytes
    unsigned char		code[];
};




struct data_area_4_int
{
    int				value;
};

// TODO: make sure it is not an lvalue for security reasons!
#define pvm_get_int( o )  ( (int) (((struct data_area_4_int *)&(o.data->da))->value))

struct data_area_4_long
{
    int64_t			value;
};

// TODO: make sure it is not an lvalue for security reasons!
#define pvm_get_long( o )  ( (int64_t) (((struct data_area_4_long *)&(o.data->da))->value))



struct data_area_4_string
{
    int				length; // bytes! (chars are unicode?)
    unsigned char		data[];
};

#define pvm_get_str_len( o )  ( (int) (((struct data_area_4_string *)&(o.data->da))->length))
#define pvm_get_str_data( o )  ( (char *) (((struct data_area_4_string *)&(o.data->da))->data))

int pvm_strcmp(pvm_object_t s1, pvm_object_t s2);


// NB! See JIT assembly hardcode for object structure offsets
struct data_area_4_class
{
    unsigned int                object_flags;			// object of this class will have such flags
    unsigned int                object_data_area_size;	// object of this class will have data area of this size
    struct pvm_object		object_default_interface; // default one

    unsigned int    		sys_table_id; // See above - index into the kernel's syscall tables table

    struct pvm_object		class_name;
    struct pvm_object		class_parent;

    struct pvm_object		static_vars; // array of static variables

    struct pvm_object		ip2line_maps; // array of maps: ip->line number
    struct pvm_object		method_names; // array of method names
    struct pvm_object		field_names; // array of field names
};



struct pvm_code_handler
{
    const unsigned char *   	code;
    unsigned int            	IP;   /* Instruction Pointer */
    unsigned int            	IP_max;
};



// TODO - some sleep/wakeup support
struct data_area_4_thread
{
    struct pvm_code_handler             code;           // Loaded by load_fast_acc from frame

    //unsigned long   		              thread_id; // Too hard to implement and nobody needs
    struct pvm_object  	                call_frame; 	// current

    // some owner pointer?
    struct pvm_object                   owner;
    struct pvm_object                   environment;

    hal_spinlock_t                      spin;           // used on manipulations with sleep_flag

#if OLD_VM_SLEEP
    volatile int                        sleep_flag;     // Is true if thread is put asleep in userland
#endif
    //timedcall_t                         timer;          // Who will wake us
    net_timer_event                     timer;          // Who will wake us
    pvm_object_t                        sleep_chain;    // More threads sleeping on the same event, meaningless for running thread
    VM_SPIN_TYPE *                      spin_to_unlock; // This spin will be unlocked after putting thread asleep

    int                                 tid;            // Actual kernel thread id - reloaded on each kernel restart

// fast access copies

    struct pvm_object  				_this_object; 	// Loaded by load_fast_acc from call_frame

    struct data_area_4_integer_stack *		_istack;        // Loaded by load_fast_acc from call_frame
    struct data_area_4_object_stack *		_ostack;        // Loaded by load_fast_acc from call_frame
    struct data_area_4_exception_stack *      	_estack;        // Loaded by load_fast_acc from call_frame


    // misc data
    int stack_depth;	// number of frames
    //long memory_size;	// memory allocated - deallocated by this thread
};

typedef struct data_area_4_thread thread_context_t;

/*
// Thread factory is used to keep thread number
struct data_area_4_thread_factory
{
};
*/


struct pvm_stack_da_common
{
    /**
     *
     * Root (first created) page of the stack.
     * rootda is shortcut pointer to root's data area (struct data_area_4_XXX_stack).
     *
     */
    struct pvm_object           	root;

    /**
     *
     * Current (last created) page of the stack.
     * This field is used in root stack page only.
     *
     * See curr_da below - it is a shortcut to curr object's data area.
     *
     * See also '#define set_me(to)' in stacks.c.
     *
     */
    struct pvm_object           	curr;

    /** Pointer to previous (older) stack page. */
    struct pvm_object  			prev;

    /** Pointer to next (newer) stack page. */
    struct pvm_object  			next;

    /** number of cells used. */
    unsigned int    			free_cell_ptr;

    /** Page array has this many elements. */
    unsigned int                        __sSize;
};


#define PVM_INTEGER_STACK_SIZE 64

struct data_area_4_integer_stack
{
    struct pvm_stack_da_common          common;

    struct data_area_4_integer_stack *  curr_da;

    int         			stack[PVM_INTEGER_STACK_SIZE];
};




#define PVM_OBJECT_STACK_SIZE 64

struct data_area_4_object_stack
{
    struct pvm_stack_da_common          common;

    struct data_area_4_object_stack *  	curr_da;

    struct pvm_object			stack[PVM_OBJECT_STACK_SIZE];
};



#define PVM_EXCEPTION_STACK_SIZE 64

struct data_area_4_exception_stack
{
    struct pvm_stack_da_common          common;

    struct data_area_4_exception_stack *	curr_da;

    struct pvm_exception_handler       stack[PVM_EXCEPTION_STACK_SIZE];
};


struct data_area_4_boot
{
    // Empty?
};



//#define PVM_DEF_TTY_XSIZE 800
//#define PVM_DEF_TTY_YSIZE 600
#define PVM_DEF_TTY_XSIZE 400
#define PVM_DEF_TTY_YSIZE 300
//#define PVM_MAX_TTY_PIXELS (1280*1024)
#define PVM_MAX_TTY_PIXELS (PVM_DEF_TTY_XSIZE*PVM_DEF_TTY_YSIZE)

#define PVM_MAX_TTY_TITLE 128

struct data_area_4_tty
{
#if NEW_WINDOWS
    window_handle_t                     w;
    rgba_t                              pixel[PVM_MAX_TTY_PIXELS];
#else
    drv_video_window_t                  w;
    /** this field extends w and works as it's last field. */
    rgba_t                              pixel[PVM_MAX_TTY_PIXELS];
#endif
    int                                 font_height; // Font is hardcoded yet - BUG - but we cant have ptr to kernel from object
    int                                 font_width;
    int                                 xsize, ysize; // in chars
    int                                 x, y; // in pixels
    rgba_t                              fg, bg; // colors

    char                                title[PVM_MAX_TTY_TITLE+1];
};


//#define MAX_MUTEX_THREADS 3


struct data_area_4_mutex
{
    //int                 poor_mans_pagefault_compatible_spinlock;
    VM_SPIN_TYPE      poor_mans_pagefault_compatible_spinlock;

    struct data_area_4_thread *owner_thread;

    // Up to MAX_MUTEX_THREADS are stored here
    //pvm_object_t	waiting_threads[MAX_MUTEX_THREADS];
    // And the rest is here
    pvm_object_t        waiting_threads_array;

    int                 nwaiting;
};

struct data_area_4_cond
{
    VM_SPIN_TYPE        poor_mans_pagefault_compatible_spinlock;

    struct data_area_4_thread *owner_thread;

    pvm_object_t        waiting_threads_array;
    int                 nwaiting;
};

struct data_area_4_sema
{
    VM_SPIN_TYPE        poor_mans_pagefault_compatible_spinlock;

    struct data_area_4_thread *owner_thread;

    pvm_object_t        waiting_threads_array;
    int                 nwaiting;

    int                 sem_value;
};



struct data_area_4_binary
{
    int                 data_size; // GCC refuses to have only unsized array in a struct
    unsigned char       data[];
};


struct data_area_4_closure
{
    /** Which object to call. */
    struct pvm_object   object;
    /** Which method to call. */
    int                 ordinal;
};


struct data_area_4_bitmap
{
    struct pvm_object   image; // .internal.binary
    int                 xsize;
    int                 ysize;
};


struct data_area_4_tcp
{
    //int waiting for recv
};



struct data_area_4_world
{
    int                 placeholder[8]; // for any case
};




#define WEAKREF_SPIN 1



struct data_area_4_weakref
{
    /** Object we point to */
    struct pvm_object   object;
#if WEAKREF_SPIN
    hal_spinlock_t      lock;   // interlocks access tp object from GC finalizer and from getting ref
#else
    hal_mutex_t         mutex;
#endif
};


// BUG! Size hardcode! Redo with separate object!
#define PVM_MAX_WIN_PIXELS 1024*1024

struct data_area_4_window
{
#if NEW_WINDOWS
    window_handle_t                     w;
    rgba_t                              pixel[PVM_MAX_TTY_PIXELS];
#else
    drv_video_window_t                  w;
    // this field extends w and works as it's last field. 
    rgba_t                              pixel[PVM_MAX_TTY_PIXELS];
#endif

    struct pvm_object   		connector;      // Used for callbacks - events

    int                                 x, y; // in pixels
    rgba_t                              fg, bg; // colors

    //struct pvm_object                   event_handler; // connection!
    char                                title[PVM_MAX_TTY_TITLE+1];
};


// Very dumb implementation, redo with hash map or bin search or tree
// Container entry has struct pvm_object at the beginning and the rest is 0-term name string
struct data_area_4_directory
{
    u_int32_t                           elSize;         // size of one dir entry, bytes, defaults to 256

    u_int32_t                           capacity;       // size of binary container in entries
    u_int32_t                           nEntries;       // number of actual entries

    struct pvm_object   		container;      // Where we actually hold it
};





#define IO_DA_BUFSIZE 4


struct data_area_4_io
{
    u_int32_t                           in_count;       // num of objects waiting for get
    u_int32_t                           out_count;      // num of objects put and not processed by kernel

    // Buffers are small and we don't bother with cycle, just
    // move contents. Input is on the right (higher index) side.
    pvm_object_t			ibuf[IO_DA_BUFSIZE];
    pvm_object_t			obuf[IO_DA_BUFSIZE];

    pvm_object_t                        in_sleep_chain;  // Threads sleeping for input
    pvm_object_t                        out_sleep_chain; // Threads sleeping for output

    u_int32_t				in_sleep_count;  // n of threads sleeping for input
    u_int32_t				out_sleep_count; // n of threads sleeping for output

#if WEAKREF_SPIN
    hal_spinlock_t      		lock;
#else
#error mutex?
    hal_mutex_t         		mutex;
    //struct pvm_object   		mutex;          // persistence-compatible mutex
#endif

    u_int32_t                           reset;          // not operational, unblock waiting threads
};


struct data_area_4_connection;

#define PVM_CONNECTION_WAKE 0

struct pvm_connection_ops
{
#if PVM_CONNECTION_WAKE
    // No block!

    // Check if op can be done, return 0 if so
    errno_t     (*check_operation)( int op_no, struct data_area_4_connection *c, struct data_area_4_thread *tc );

    // request to wake me up when ready
    errno_t     (*req_wake)( int op_no, struct data_area_4_connection *c, struct data_area_4_thread *tc );
#endif

    // Actually do op 
    errno_t     (*do_operation)( int op_no, struct data_area_4_connection *c, struct data_area_4_thread *tc, pvm_object_t o );

    // Init connection
    errno_t     (*init)( struct data_area_4_connection *c, struct data_area_4_thread *tc, const char *suffix );

    // Finish connection
    //errno_t     (*disconnect)( struct data_area_4_connection *c, struct data_area_4_thread *tc );
    errno_t     (*disconnect)( struct data_area_4_connection *c );
};




struct data_area_4_connection
{
    struct data_area_4_thread *         owner;		// Just this one can use
    struct pvm_connection_ops *         kernel;         // Stuff in kernel that serves us

    pvm_object_t                        callback;
    int                                 callback_method;
    int                                 n_active_callbacks;

    // Persistent kernel state, p_kernel_state_object is binary
    size_t                              p_kernel_state_size;
    pvm_object_t                        p_kernel_state_object;
    void *                              p_kernel_state;

    pvm_object_t 			(*blocking_syscall_worker)( pvm_object_t this, struct data_area_4_thread *tc, int nmethod, pvm_object_t arg );

    // Volatile kernel state
    size_t                              v_kernel_state_size;
    void *                              v_kernel_state;

    char                                name[1024];     // Used to reconnect on restart
};

errno_t phantom_connect_object( struct data_area_4_connection *da, struct data_area_4_thread *tc);
//errno_t phantom_disconnect_object( struct data_area_4_connection *da, struct data_area_4_thread *tc);
errno_t phantom_disconnect_object( struct data_area_4_connection *da );


// Internal connect - when connection is used by other object (host_object)
//int pvm_connect_object_internal(struct data_area_4_connection *da, int connect_type, pvm_object_t host_object, void *arg);
errno_t phantom_connect_object_internal(struct data_area_4_connection *da, int connect_type, pvm_object_t host_object, void *arg);



#endif // PVM_INTERNAL_DA_H

