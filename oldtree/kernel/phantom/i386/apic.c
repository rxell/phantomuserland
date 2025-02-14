/**
 *
 * Phantom OS
 *
 * Copyright (C) 2005-2010 Dmitry Zavalishin, dz@dz.ru
 *
 * APIC support
 *
**/

#define DEBUG_MSG_PREFIX "apic"
#include <debug_ext.h>
#define debug_level_flow 0
#define debug_level_error 10
#define debug_level_info 10

#include "../misc.h"
#include <kernel/ia32/cpu.h>
#include <kernel/ia32/apic.h>
#include <kernel/ia32/idt.h>
#include <ia32/proc_reg.h>
#include <ia32/phantom_pmap.h>
#include <kernel/interrupts.h>


#include <hal.h>
#include <phantom_libc.h>
#include <kernel/init.h>
#include <kernel/boot.h>
#include <kernel/trap.h>

// scheduler_time_interrupt
#include <threads.h>


// NB! Spurious must have lower bits = 1111
#define APIC_SPURIOUS_VECTOR    (APIC_INT_BASE+31)
#define APIC_TIMER_VECTOR 	(APIC_INT_BASE+30)
#define APIC_ERROR_VECTOR       (APIC_INT_BASE+29)
#define APIC_ICI_VECTOR         (APIC_INT_BASE+28)

static int      have_apic = 0;

volatile ApicLocalUnit * apic_local_unit;

/*

  todo 1. Connect with scheduler (set task_pri by scheduler)
  todo 2. Use APIC timer for scheduler/task switch
  todo 3. IPI for TLB invalidation
 */




// This one is called on CPU 0 (BSP)
void phantom_init_apic(void)
{
    //if(!boot_cpu_has_apic())
    if(!ARCH_HAS_FLAG(ARCH_IA32_APIC))
    {
        printf("Boot CPU has no APIC?!\n");
        return;
    }
    printf("Activating APIC...");

    int rc = hal_alloc_vaddress( (void**)&apic_local_unit, 1);
    if(rc) panic("Can't alloc APIC vaddr");

    relocate_apic();


    int pte = create_pte(0xFEE00000,
                         INTEL_PTE_VALID|INTEL_PTE_WRITE|
                         INTEL_PTE_WTHRU|INTEL_PTE_NCACHE
                        );
    phantom_map_page( (linaddr_t)apic_local_unit, pte );

    printf("APIC ver. 0x%X, unit id 0x%X\n",
           0xFF && apic_local_unit->version.r,
           apic_local_unit->unit_id.r
          );

    have_apic = 1;

    phantom_setup_apic();

    // must be after imps_probe and must get address from it
    phantom_io_apic_init( 0xFEC00000 );

#if HAVE_SMP
    imps_probe();
#endif

}

// This one is called on all CPUs
void phantom_setup_apic(void)
{
#if 0
    phantom_turn_off_pic_scheduler_timer();

    apic_local_unit->divider_config.r = 0xA; // div 128
    apic_local_unit->init_count.r = 1024*1024; // some khz range?

    //char timer_vect = 0x40;
    apic_local_unit->timer_vector.r =
        APIC_TIMER_VECTOR |
        0x02000000 | 	// enable timer
        (1<<17); 	// periodic
#endif

    apic_local_unit->error_vector.r = APIC_ERROR_VECTOR;


#if 1
    // Actually enable APIC, disable focus check (why?)
    apic_local_unit->spurious_vector.r =
        APIC_SPURIOUS_VECTOR |
        APIC_SVR_SWEN | APIC_SVR_FOCUS
        ;
#endif


}



void relocate_apic(void)
{
    u_int64_t aa = rdmsr( APIC_BASE_MSR );

    aa &= 0xFFF;

    wrmsr( APIC_BASE_MSR, aa | 0xFEE00000 | (1<<11) ); // make sure it is there
}

void apic_eoi()
{
    apic_local_unit->eoi.r = 0; // 0 is critical for X2APIC
}





static void apic_timer()
{
    phantom_scheduler_time_interrupt();
}


static void apic_spur()
{
    SHOW_ERROR0( 0, "Spurious APIC interrupt" );
}


