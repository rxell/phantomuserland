/**
 *
 * Phantom OS
 *
 * Copyright (C) 2005-2012 Dmitry Zavalishin, dz@dz.ru
 *
 * Quick and dirty PS2 mouse driver.
 *
**/

#ifdef ARCH_ia32


#define DEBUG_MSG_PREFIX "mouse"
#include <debug_ext.h>
#define debug_level_flow 0
#define debug_level_error 10
#define debug_level_info 10

#include <ia32/pio.h>
#include <phantom_libc.h>

#include <kernel/device.h>
#include <kernel/drivers.h>
#include <hal.h>
#include <errno.h>

//#include <drv_video_screen.h>
#include <video/screen.h>

#include <event.h>
#include <queue.h>

#include <threads.h>
#include <kernel/config.h>
#include <time.h>



#define PS2_DATA_ADDR 	0x60
#define PS2_CTRL_ADDR 	0x64




// control words
#define PS2_CTRL_WRITE_CMD       0x60
#define PS2_CTRL_WRITE_AUX       0xD4

// data words
#define PS2_CMD_DEV_INIT         0x43
#define PS2_CMD_ENABLE_MOUSE     0xF4
#define PS2_CMD_DISABLE_MOUSE    0xF5
#define PS2_CMD_RESET_MOUSE      0xFF

#define PS2_RES_ACK              0xFA
#define PS2_RES_RESEND           0xFE


#define MAX_EVENTS      64


static struct ui_event  ebuf[MAX_EVENTS];
static int put_pos = 0;
static int get_pos = 0;
static hal_spinlock_t    elock;

static hal_sem_t 	mouse_sem;

static void put_buf(struct ui_event *e)
{
    //int ie = hal_save_cli();
    hal_spin_lock_cli( &elock );

    // Overflow - just loose it
    if(
       (put_pos == get_pos-1) ||
       ((get_pos == 0) && (put_pos == MAX_EVENTS-1))
      )
        goto ret;

    ebuf[put_pos++] = *e;

    if(put_pos >= MAX_EVENTS)
        put_pos = 0;

ret:
    hal_spin_unlock_sti( &elock );
    //if(ie) hal_sti();
}

static int get_buf(struct ui_event *e)
{
    int ret = 1;

    //int ie = hal_save_cli();
    hal_spin_lock_cli( &elock );

    if( get_pos == put_pos )
    {
        ret = 0;
        goto fin;
    }
    *e = ebuf[get_pos++];
    ret = 1;

    if(get_pos >= MAX_EVENTS)
        get_pos = 0;

fin:
    hal_spin_unlock_sti( &elock );
    //if(ie) hal_sti();

    return ret;
}

static int peek_buf(struct ui_event *e)
{
    int ret = 1;

    //int ie = hal_save_cli();
    hal_spin_lock_cli( &elock );

    if( get_pos == put_pos )
    {
        ret = 0;
        goto fin;
    }
    *e = ebuf[get_pos];
    ret = 1;

fin:
    hal_spin_unlock_sti( &elock );
    //if(ie) hal_sti();

    return ret;
}






static void ps2ms_int_handler( void * arg );
static int ps2ms_do_init( void );


static unsigned char 	ps2ms_state_buttons = 0;
static int 		ps2ms_state_xpos = 0;
static int 		ps2ms_state_ypos = 0;
static int		xsign, ysign, xval, yval;



static void mouse_push_event_thread(void *arg)
{
    (void) arg;

    t_current_set_name("MouEvents");
    t_current_set_priority(PHANTOM_SYS_THREAD_PRIO);

    while(1)
    {
        hal_sem_acquire( &mouse_sem );

        if(video_drv->mouse_redraw_cursor != NULL)
            video_drv->mouse_redraw_cursor();

        //hal_mutex_lock( &mouse_mutex );

        struct ui_event e, e1;
        while( get_buf(&e) )
        {
            if( peek_buf( &e1 ) )
            {
                // We already have one more event and buttons state is the same?
                // Throw away current event, use next one.
                if( e1.m.buttons == e.m.buttons )
                    continue;
            }

            ev_q_put_any( &e );
        }

        //hal_mutex_unlock( &mouse_mutex );
    }
}




static int insert_bit9( int val, int sign )
{
#if 0
    return val;
#else
    val &= 0xFFu;

    if( sign )
    {
        // set top bits
        val = ~val;
        val &= 0xFFu; 
        val = ~val;
    }

    return val;
#endif
}



