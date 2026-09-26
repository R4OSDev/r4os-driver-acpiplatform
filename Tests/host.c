// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
#include "port.h"
#include "fixture.h"
#include <uacpi/acpi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern int r4_test_start(uint32_t);
extern void r4_test_join(void);
static struct acpi_rsdp root;
static struct acpi_fadt fadt;
static struct acpi_facs facs;
static unsigned char xsdt[36+24],ecdt[96],mcfg[60],pci[4096];
static unsigned char ports[65536],ec_ram[256],query[64];
static unsigned query_head,query_count,ec_command,ec_address,ec_phase,ec_status,ec_output;
static unsigned stuck,allocations,capabilities,lid,up,down,writes,ready_messages,retired_messages;
static unsigned char hardware_lock;
static unsigned deny_allocations;
static unsigned event_waits,queue_callbacks,inject_wait_work,inject_wait_failure;
static unsigned semaphore_creates,fail_semaphore_at;
static void counted_work(void *context) {
    (void)context;__atomic_add_fetch(&queue_callbacks,1,__ATOMIC_RELEASE);
}
static unsigned guard(void) {while(__atomic_test_and_set(&hardware_lock,__ATOMIC_ACQUIRE)){}return 0;}
static void unguard(void) {__atomic_clear(&hardware_lock,__ATOMIC_RELEASE);}
void *r4_alloc(size_t n) {if(__atomic_load_n(&deny_allocations,__ATOMIC_ACQUIRE))return 0;void *p=malloc(n);if(p)__atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED);return p;}
void r4_free(void *p) {if(p){__atomic_sub_fetch(&allocations,1,__ATOMIC_RELAXED);free(p);}}
uint64_t r4_rsdp(void) {return (uintptr_t)&root;}
static int within(uint64_t p,uint64_t n,const void *base,size_t bytes) {
    return p>=(uintptr_t)base&&n<=bytes&&p-(uintptr_t)base<=bytes-n;
}
void *r4_view(uint64_t p,uint64_t n) {
    if(within(p,n,&root,sizeof(root))||within(p,n,&fadt,sizeof(fadt))||within(p,n,&facs,sizeof(facs))||
       within(p,n,xsdt,sizeof(xsdt))||within(p,n,ecdt,sizeof(ecdt))||within(p,n,mcfg,sizeof(mcfg))||within(p,n,aml_fixture,sizeof(aml_fixture)))return (void*)(uintptr_t)p;
    if(p>=0x80000000&&n<=sizeof(pci)&&p-0x80000000<=sizeof(pci)-n)return pci+(p-0x80000000);
    return 0;
}
void r4_log(uint32_t level,const char *s) {
    if(strstr(s,"namespace ready"))__atomic_add_fetch(&ready_messages,1,__ATOMIC_RELEASE);
    if(strstr(s,"callbacks quiesced"))__atomic_add_fetch(&retired_messages,1,__ATOMIC_RELEASE);
    printf("[ACPI:%u] %s\n",level,s);
}
typedef struct {unsigned count,maximum;} Semaphore;
uint64_t r4_semaphore(uint32_t initial,uint32_t maximum) {
    if(__atomic_add_fetch(&semaphore_creates,1,__ATOMIC_RELAXED)==fail_semaphore_at)return 0;
    Semaphore *s=r4_alloc(sizeof(*s));if(!s)return 0;
    *s=(Semaphore){initial,maximum};return (uintptr_t)s;
}
int r4_acquire(uint64_t handle,uint16_t ms) {
    Semaphore *s=(void*)(uintptr_t)handle;
    if(ms==UINT16_MAX&&s->maximum==1) {
        __atomic_add_fetch(&event_waits,1,__ATOMIC_RELEASE);
        // Publish after the worker's empty check, before its actual wait.
        if(__atomic_exchange_n(&inject_wait_work,0,__ATOMIC_ACQ_REL))
            uacpi_kernel_schedule_work(0,counted_work,0);
        if(__atomic_exchange_n(&inject_wait_failure,0,__ATOMIC_ACQ_REL))return -1;
    }
    uint64_t end=r4_now()+(uint64_t)ms*1000000;
    for(;;) {
        unsigned value=__atomic_load_n(&s->count,__ATOMIC_ACQUIRE);
        if(value&&__atomic_compare_exchange_n(&s->count,&value,value-1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
        if(!ms||(ms!=65535&&r4_now()>=end))return 1;
        r4_sleep(1);
    }
}
void r4_release(uint64_t handle) {
    Semaphore *s=(void*)(uintptr_t)handle;unsigned value=__atomic_load_n(&s->count,__ATOMIC_ACQUIRE);
    while(value<s->maximum&&!__atomic_compare_exchange_n(&s->count,&value,value+1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){}
}
void r4_destroy(uint64_t h) {r4_free((void*)(uintptr_t)h);}
int r4_input(uint32_t kind,uint32_t value) {
    switch(kind) {
        case 4:__atomic_store_n(&capabilities,value,__ATOMIC_RELEASE);if(!value)__atomic_store_n(&lid,0,__ATOMIC_RELEASE);return 0;
        case 3:__atomic_store_n(&lid,value,__ATOMIC_RELEASE);return 0;
        case 1:__atomic_add_fetch(&up,1,__ATOMIC_RELEASE);return 0;
        case 2:__atomic_add_fetch(&down,1,__ATOMIC_RELEASE);return 0;
        default:return -1;
    }
}
uint32_t r4_in(uint16_t port,uint32_t width) {
    guard();uint32_t value=0;
    if(port==0x66&&width==1)value=ec_status|(query_count?0x20:0)|(stuck?2:0);
    else if(port==0x62&&width==1){value=ec_output;ec_status&=~1;}
    else if(port==0x508&&width==4)value=(uint32_t)(r4_now()/1000*3579545/1000000);
    else for(unsigned i=0;i<width;i++)value|=(uint32_t)ports[port+i]<<(8*i);
    unguard();return value;
}
void r4_out(uint16_t port,uint32_t width,uint32_t value) {
    guard();
    if((port==0x66||port==0x62)&&width==1) {
        writes++;
        if(port==0x66) {
            ec_command=value;ec_phase=0;
            if(value==0x84) {
                ec_output=0;
                if(query_count){ec_output=query[query_head];query_head=(query_head+1)%64;query_count--;}
                ec_status|=1;
            }
        } else if(ec_command==0x80) {ec_address=value;ec_output=ec_ram[value&255];ec_status|=1;}
        else if(ec_command==0x81) {
            if(!ec_phase){ec_address=value;ec_phase=1;}else {ec_ram[ec_address&255]=value;ec_phase=0;}
        }
    } else for(unsigned i=0;i<width;i++) {
        unsigned byte=(value>>(8*i))&255,p=port+i;
        if((p>=0x400&&p<0x404)||(p>=0x500&&p<0x502))ports[p]&=~byte;
        else ports[p]=byte;
    }
    unguard();
}
static void checksum(void *data,size_t n,unsigned index) {
    unsigned char *p=data,sum=0;p[index]=0;for(size_t i=0;i<n;i++)sum+=p[i];p[index]=-sum;
}
static void header(void *data,const char *signature,size_t n) {
    struct acpi_sdt_hdr *h=data;memcpy(h->signature,signature,4);h->length=n;h->revision=6;
    memcpy(h->oemid,"R4TEST",6);memcpy(h->oem_table_id,"INPUT035",8);h->oem_revision=1;
}
static void tables(int bad_ecdt) {
    memset(&root,0,sizeof(root));memset(&fadt,0,sizeof(fadt));memset(&facs,0,sizeof(facs));
    memset(xsdt,0,sizeof(xsdt));memset(ecdt,0,sizeof(ecdt));memset(mcfg,0,sizeof(mcfg));memset(ports,0,sizeof(ports));
    memset(ec_ram,0,sizeof(ec_ram));ec_ram[0]=1;ports[0x504]=1;
    query_count=query_head=ec_command=ec_phase=ec_status=stuck=0;ready_messages=retired_messages=capabilities=lid=up=down=writes=0;
    header(&fadt,"FACP",sizeof(fadt));fadt.x_dsdt=(uintptr_t)aml_fixture;fadt.x_firmware_ctrl=(uintptr_t)&facs;fadt.sci_int=9;
    fadt.pm1a_evt_blk=0x500;fadt.pm1_evt_len=4;fadt.pm1a_cnt_blk=0x504;fadt.pm1_cnt_len=2;
    fadt.pm_tmr_blk=0x508;fadt.pm_tmr_len=4;fadt.gpe0_blk=0x400;fadt.gpe0_blk_len=8;fadt.flags=ACPI_TMR_VAL_EXT;
    memcpy(facs.signature,"FACS",4);facs.length=sizeof(facs);facs.version=2;
    checksum(&fadt,sizeof(fadt),9);
    struct acpi_ecdt *e=(void*)ecdt;const char path[]="\\_SB.EC0";
    header(e,"ECDT",sizeof(*e)+sizeof(path));e->ec_control=(struct acpi_gas){1,8,0,1,bad_ecdt?0x62:0x66};e->ec_data=(struct acpi_gas){1,8,0,1,0x62};e->gpe_bit=0x12;
    memcpy(e->ec_id,path,sizeof(path));checksum(e,e->hdr.length,9);
    struct acpi_mcfg *m=(void*)mcfg;header(m,"MCFG",sizeof(mcfg));m->entries[0]=(struct acpi_mcfg_allocation){0x80000000,0,0,0,0};checksum(m,sizeof(mcfg),9);
    header(xsdt,"XSDT",sizeof(xsdt));uint64_t entries[]={(uintptr_t)&fadt,(uintptr_t)ecdt,(uintptr_t)mcfg};memcpy(xsdt+36,entries,sizeof(entries));checksum(xsdt,sizeof(xsdt),9);
    memcpy(root.signature,"RSD PTR ",8);memcpy(root.oemid,"R4TEST",6);root.revision=2;root.length=sizeof(root);root.xsdt_addr=(uintptr_t)xsdt;
    checksum(&root,20,8);checksum(&root,sizeof(root),32);
}
static int wait_value(unsigned *p,unsigned value,unsigned ms) {
    uint64_t end=r4_now()+(uint64_t)ms*1000000;
    while(__atomic_load_n(p,__ATOMIC_ACQUIRE)!=value){if(r4_now()>=end)return 0;r4_sleep(1);}return 1;
}
static void event(unsigned value) {
    guard();query[(query_head+query_count)%64]=value;query_count++;ports[0x402]|=4;unguard();
}
static int start(void) {
    if(r4_host_reset())return -1;r4_platform_reset();
    for(unsigned i=0;i<4;i++)if(r4_test_start(i))return -1;
    return 0;
}
static int queue_test(void) {
    // Startup allocation failure must not leave any of the earlier events.
    for(unsigned n=1;n<=4;n++) {
        semaphore_creates=0;fail_semaphore_at=n;
        if(r4_host_reset()==0||allocations)return 1;
    }
    fail_semaphore_at=0;
    if(r4_host_reset())return 1;
    queue_callbacks=event_waits=0;inject_wait_work=1;
    if(r4_test_start(0))return 1;
    if(!wait_value(&queue_callbacks,1,1000))goto failed;
    // At most one stale binary permit may need consuming before idle.
    r4_sleep(20);unsigned before=__atomic_load_n(&event_waits,__ATOMIC_ACQUIRE);
    r4_sleep(40);
    if(before!=__atomic_load_n(&event_waits,__ATOMIC_ACQUIRE))goto failed;
    for(unsigned i=0;i<200;i++)if(uacpi_kernel_schedule_work(0,counted_work,0))goto failed;
    r4_host_stop();r4_test_join();
    if(queue_callbacks!=201||allocations)return 1;

    // Stop before start still drains both admitted queues, including a full
    // queue whose many notifications coalesced into just one permit.
    if(r4_host_reset())return 1;
    queue_callbacks=0;
    for(unsigned i=0;i<256;i++)if(uacpi_kernel_schedule_work(0,counted_work,0))goto failed;
    for(unsigned i=0;i<256;i++)if(uacpi_kernel_schedule_work(1,counted_work,0))goto failed;
    r4_host_stop();
    if(uacpi_kernel_schedule_work(0,counted_work,0)==UACPI_STATUS_OK)goto failed;
    if(r4_test_start(0)||r4_test_start(1))goto failed;
    r4_test_join();if(queue_callbacks!=512||allocations)return 1;

    if(r4_host_reset())return 1;
    inject_wait_failure=1;
    if(r4_test_start(0))goto failed;
    r4_test_join();if(!r4_host_fault()||allocations)return 1;
    printf("[ACPIPLAT] queue idle/no polling, enqueue at wait, burst, stop/drain, wait failure and event OOM PASS\n");
    return 0;
failed:
    r4_host_stop();r4_test_join();return 1;
}
#define CHECK(C) do {if(!(C)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#C);result=1;goto cleanup;}}while(0)
int r4_test(void) {
    if(queue_test())return 1;
    int result=0;tables(0);CHECK(start()==0);
    CHECK(wait_value(&ready_messages,1,5000));CHECK(wait_value(&capabilities,3,500));CHECK(wait_value(&lid,1,500));
    uacpi_u64 mode=0;CHECK(!uacpi_eval_simple_integer(0,"\\_SB.VGA0.MODE",&mode)&&mode==4);
    void *handle=0;CHECK(!uacpi_kernel_pci_device_open((uacpi_pci_address){0,0,0,0},&handle));
    CHECK(!uacpi_kernel_pci_write16(handle,2,0x1234));uacpi_u16 word=0;CHECK(!uacpi_kernel_pci_read16(handle,2,&word)&&word==0x1234);
    CHECK(uacpi_kernel_pci_read16(handle,4095,&word)==UACPI_STATUS_INVALID_ARGUMENT);
    CHECK(uacpi_kernel_pci_device_open((uacpi_pci_address){1,0,0,0},&handle)==UACPI_STATUS_UNIMPLEMENTED);
    guard();ec_ram[0]=0;unguard();event(1);event(2);event(3);event(4);event(5);
    CHECK(wait_value(&lid,2,2000));CHECK(wait_value(&up,1,2000));CHECK(wait_value(&down,1,2000));r4_sleep(100);
    guard();unsigned stored=ec_ram[1],mask=ports[0x406];unguard();CHECK(stored==0xA5&&(mask&4));
    // General AML GPE path uses the actual SCI status handler and deferred
    // execution/notification queues, independently of EC event polling.
    guard();ports[0x402]|=2;unguard();CHECK(wait_value(&up,2,2000));r4_sleep(100);
    guard();mask=ports[0x406];unsigned status=ports[0x402];unguard();CHECK((mask&2)&&!(status&2));
    guard();ec_ram[0]=1;unguard();event(1);CHECK(wait_value(&lid,1,2000));
    CHECK(!r4_host_fault());printf("[ACPIPLAT] AML EC READ/WRITE/QUERY, GPE/Notify, lid, brightness filtering, _DOS and ECAM PASS\n");
    // A timed-out EC transaction cannot be replayed against a late byte.
    guard();stuck=1;unsigned before=writes;unguard();event(1);
    CHECK(wait_value(&lid,0,3000));r4_sleep(30);
    guard();unsigned after=writes;unguard();CHECK(after==before);
    printf("[ACPIPLAT] EC timeout, no replay and lost lid source PASS\n");
cleanup:
    r4_platform_stop();r4_test_join();
    if(allocations||capabilities||lid||retired_messages!=1){fprintf(stderr,"FAIL shutdown allocations=%u capabilities=%u lid=%u retired=%u\n",allocations,capabilities,lid,retired_messages);return 1;}
    if(result)return result;
    tables(1);if(start())return 1;
    if(!wait_value(&retired_messages,1,3000)){r4_platform_stop();r4_test_join();return 1;}
    r4_test_join();
    if(allocations||ready_messages||capabilities||writes){fprintf(stderr,"FAIL malformed ECDT admitted IO or retained callbacks\n");return 1;}
    tables(0);if(start())return 1;
    if(!wait_value(&ready_messages,1,5000)){r4_platform_stop();r4_test_join();return 1;}
    r4_platform_stop();r4_test_join();
    if(allocations||capabilities||lid||r4_platform_shutdown_status())return 1;
    printf("[ACPIPLAT] shutdown/restart, exact callback retirement and malformed ECDT rejection PASS\n");
    tables(0);if(start())return 1;
    if(!wait_value(&ready_messages,1,5000)){r4_platform_stop();r4_test_join();return 1;}
    // Closed allocation admission cannot pretend to restore firmware policy.
    __atomic_store_n(&deny_allocations,1,__ATOMIC_RELEASE);
    r4_platform_stop();r4_test_join();
    __atomic_store_n(&deny_allocations,0,__ATOMIC_RELEASE);
    if(allocations||capabilities||lid||r4_platform_shutdown_status()!=-1)return 1;
    printf("[ACPIPLAT] failed _DOS restoration remains an unconfirmed handoff PASS\n");
    return 0;
}