static void apic_err()
{
    apic_local_unit->error_status.r = 0; // request to update
    int err = apic_local_unit->error_status.r;
    SHOW_ERROR( 0, "APIC error interrupt, err = %b",
                err,
                "\020\1SendCs\2RecvCs\3SendAccept\4RecvAccept\5Rsvd5\6SendIllVect\7RecvIllVect\10IllRegAddr"
              );
}


static void apic_incoming_ici()
{
    SHOW_FLOW0( 10, "ICI interrupt" );
}



void hal_APIC_interrupt_dispatcher(struct trap_state *ts, int vector)
{
    vector += APIC_INT_BASE;

    switch(vector)
    {
    case APIC_SPURIOUS_VECTOR:
        apic_spur();
        return; // NB! No EOI!

    case APIC_ERROR_VECTOR:
        apic_err();
        break;

    case APIC_ICI_VECTOR:
        apic_incoming_ici();
        break;

    case APIC_TIMER_VECTOR:
        apic_timer();
        break;

    default:
        {
            // TODO will die on ints >= 16
            check_global_lock_entry_count();

            irq_nest++;
            call_irq_handler(ts,vector);
            irq_nest--;

            check_global_lock_entry_count();
        }
    }

    printf("APIC int %d ", vector);
    apic_eoi();

    if(irq_nest)
        return;

#if USE_SOFTIRQ_DISABLE

    // Now for soft IRQs
    irq_nest = SOFT_IRQ_DISABLED|SOFT_IRQ_NOT_PENDING;
    hal_softirq_dispatcher(ts);
    ENABLE_SOFT_IRQ();
#else
#error so?
#endif
}

int GET_CPU_ID(void)
{
    //asm volatile("movl %0" : : "m" (mystate));
    return
        have_apic ?
            ((apic_local_unit->unit_id.r >> 24) & 0xF)
        :
            0
        ;
}


/*
#define GET_PROCESSOR_ID \
	"movl "SYMBOL_NAME_STR(apic_reg)", %edx\n\t" \
	"movl 32(%edx), %eax\n\t" \
	"shrl $24,%eax\n\t" \
	"andb $0x0F,%al\n"



        */







void phantom_smp_send_broadcast_ici(void)
{
    int config;

    int ie = hal_save_cli();

    config = apic_local_unit->int_command[0].r & APIC_ICR1_WRITE_MASK;
    apic_local_unit->int_command[0].r = config | APIC_ICI_VECTOR | APIC_ICR1_DELMODE_FIXED | APIC_ICR1_DESTMODE_PHYS | APIC_ICR1_DEST_ALL_BUT_SELF;

    if(ie) hal_sti();
}



/* cpu_apic_id?

void arch_smp_send_ici(int target_cpu)
{
    int config;

    int ie = hal_save_cli();

    config = apic_local_unit->int_command[1].r & APIC_ICR2_MASK;
    apic_local_unit->int_command[1].r = config | cpu_apic_id[target_cpu] << 24;

    config = apic_local_unit->int_command[0].r & APIC_ICR1_WRITE_MASK;
    apic_local_unit->int_command[0].r = config | 0xfd | APIC_ICR1_DELMODE_FIXED | APIC_ICR1_DESTMODE_PHYS | APIC_ICR1_DEST_FIELD;

    if(ie) hal_sti();
}

*/




























#if 0


static int num_cpus = 1;
//static unsigned int *apic = NULL;
static unsigned int cpu_apic_id[_MAX_CPUS] = { 0 };
static unsigned int cpu_os_id[_MAX_CPUS] = { 0 };
static unsigned int cpu_apic_version[_MAX_CPUS] = { 0 };
static unsigned int *ioapic = NULL;
static unsigned int apic_timer_tics_per_sec = 0;

static int i386_timer_interrupt(void* data)
{
    apic_eoi();

    return apic_timer_interrupt();
}

static int i386_ici_interrupt(void* data)
{
    // gin-u-wine inter-cpu interrupt
    //	dprintf("inter-cpu interrupt on cpu %d\n", arch_smp_get_current_cpu());
    apic_eoi();

    return smp_intercpu_int_handler();
}

static int i386_spurious_interrupt(void* data)
{
    // spurious interrupt
    //	dprintf("spurious interrupt on cpu %d\n", arch_smp_get_current_cpu());
    apic_eoi();
    return INT_NO_RESCHEDULE;
}

