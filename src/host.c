// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
#include "port.h"
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

// The SCI transport is deliberately a polling task. It invokes the real
// uACPI status/ack handler, while AML GPE and Notify work run on separate
// CPU0 tasks. No allocation, wait or AML under this short queue lock.
typedef struct { uacpi_work_handler fn; void *ctx; } Work;
typedef struct { Work items[256]; unsigned head, count, active; } Queue;
static Queue queues[2];
static unsigned char guard;
static unsigned stop_workers, fault;
static uacpi_interrupt_handler sci;
static void *sci_context;
static unsigned sci_active;
static uint64_t lock(void) {
    uint64_t flags = r4_irq_disable();
    while (__atomic_test_and_set(&guard, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void unlock(uint64_t flags) {
    __atomic_clear(&guard, __ATOMIC_RELEASE); r4_irq_restore(flags);
}
void r4_host_reset(void) {
    // Only before starting any task, after the previous instance was joined.
    for (unsigned i=0;i<2;i++) queues[i]=(Queue){0};
    guard=0; sci=0; sci_context=0; sci_active=0; fault=0; stop_workers=0;
}
void r4_host_stop(void) { __atomic_store_n(&stop_workers,1,__ATOMIC_RELEASE); }
int r4_host_fault(void) { return __atomic_load_n(&fault,__ATOMIC_ACQUIRE)!=0; }
int r4_host_worker(uint32_t kind) {
    if (kind > 2) return -1;
    while (!__atomic_load_n(&stop_workers,__ATOMIC_ACQUIRE)) {
        uint64_t flags=lock();
        if (kind==2) {
            uacpi_interrupt_handler fn=sci; void *ctx=sci_context;
            if (fn) sci_active++;
            unlock(flags);
            if (fn) { fn(ctx); flags=lock(); sci_active--; unlock(flags); }
            r4_sleep(10);
        } else {
            Queue *q=&queues[kind]; Work work={0};
            if(q->count) {work=q->items[q->head];q->head=(q->head+1)%256;q->count--;q->active++;}
            unlock(flags);
            if(work.fn) {work.fn(work.ctx);flags=lock();q->active--;unlock(flags);}
            else r4_sleep(2);
        }
    }
    return 0;
}
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    *out=r4_rsdp();return *out?UACPI_STATUS_OK:UACPI_STATUS_NOT_FOUND;
}
void *uacpi_kernel_map(uacpi_phys_addr p,uacpi_size n) {
    void *v=r4_view(p,n);return v?v:UACPI_MAP_FAILED;
}
void uacpi_kernel_unmap(void *v,uacpi_size n) {(void)v;(void)n;}
void uacpi_kernel_log(uacpi_log_level level,const char *text) {r4_log(level,text);}
void *uacpi_kernel_alloc(uacpi_size n) {return r4_alloc(n);}
void uacpi_kernel_free(void *p) {r4_free(p);}
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {return r4_now();}
void uacpi_kernel_stall(uacpi_u8 us) {
    const uint64_t start=r4_now(), delta=(uint64_t)us*1000;
    while(r4_now()-start<delta) __asm__ volatile("pause");
}
void uacpi_kernel_sleep(uacpi_u64 ms) {r4_sleep(ms);}
uacpi_thread_id uacpi_kernel_get_thread_id(void) {return (uacpi_thread_id)(uintptr_t)r4_thread();}
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {return r4_irq_disable();}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state f) {r4_irq_restore(f);}
uacpi_handle uacpi_kernel_create_mutex(void) {return (void*)(uintptr_t)r4_semaphore(1,1);}
void uacpi_kernel_free_mutex(uacpi_handle h) {r4_destroy((uintptr_t)h);}
uacpi_handle uacpi_kernel_create_event(void) {return (void*)(uintptr_t)r4_semaphore(0,UINT32_MAX);}
void uacpi_kernel_free_event(uacpi_handle h) {r4_destroy((uintptr_t)h);}
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h,uacpi_u16 ms) {
    int rc=r4_acquire((uintptr_t)h,ms);
    return rc==0?UACPI_STATUS_OK:rc==1?UACPI_STATUS_TIMEOUT:UACPI_STATUS_INTERNAL_ERROR;
}
void uacpi_kernel_release_mutex(uacpi_handle h) {r4_release((uintptr_t)h);}
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h,uacpi_u16 ms) {return r4_acquire((uintptr_t)h,ms)==0;}
void uacpi_kernel_signal_event(uacpi_handle h) {r4_release((uintptr_t)h);}
void uacpi_kernel_reset_event(uacpi_handle h) {while(r4_acquire((uintptr_t)h,0)==0){}}
uacpi_handle uacpi_kernel_create_spinlock(void) {
    unsigned char *p=r4_alloc(1);if(p)*p=0;return p;
}
void uacpi_kernel_free_spinlock(uacpi_handle h) {r4_free(h);}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    uint64_t f=r4_irq_disable();
    while(__atomic_test_and_set((unsigned char*)h,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return f;
}
void uacpi_kernel_unlock_spinlock(uacpi_handle h,uacpi_cpu_flags f) {
    __atomic_clear((unsigned char*)h,__ATOMIC_RELEASE);r4_irq_restore(f);
}
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    (void)req;__atomic_store_n(&fault,1,__ATOMIC_RELEASE);
    r4_log(UACPI_LOG_ERROR,"ACPIPLAT firmware Breakpoint/Fatal request; platform input will be withdrawn");
    return UACPI_STATUS_DENIED;
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,uacpi_interrupt_handler fn,uacpi_handle ctx,uacpi_handle *out) {
    (void)irq;
    uint64_t f=lock();if(sci){unlock(f);return UACPI_STATUS_ALREADY_EXISTS;}
    sci=fn;sci_context=ctx;*out=&sci;unlock(f);return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn,uacpi_handle h) {
    uint64_t f=lock();
    if(h!=&sci||sci!=fn){unlock(f);return UACPI_STATUS_INVALID_ARGUMENT;}
    sci=0;sci_context=0;unlock(f);
    // A timeout cannot authorize uACPI to free a callback still in use.
    for(;;){f=lock();unsigned n=sci_active;unlock(f);if(!n)break;r4_sleep(1);}
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,uacpi_work_handler fn,uacpi_handle ctx) {
    if((unsigned)type>1||!fn)return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t f=lock();Queue *q=&queues[type];
    if(q->count==256||__atomic_load_n(&stop_workers,__ATOMIC_ACQUIRE)){__atomic_store_n(&fault,1,__ATOMIC_RELEASE);unlock(f);return UACPI_STATUS_OUT_OF_MEMORY;}
    q->items[(q->head+q->count)%256]=(Work){fn,ctx};q->count++;unlock(f);return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    for(;;) {
        uint64_t f=lock();unsigned busy=sci_active;
        for(unsigned i=0;i<2;i++)busy+=queues[i].count+queues[i].active;
        unlock(f);if(!busy)return UACPI_STATUS_OK;r4_sleep(1);
    }
}