static void ps2ms_int_handler( void *arg )
{
    (void) arg;

    static int inbytepos = 0;

    signed char mousedata = inb( PS2_DATA_ADDR );

    SHOW_FLOW( 10 ,"%2X ", mousedata & 0xFFu );

    switch(inbytepos)
    {
    case 0:

        // first byte has one in this pos
        if(1 && ! (0x8 & mousedata) )
        {
            //inbytepos = -1; break;
            inbytepos = 0; return;
        }

        ps2ms_state_buttons = 0x7 & mousedata;
        xsign = 0x10 & mousedata;
        ysign = 0x20 & mousedata;
        break;

    case 1:     xval = mousedata; break;
    case 2:     yval = mousedata; break;
    case 3:     break;
    case 4:     break;
    }

    inbytepos++;
    inbytepos %= 3;
    //inbytepos %= 4;

    if(inbytepos != 0)
        return;

    xval = insert_bit9( xval, xsign );
    yval = insert_bit9( yval, ysign );

    ps2ms_state_xpos += xval;
    ps2ms_state_ypos += yval;

    if( ps2ms_state_xpos < 0 ) ps2ms_state_xpos = 0;
    if( ps2ms_state_ypos < 0 ) ps2ms_state_ypos = 0;

    if( ps2ms_state_xpos > video_drv->xsize ) ps2ms_state_xpos = video_drv->xsize;
    if( ps2ms_state_ypos > video_drv->ysize ) ps2ms_state_ypos = video_drv->ysize;


    //printf("ms %d %d %x\n", ps2ms_state_xpos, ps2ms_state_ypos, ps2ms_state_buttons );

    if(NULL != video_drv)
    {
        video_drv->mouse_x = ps2ms_state_xpos;
        video_drv->mouse_y = ps2ms_state_ypos;

        struct ui_event e;
        e.type = UI_EVENT_TYPE_MOUSE;
        e.time = fast_time();
        e.focus= 0;

        e.m.buttons = ps2ms_state_buttons;
        e.abs_x = ps2ms_state_xpos;
        e.abs_y = ps2ms_state_ypos;

        put_buf(&e);
        hal_sem_release( &mouse_sem );
    }

}


static unsigned char ps2ms_get_data()
{
    while((inb(PS2_CTRL_ADDR) & 0x1) == 0)
        ;
    return inb( PS2_DATA_ADDR );
}

static void wait_write_data()
{
    while(inb(PS2_CTRL_ADDR) & 0x2);
}

static void wait_write_ctrl()
{
    while(inb(PS2_CTRL_ADDR) & 0x3);
}


static void ps2ms_send_cmd(unsigned char cmd)
{
    wait_write_ctrl();    	outb(PS2_CTRL_ADDR, PS2_CTRL_WRITE_CMD);
    wait_write_data();    	outb(PS2_DATA_ADDR, cmd);
}


static void ps2ms_send_aux(unsigned char aux)
{
    wait_write_ctrl();   	outb(PS2_CTRL_ADDR, PS2_CTRL_WRITE_AUX);
    wait_write_data();   	outb(PS2_DATA_ADDR, aux);
}

// check for ACK
static errno_t ps2ms_aux_wait_ack()
{
    if(ps2ms_get_data() != PS2_RES_ACK)
        return ENXIO;

    return 0;
}


static int ps2ms_do_init( void )
{


    int tries = 10000;
    // Purge buffer
    while( tries-- > 0 && inb( PS2_CTRL_ADDR ) & 0x01 )
        inb( PS2_DATA_ADDR );

    if( tries <= 0 ) goto notfound;

    ps2ms_send_cmd(PS2_CMD_DEV_INIT);

    // hangs
    //ps2ms_send_aux(PS2_CMD_RESET_MOUSE);    ps2ms_aux_wait_ack(); // ignore result

    ps2ms_send_aux(PS2_CMD_ENABLE_MOUSE);
    if( ps2ms_aux_wait_ack() ) goto notfound;

    return 0;

notfound:
    SHOW_ERROR0( 1, "PS/2 mouse not found\n" );
    return ENXIO;

}

static int seq_number = 0;

phantom_device_t * driver_isa_ps2m_probe( int port, int irq, int stage )
{
    (void) port;
    (void) stage;

    //dpc_request_init( &mouse_dpc, push_event );
    //hal_mutex_init( &mouse_mutex, "MouseDrv" );
    //hal_cond_init( &mouse_cond );
    hal_sem_init( &mouse_sem, "MouseDrv" );
    hal_spin_init( &elock );

    if( seq_number || ps2ms_do_init())
        return 0;

    hal_irq_alloc( irq, ps2ms_int_handler, 0, HAL_IRQ_SHAREABLE );

    phantom_device_t * dev = malloc(sizeof(phantom_device_t));
    dev->name = "ps2-mouse";
    dev->seq_number = seq_number++;

    hal_start_kernel_thread((void*)mouse_push_event_thread);

    return dev;
}



















#if 0 /*FOLD00*/
//Mouse.inc by SANiK
//License: Use as you wish, except to cause damage
byte mouse_cycle=0;     //unsigned char
sbyte mouse_byte[3];    //signed char
sbyte mouse_x=0;         //signed char
sbyte mouse_y=0;         //signed char