static int i386_smp_error_interrupt(void* data)
{
    // smp error interrupt
    //	dprintf("smp error interrupt on cpu %d\n", arch_smp_get_current_cpu());
    apic_eoi();
    return INT_NO_RESCHEDULE;
}


int arch_smp_init(kernel_args *ka)
{
    dprintf("arch_smp_init: entry\n");

    if(ka->num_cpus > 1) {
        // setup some globals
        num_cpus = ka->num_cpus;
        apic = ka->arch_args.apic;
        ioapic = ka->arch_args.ioapic;
        memcpy(cpu_apic_id, ka->arch_args.cpu_apic_id, sizeof(ka->arch_args.cpu_apic_id));
        memcpy(cpu_os_id, ka->arch_args.cpu_os_id, sizeof(ka->arch_args.cpu_os_id));
        memcpy(cpu_apic_version, ka->arch_args.cpu_apic_version, sizeof(ka->arch_args.cpu_apic_version));
        apic_timer_tics_per_sec = ka->arch_args.apic_time_cv_factor;

        // setup regions that represent the apic & ioapic
        vm_create_anonymous_region(vm_get_kernel_aspace_id(), "local_apic", (void *)&apic,
                                   REGION_ADDR_EXACT_ADDRESS, PAGE_SIZE, REGION_WIRING_WIRED_ALREADY, LOCK_RW|LOCK_KERNEL);
        vm_create_anonymous_region(vm_get_kernel_aspace_id(), "ioapic", (void *)&ioapic,
                                   REGION_ADDR_EXACT_ADDRESS, PAGE_SIZE, REGION_WIRING_WIRED_ALREADY, LOCK_RW|LOCK_KERNEL);

        // set up the local apic on the boot cpu
        arch_smp_init_percpu(ka, 1);

        // set up the interrupt handlers for local apic interrupts.
        // they are mapped to absolute interrupts 0xfb-0xff, but the io handlers are all
        // base 0x20, so subtract 0x20.
        int_set_io_interrupt_handler(0xfb - 0x20, &i386_timer_interrupt, NULL, "lapic timer");
        int_set_io_interrupt_handler(0xfd - 0x20, &i386_ici_interrupt, NULL, "ici");
        int_set_io_interrupt_handler(0xfe - 0x20, &i386_smp_error_interrupt, NULL, "lapic smp error");
        int_set_io_interrupt_handler(0xff - 0x20, &i386_spurious_interrupt, NULL, "lapic spurious");
    } else {
        num_cpus = 1;
    }

    return 0;
}

static int smp_setup_apic(kernel_args *ka)
{
    unsigned int config;

    /* set spurious interrupt vector to 0xff */
    config = apic_read(APIC_SIVR) & 0xfffffc00;
    config |= APIC_ENABLE | 0xff;
    apic_write(APIC_SIVR, config);
#if 0
    /* setup LINT0 as ExtINT */
    config = (apic_read(APIC_LINT0) & 0xffff1c00);
    config |= APIC_LVT_DM_ExtINT | APIC_LVT_IIPP | APIC_LVT_TM;
    apic_write(APIC_LINT0, config);

    /* setup LINT1 as NMI */
    config = (apic_read(APIC_LINT1) & 0xffff1c00);
    config |= APIC_LVT_DM_NMI | APIC_LVT_IIPP;
    apic_write(APIC_LINT1, config);
#endif

    /* setup timer */
    config = apic_read(APIC_LVTT) & ~APIC_LVTT_MASK;
    config |= 0xfb | APIC_LVTT_M; // vector 0xfb, timer masked
    apic_write(APIC_LVTT, config);

    apic_write(APIC_ICRT, 0); // zero out the clock

    config = apic_read(APIC_TDCR) & ~0x0000000f;
    config |= APIC_TDCR_1; // clock division by 1
    apic_write(APIC_TDCR, config);

    /* setup error vector to 0xfe */
    config = (apic_read(APIC_LVT3) & 0xffffff00) | 0xfe;
    apic_write(APIC_LVT3, config);

    /* accept all interrupts */
    config = apic_read(APIC_TPRI) & 0xffffff00;
    apic_write(APIC_TPRI, config);

    config = apic_read(APIC_SIVR);
    apic_write(APIC_EOI, 0);

    //	dprintf("done\n");
    return 0;
}