typedef struct {uint16_t base;uint32_t length;} Io;
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base,uacpi_size length,uacpi_handle *out) {
    if(!length||base>65535||length>65536-base)return UACPI_STATUS_INVALID_ARGUMENT;
    Io *io=r4_alloc(sizeof(*io));if(!io)return UACPI_STATUS_OUT_OF_MEMORY;
    *io=(Io){base,length};*out=io;return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) {r4_free(h);}
#define IO(W,T) \
uacpi_status uacpi_kernel_io_read##W(uacpi_handle h,uacpi_size off,T *v) { \
    Io *io=h;if(!io||!v||off>io->length||sizeof(T)>io->length-off)return UACPI_STATUS_INVALID_ARGUMENT; \
    *v=(T)r4_in(io->base+off,sizeof(T));return UACPI_STATUS_OK; } \
uacpi_status uacpi_kernel_io_write##W(uacpi_handle h,uacpi_size off,T v) { \
    Io *io=h;if(!io||off>io->length||sizeof(T)>io->length-off)return UACPI_STATUS_INVALID_ARGUMENT; \
    r4_out(io->base+off,sizeof(T),v);return UACPI_STATUS_OK; }
IO(8,uacpi_u8) IO(16,uacpi_u16) IO(32,uacpi_u32)

// ECAM needs no shared CF8/CFC selector. Borrow only the kernel's existing
// permanent mapping; a missing segment/mapping is an explicit error, never
// a made-up absent-device success. MCFG is checksum-validated by uACPI.
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a,uacpi_handle *out) {
    if(a.device>31||a.function>7)return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_table t;uacpi_status rc=uacpi_table_find_by_signature("MCFG",&t);
    if(rc!=UACPI_STATUS_OK)return UACPI_STATUS_UNIMPLEMENTED;
    struct acpi_mcfg *m=t.ptr;
    rc=UACPI_STATUS_UNIMPLEMENTED;
    if(m->hdr.length>=sizeof(*m)&&(m->hdr.length-sizeof(*m))%sizeof(m->entries[0])==0) {
        unsigned n=(m->hdr.length-sizeof(*m))/sizeof(m->entries[0]);
        for(unsigned i=0;i<n;i++) {
            const struct acpi_mcfg_allocation *e=&m->entries[i];
            if(e->segment!=a.segment||e->start_bus>a.bus||e->end_bus<a.bus||e->start_bus>e->end_bus||e->address&0xfffff)continue;
            uint64_t offset=((uint64_t)a.bus<<20)|((uint64_t)a.device<<15)|((uint64_t)a.function<<12);
            if(e->address>UINT64_MAX-offset)break;
            *out=r4_view(e->address+offset,4096);
            rc=*out?UACPI_STATUS_OK:UACPI_STATUS_MAPPING_FAILED;break;
        }
    }
    uacpi_table_unref(&t);return rc;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) {(void)h;}
#define PCI(W,T) \
uacpi_status uacpi_kernel_pci_read##W(uacpi_handle h,uacpi_size off,T *v) { \
    if(!h||!v||off>4096-sizeof(T)||off%sizeof(T))return UACPI_STATUS_INVALID_ARGUMENT; \
    *v=*(volatile T*)((char*)h+off);return UACPI_STATUS_OK; } \
uacpi_status uacpi_kernel_pci_write##W(uacpi_handle h,uacpi_size off,T v) { \
    if(!h||off>4096-sizeof(T)||off%sizeof(T))return UACPI_STATUS_INVALID_ARGUMENT; \
    *(volatile T*)((char*)h+off)=v;return UACPI_STATUS_OK; }
PCI(8,uacpi_u8) PCI(16,uacpi_u16) PCI(32,uacpi_u32)