//Mouse functions
void mouse_handler(struct regs *a_r) //struct regs *a_r (not used but just there)
{
    switch(mouse_cycle)
    {
    case 0:
        mouse_byte[0]=inportb(0x60);
        mouse_cycle++;
        break;
    case 1:
        mouse_byte[1]=inportb(0x60);
        mouse_cycle++;
        break;
    case 2:
        mouse_byte[2]=inportb(0x60);
        mouse_x=mouse_byte[1];
        mouse_y=mouse_byte[2];
        mouse_cycle=0;
        break;
    }
}

inline void mouse_wait(byte a_type) //unsigned char
{
    dword _time_out=100000; //unsigned int
    if(a_type==0)
    {
        while(_time_out--) //Data
        {
            if((inportb(0x64) & 1)==1)
            {
                return;
            }
        }
        return;
    }
    else
    {
        while(_time_out--) //Signal
        {
            if((inportb(0x64) & 2)==0)
            {
                return;
            }
        }
        return;
    }
}

inline void mouse_write(byte a_write) //unsigned char
{
    //Wait to be able to send a command
    mouse_wait(1);
    //Tell the mouse we are sending a command
    outportb(0x64, 0xD4);
    //Wait for the final part
    mouse_wait(1);
    //Finally write
    outportb(0x60, a_write);
}

byte mouse_read()
{
    //Get's response from mouse
    mouse_wait(0);
    return inportb(0x60);
}

void mouse_install()
{
    byte _status;  //unsigned char

    //Enable the auxiliary mouse device
    mouse_wait(1);
    outportb(0x64, 0xA8);

    //Enable the interrupts
    mouse_wait(1);
    outportb(0x64, 0x20);
    mouse_wait(0);
    _status=(inportb(0x60) | 2);
    mouse_wait(1);
    outportb(0x64, 0x60);
    mouse_wait(1);
    outportb(0x60, _status);

    //Tell the mouse to use default settings
    mouse_write(0xF6);
    mouse_read();  //Acknowledge

    //Enable the mouse
    mouse_write(0xF4);
    mouse_read();  //Acknowledge

    //Setup the mouse handler
    irq_install_handler(12, mouse_handler);
}
#endif


/**

From: Tomi Engdahl <then@snakemail.hut.fi>
Date: Fri, 29 Apr 1994 13:28:37 +0200 (EET DST)


T{ss{ hiirien speksit englanniksi. Ei nyt ihan t{ydelliset, mutta kyll{
aika kattavat.


        Serial mouse

Voltage levels:
Mouse takes standard RS-232C output signals (+-12V) as its input signals.
Those outputs are in +12V when mouse is operated. Mouse takes some current
>from each of the RS-232C port output lines it is connected (about 10mA).
Mouse send data to computer in levels that RS-232C receiver chip in the
computer can uderstand as RS-232C input levels. Mouse outputs are normally
something like +-5V, 0..5V or sometimes +-12V. Mouse electronics
normally use +5V voltage.


        Microsoft serial mouse

Pins used:
TD, RTS and DTR are used only as power source for the mouse.
RD is used to receive data from mouse.

Serial data parameters: 1200bps, 7 databits, 1 stop-bit

Data packet format:
Data packet is 3 byte packet. It is send to the computer every time
mouse state changes (mouse moves or keys are pressed/released).

        D7      D6      D5      D4      D3      D2      D1      D0

1.      X       1       LB      RB      Y7      Y6      X7      X6
2.      X       0       X5      X4      X3      X2      X1      X0
3.      X       0       Y5      Y4      Y3      Y2      Y1      Y0

The byte marked with 1. is send first, then the others. The bit D6
in the first byte is used for syncronizing the software to mouse
packets if it goes out of sync.

LB is the state of the left button (0 means pressed down)
RB is the state of the right button (0 means pressed down)
X7-X0 movement in X direction since last packet (signed byte)
Y7-Y0 movement in Y direction since last packet (signed byte)


        Mouse systems mouse

Serial data parameters: 1200bps, 8 databits, 1 stop-bit

The data is sent in 5 byte packets in following format:

        D7      D6      D5      D4      D3      D2      D1      D0

1.      1       0       0       0       0       LB      CB      RB
2.      X7      X6      X5      X4      X3      X2      X1      X0
3.      Y7      Y6      Y5      Y4      Y3      Y4      Y1      Y0
4.
5.

LB is left button state (0=pressed, 1=released)
CB is center button state (0=pressed, 1=released)
RB is right button state (0=pressed, 1=released)
X7-X0 movement in X direction since last packet in signed byte
      format (-128..+127), positive direction right
Y7-Y0 movement in Y direction since last packet in signed byte
      format (-128..+127), positive direction up

The last two bytes in the packet (bytes 4 and 5) contains
information about movement data send in last packet. I have not
found exact information about those bytes. I have not also found
any use for such a information (maybe it is for syncronization
or something like that).





--
Tomi.Engdahl@hut.fi                        Helsinki University of Technology
G=Tomi S=Engdahl O=hut ADMD=fumail C=fi    Department of Computer Science
# This text is provided "as is" without any express or implied warranty #




**/

#endif // ARCH_ia32