int arch_smp_init_percpu(kernel_args *ka, int cpu_num)
{
    // set up the local apic on the current cpu
    dprintf("arch_smp_init_percpu: setting up the apic on cpu %d", cpu_num);
    smp_setup_apic(ka);

    return 0;
}



/*void arch_smp_ack_interrupt(void)
 {
 apic_write(APIC_EOI, 0);
 }*/

#define MIN_TIMEOUT 1000

int arch_smp_set_apic_timer(bigtime_t relative_timeout, int type)
{
    unsigned int config;
    unsigned int ticks;

    if(apic == NULL)
        return -1;

    if(relative_timeout < MIN_TIMEOUT)
        relative_timeout = MIN_TIMEOUT;

    // calculation should be ok, since it's going to be 64-bit
    ticks = ((relative_timeout * apic_timer_tics_per_sec) / 1000000);

    int_disable_interrupts();

    config = apic_read(APIC_LVTT) | APIC_LVTT_M; // mask the timer
    apic_write(APIC_LVTT, config);

    apic_write(APIC_ICRT, 0); // zero out the timer

    config = apic_read(APIC_LVTT) & ~APIC_LVTT_M; // unmask the timer

    if(type == HW_TIMER_ONESHOT)
        config &= ~APIC_LVTT_TM; // clear the periodic bit
    else
        config |= APIC_LVTT_TM; // periodic

    apic_write(APIC_LVTT, config);

    dprintf("arch_smp_set_apic_timer: config 0x%x, timeout %Ld, tics/sec %d, tics %d\n",
            config, relative_timeout, apic_timer_tics_per_sec, ticks);

    apic_write(APIC_ICRT, ticks); // start it up

    int_restore_interrupts();

    return 0;
}

int arch_smp_clear_apic_timer(void)
{
    unsigned int config;

    if(apic == NULL)
        return -1;

    int_disable_interrupts();

    config = apic_read(APIC_LVTT) | APIC_LVTT_M; // mask the timer
    apic_write(APIC_LVTT, config);

    apic_write(APIC_ICRT, 0); // zero out the timer

    int_restore_interrupts();

    return 0;
}


#endif













#define MP_FLT_SIGNATURE '_PM_'
#define MP_CTH_SIGNATURE 'PCMP'

#if 0

static unsigned int *smp_probe(unsigned int base, unsigned int limit)
{
    unsigned int *ptr;

    printf("smp_probe: entry base 0x%x, limit 0x%x\n", base, limit);

    for (ptr = (unsigned int *) base; (unsigned int) ptr < limit; ptr++)
    {
        if (*ptr == MP_FLT_SIGNATURE) {
            printf("smp_probe: found floating pointer structure at 0x%x\n", ptr);
            return ptr;
        }
    }
    return NULL;
}



#endif



















#if 0 // From boot code


/*
 ** Copyright 2001, Travis Geiselbrecht. All rights reserved.
 ** Distributed under the terms of the NewOS License.
 */
#include <boot/stage2.h>
#include "stage2_priv.h"

#include <string.h>

#define CHATTY_SMP 0

static unsigned int mp_mem_phys = 0;
static unsigned int mp_mem_virt = 0;
static struct mp_flt_struct *mp_flt_ptr = NULL;
static kernel_args *saved_ka = NULL;
static unsigned int kernel_entry_point = 0;

static int smp_get_current_cpu(kernel_args *ka);

static unsigned int map_page(kernel_args *ka, unsigned int paddr, unsigned int vaddr)
{
    unsigned int *pentry;
    unsigned int *pgdir = (unsigned int *)(ka->arch_args.page_hole + (4*1024*1024-PAGE_SIZE));

    // check to see if a page table exists for this range
    if(pgdir[vaddr / PAGE_SIZE / 1024] == 0) {
        unsigned int pgtable;
        // we need to allocate a pgtable
        pgtable = ka->phys_alloc_range[0].start + ka->phys_alloc_range[0].size;
        ka->phys_alloc_range[0].size += PAGE_SIZE;
        ka->arch_args.pgtables[ka->arch_args.num_pgtables++] = pgtable;

        // put it in the pgdir
        pgdir[vaddr / PAGE_SIZE / 1024] = (pgtable & ADDR_MASK) | DEFAULT_PAGE_FLAGS;

        // zero it out in it's new mapping
        memset((unsigned int *)((unsigned int *)ka->arch_args.page_hole + (vaddr / PAGE_SIZE / 1024) * PAGE_SIZE), 0, PAGE_SIZE);
    }
    // now, fill in the pentry
    pentry = (unsigned int *)((unsigned int *)ka->arch_args.page_hole + vaddr / PAGE_SIZE);

    *pentry = (paddr & ADDR_MASK) | DEFAULT_PAGE_FLAGS;

    asm volatile("invlpg (%0)" : : "r" (vaddr));

    return 0;
}

static unsigned int apic_read(unsigned int *addr)
{
    return *addr;
}

static void apic_write(unsigned int *addr, unsigned int data)
{
    *addr = data;
}

/*
 static void *mp_virt_to_phys(void *ptr)
 {
 return ((void *)(((unsigned int)ptr - mp_mem_virt) + mp_mem_phys));
 }
 */
static void *mp_phys_to_virt(void *ptr)
{
    return ((void *)(((unsigned int)ptr - mp_mem_phys) + mp_mem_virt));
}


static void smp_do_config(kernel_args *ka)
{
    char *ptr;
    int i;
    struct mp_config_table *mpc;
    struct mp_ext_pe *pe;
    struct mp_ext_ioapic *io;
    struct mp_ext_bus *bus;
#if CHATTY_SMP
    const char *cpu_family[] = { "", "", "", "", "Intel 486",
    "Intel Pentium", "Intel Pentium Pro", "Intel Pentium II" };
#endif

    /*
     * we are not running in standard configuration, so we have to look through
     * all of the mp configuration table crap to figure out how many processors
     * we have, where our apics are, etc.
     */
    ka->num_cpus = 0;

    mpc = mp_phys_to_virt(mp_flt_ptr->mpc);

    /* print out our new found configuration. */
    ptr = (char *) &(mpc->oem[0]);
#if CHATTY_SMP
    dprintf ("smp: oem id: %c%c%c%c%c%c%c%c product id: "
             "%c%c%c%c%c%c%c%c%c%c%c%c\n", ptr[0], ptr[1], ptr[2], ptr[3], ptr[4],
             ptr[5], ptr[6], ptr[7], ptr[8], ptr[9], ptr[10], ptr[11], ptr[12],
             ptr[13], ptr[14], ptr[15], ptr[16], ptr[17], ptr[18], ptr[19],
             ptr[20]);
    dprintf("smp: base table has %d entries, extended section %d bytes\n",
            mpc->num_entries, mpc->ext_len);
#endif
    ka->arch_args.apic_phys = (unsigned int)mpc->apic;

    ptr = (char *) ((unsigned int) mpc + sizeof (struct mp_config_table));
    for (i = 0; i < mpc->num_entries; i++) {
        switch (*ptr) {
        case MP_EXT_PE:
            pe = (struct mp_ext_pe *) ptr;
            ka->arch_args.cpu_apic_id[ka->num_cpus] = pe->apic_id;
            ka->arch_args.cpu_os_id[pe->apic_id] = ka->num_cpus;
            ka->arch_args.cpu_apic_version[ka->num_cpus] = pe->apic_version;
#if CHATTY_SMP
            dprintf ("smp: cpu#%d: %s, apic id %d, version %d%s\n",
                     ka->num_cpus, cpu_family[(pe->signature & 0xf00) >> 8],
                     pe->apic_id, pe->apic_version, (pe->cpu_flags & 0x2) ?
                     ", BSP" : "");
#endif
            ptr += 20;
            ka->num_cpus++;
            break;
        case MP_EXT_BUS:
            bus = (struct mp_ext_bus *)ptr;
#if CHATTY_SMP
            dprintf("smp: bus%d: %c%c%c%c%c%c\n", bus->bus_id,
                    bus->name[0], bus->name[1], bus->name[2], bus->name[3],
                    bus->name[4], bus->name[5]);
#endif
            ptr += 8;
            break;
        case MP_EXT_IO_APIC:
            io = (struct mp_ext_ioapic *) ptr;
            ka->arch_args.ioapic_phys = (unsigned int)io->addr;
#if CHATTY_SMP
            dprintf("smp: found io apic with apic id %d, version %d\n",
                    io->ioapic_id, io->ioapic_version);
#endif
            ptr += 8;
            break;
        case MP_EXT_IO_INT:
            ptr += 8;
            break;
        case MP_EXT_LOCAL_INT:
            ptr += 8;
            break;
        }
    }
    dprintf("smp: apic @ 0x%x, i/o apic @ 0x%x, total %d processors detected\n",
            (unsigned int)ka->arch_args.apic_phys, (unsigned int)ka->arch_args.ioapic_phys, ka->num_cpus);

    // this BIOS looks broken, because it didn't report any cpus (VMWare)
    if(ka->num_cpus == 0) {
        ka->num_cpus = 1;
    }
}

struct smp_scan_spots_struct {
    unsigned int start;
    unsigned int stop;
    unsigned int len;
};

static struct smp_scan_spots_struct smp_scan_spots[] = {
    { 0x9fc00, 0xa0000, 0xa0000 - 0x9fc00 },
    { 0xf0000, 0x100000, 0x100000 - 0xf0000 },
    { 0, 0, 0 }
};

static int smp_find_mp_config(kernel_args *ka)
{
    int i;

    // XXX for now, assume the memory is identity mapped by the 1st stage
    for(i=0; smp_scan_spots[i].len > 0; i++) {
        mp_flt_ptr = (struct mp_flt_struct *)smp_probe(smp_scan_spots[i].start,
                                                       smp_scan_spots[i].stop);
        if(mp_flt_ptr != NULL)
            break;
    }
#if !_WITH_SMP
    if(0) {
#else
        if(mp_flt_ptr != NULL) {
#endif
            mp_mem_phys = smp_scan_spots[i].start;
            mp_mem_virt = smp_scan_spots[i].start;

#if CHATTY_SMP
            dprintf ("smp_boot: intel mp version %s, %s", (mp_flt_ptr->mp_rev == 1) ? "1.1" :
                     "1.4", (mp_flt_ptr->mp_feature_2 & 0x80) ?
                     "imcr and pic compatibility mode.\n" : "virtual wire compatibility mode.\n");
#endif
            if (mp_flt_ptr->mpc == 0) {
                // XXX need to implement
#if 1
                ka->num_cpus = 1;
                return 1;
#else
                /* this system conforms to one of the default configurations */
                //			mp_num_def_config = mp_flt_ptr->mp_feature_1;
                dprintf ("smp: standard configuration %d\n", mp_flt_ptr->mp_feature_1);
                /*			num_cpus = 2;
                 ka->cpu_apic_id[0] = 0;
                 ka->cpu_apic_id[1] = 1;
                 apic_phys = (unsigned int *) 0xfee00000;
                 ioapic_phys = (unsigned int *) 0xfec00000;
                 kprintf ("smp: WARNING: standard configuration code is untested");
                 */
#endif
            } else {
                smp_do_config(ka);
            }
            return ka->num_cpus;
        } else {
            ka->num_cpus = 1;
            return 1;
        }
    }

    // target function of the trampoline code
    // The trampoline code should have the pgdir and a gdt set up for us,
    // along with us being on the final stack for this processor. We need
    // to set up the local APIC and load the global idt and gdt. When we're
    // done, we'll jump into the kernel with the cpu number as an argument.
    static int smp_cpu_ready(void)
    {
        kernel_args *ka = saved_ka;
        unsigned int curr_cpu = smp_get_current_cpu(ka);
        struct gdt_idt_descr idt_descr;
        struct gdt_idt_descr gdt_descr;

        //	dprintf("smp_cpu_ready: entry cpu %d\n", curr_cpu);

        // Important.  Make sure supervisor threads can fault on read only pages...
        asm("movl %%eax, %%cr0" : : "a" ((1 << 31) | (1 << 16) | (1 << 5) | 1));
        asm("cld");
        asm("fninit");

        // Set up the final idt
        idt_descr.a = IDT_LIMIT - 1;
        idt_descr.b = (unsigned int *)ka->arch_args.vir_idt;

        asm("lidt	%0;"
            : : "m" (idt_descr));

        // Set up the final gdt
        gdt_descr.a = GDT_LIMIT - 1;
        gdt_descr.b = (unsigned int *)ka->arch_args.vir_gdt;

        asm("lgdt	%0;"
            : : "m" (gdt_descr));

        asm("pushl  %0; "					// push the cpu number
            "pushl 	%1;	"					// kernel args
            "pushl 	$0x0;"					// dummy retval for call to main
            "pushl 	%2;	"		// this is the start address
            "ret;		"					// jump.
            : : "r" (curr_cpu), "m" (ka), "g" (kernel_entry_point));

        // no where to return to
        return 0;
    }

    static int smp_boot_all_cpus(kernel_args *ka)
    {
        unsigned int trampoline_code;
        unsigned int trampoline_stack;
        unsigned int i;

        // XXX assume low 1 meg is identity mapped by the 1st stage bootloader
        // and nothing important is in 0x9e000 & 0x9f000

        // allocate a stack and a code area for the smp trampoline
        // (these have to be < 1M physical)
        trampoline_code = 0x9f000; 	// 640kB - 4096 == 0x9f000
        trampoline_stack = 0x9e000; // 640kB - 8192 == 0x9e000
        map_page(ka, 0x9f000, 0x9f000);
        map_page(ka, 0x9e000, 0x9e000);

        // copy the trampoline code over
        memcpy((char *)trampoline_code, &smp_trampoline,
               (unsigned int)&smp_trampoline_end - (unsigned int)&smp_trampoline);

        // boot the cpus
        for(i = 1; i < ka->num_cpus; i++) {
            unsigned int *final_stack;
            unsigned int *final_stack_ptr;
            unsigned int *tramp_stack_ptr;
            unsigned int config;
            unsigned int num_startups;
            unsigned int j;

            // create a final stack the trampoline code will put the ap processor on
            ka->cpu_kstack[i].start = ka->virt_alloc_range[0].start + ka->virt_alloc_range[0].size;
            ka->cpu_kstack[i].size = STACK_SIZE * PAGE_SIZE;
            for(j=0; j<ka->cpu_kstack[i].size/PAGE_SIZE; j++) {
                // map the pages in
                map_page(ka, ka->phys_alloc_range[0].start + ka->phys_alloc_range[0].size,
                         ka->virt_alloc_range[0].start + ka->virt_alloc_range[0].size);
                ka->phys_alloc_range[0].size += PAGE_SIZE;
                ka->virt_alloc_range[0].size += PAGE_SIZE;
            }

            // set this stack up
            final_stack = (unsigned int *)ka->cpu_kstack[i].start;
            memset(final_stack, 0, STACK_SIZE * PAGE_SIZE);
            final_stack_ptr = (final_stack + (STACK_SIZE * PAGE_SIZE) / sizeof(unsigned int)) - 1;
            *final_stack_ptr = (unsigned int)&smp_cpu_ready;
            final_stack_ptr--;

            // set the trampoline stack up
            tramp_stack_ptr = (unsigned int *)(trampoline_stack + PAGE_SIZE - 4);
            // final location of the stack
            *tramp_stack_ptr = ((unsigned int)final_stack) + STACK_SIZE * PAGE_SIZE - sizeof(unsigned int);
            tramp_stack_ptr--;
            // page dir
            *tramp_stack_ptr = ka->arch_args.phys_pgdir;
            tramp_stack_ptr--;

            // put a gdt descriptor at the bottom of the stack
            *((unsigned short *)trampoline_stack) = 0x18-1; // LIMIT
            *((unsigned int *)(trampoline_stack + 2)) = trampoline_stack + 8;
            // put the gdt at the bottom
            memcpy(&((unsigned int *)trampoline_stack)[2], (void *)ka->arch_args.vir_gdt, 6*4);

            /* clear apic errors */
            if(ka->arch_args.cpu_apic_version[i] & 0xf0) {
                apic_write(APIC_ESR, 0);
                apic_read(APIC_ESR);
            }

            /* send (aka assert) INIT IPI */
            config = (apic_read(APIC_ICR2) & 0x00ffffff) | (ka->arch_args.cpu_apic_id[i] << 24);
            apic_write(APIC_ICR2, config); /* set target pe */
            config = (apic_read(APIC_ICR1) & 0xfff00000) | 0x0000c500;
            apic_write(APIC_ICR1, config);

            // wait for pending to end
            while((apic_read(APIC_ICR1) & 0x00001000) == 0x00001000);

            /* deassert INIT */
            config = (apic_read(APIC_ICR2) & 0x00ffffff) | (ka->arch_args.cpu_apic_id[i] << 24);
            apic_write(APIC_ICR2, config);
            config = (apic_read(APIC_ICR1) & 0xfff00000) | 0x00008500;

            // wait for pending to end
            while((apic_read(APIC_ICR1) & 0x00001000) == 0x00001000);
            //			dprintf("0x%x\n", apic_read(APIC_ICR1));

            /* wait 10ms */
            sleep(10000);

            /* is this a local apic or an 82489dx ? */
            num_startups = (ka->arch_args.cpu_apic_version[i] & 0xf0) ? 2 : 0;
            for (j = 0; j < num_startups; j++) {
                /* it's a local apic, so send STARTUP IPIs */
                apic_write(APIC_ESR, 0);

                /* set target pe */
                config = (apic_read(APIC_ICR2) & 0xf0ffffff) | (ka->arch_args.cpu_apic_id[i] << 24);
                apic_write(APIC_ICR2, config);

                /* send the IPI */
                config = (apic_read(APIC_ICR1) & 0xfff0f800) | APIC_DM_STARTUP |
                    (0x9f000 >> 12);
                apic_write(APIC_ICR1, config);

                /* wait */
                sleep(200);

                while((apic_read(APIC_ICR1)& 0x00001000) == 0x00001000);
            }
        }

        return 0;
    }

    // XXX fails on cpus with no RDTSC
    static void calculate_apic_timer_conversion_factor(kernel_args *ka)
    {
        long long t1, t2;
        unsigned int config;
        unsigned int count;

        // setup the timer
        config = apic_read(APIC_LVTT);
        config = (config & ~APIC_LVTT_MASK) + APIC_LVTT_M; // timer masked, vector 0
        apic_write(APIC_LVTT, config);

        config = (apic_read(APIC_TDCR) & ~0x0000000f) + 0xb; // divide clock by one
        apic_write(APIC_TDCR, config);

        t1 = system_time();
        apic_write(APIC_ICRT, 0xffffffff); // start the counter

        execute_n_instructions(128*20000);

        count = apic_read(APIC_CCRT);
        t2 = system_time();

        count = 0xffffffff - count;

        ka->arch_args.apic_time_cv_factor = (unsigned int)((1000000.0/(t2 - t1)) * count);

        dprintf("APIC ticks/sec = %d\n", ka->arch_args.apic_time_cv_factor);
    }

    int smp_boot(kernel_args *ka, unsigned int kernel_entry)
    {
        //	dprintf("smp_boot: entry\n");

        kernel_entry_point = kernel_entry;
        saved_ka = ka;

        if(smp_find_mp_config(ka) > 1) {
            //		dprintf("smp_boot: had found > 1 cpus\n");
            //		dprintf("post config:\n");
            //		dprintf("num_cpus = 0x%p\n", ka->num_cpus);
            //		dprintf("apic_phys = 0x%p\n", ka->arch_args.apic_phys);
            //		dprintf("ioapic_phys = 0x%p\n", ka->arch_args.ioapic_phys);

            // map in the apic & ioapic
            map_page(ka, ka->arch_args.apic_phys, ka->virt_alloc_range[0].start + ka->virt_alloc_range[0].size);
            ka->arch_args.apic = (unsigned int *)(ka->virt_alloc_range[0].start + ka->virt_alloc_range[0].size);
            ka->virt_alloc_range[0].size += PAGE_SIZE;

            map_page(ka, ka->arch_args.ioapic_phys, ka->virt_alloc_range[0].start + ka->virt_alloc_range[0].size);
            ka->arch_args.ioapic = (unsigned int *)(ka->virt_alloc_range[0].start + ka->virt_alloc_range[0].size);
            ka->virt_alloc_range[0].size += PAGE_SIZE;

            //		dprintf("apic = 0x%p\n", ka->arch_args.apic);
            //		dprintf("ioapic = 0x%p\n", ka->arch_args.ioapic);

            // calculate how fast the apic timer is
            calculate_apic_timer_conversion_factor(ka);

            //		dprintf("trampolining other cpus\n");
            smp_boot_all_cpus(ka);
            //		dprintf("done trampolining\n");
        }

        //	dprintf("smp_boot: exit\n");

        return 0;
    }

    static int smp_get_current_cpu(kernel_args *ka)
    {
        if(ka->arch_args.apic == NULL)
            return 0;
        else
            return ka->arch_args.cpu_os_id[(apic_read(APIC_ID) & 0xffffffff) >> 24];
    }


#endif

