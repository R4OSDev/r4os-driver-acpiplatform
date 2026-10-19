// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
// ACPI6.4 chapter8 contracts. MIT uACPI owns evaluation. The existing platform
// worker owns every descriptor, register operation and restoration receipt.
#include "port.h"
#include "power.h"
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>
#include <uacpi/internal/stdlib.h>
#define memcpy uacpi_memcpy
#define memset uacpi_memset
#define memcmp uacpi_memcmp

enum { MAX_CPU=64, MAX_PSS=32, REGS=4, INVALID=-1, BUSY=-2, EPOCH=-4, UNSUPPORTED=-7, FAULT=-8 };
typedef struct { uint8_t space,bits,shift,bytes; uint64_t address; } Gas;
typedef struct { uint32_t mhz,power,latency,bus_latency,control,status; } Pstate;
typedef struct {
    uacpi_namespace_node *node;
    uint32_t uid,apic,kind,revision,domain,coord,members,count,invalid;
    uint64_t mask,identity;
    Gas control[REGS],status,caps[4],enable,autonomous;
    uint32_t values[4],caps_are_registers;
    Pstate states[MAX_PSS];
} Cpu;
typedef struct {
    uint64_t token,revision,epoch,mask;
    uint64_t baseline[MAX_CPU][REGS],last[MAX_CPU][REGS];
    uint32_t minimum,maximum,desired,epp,flags,uncertain,baseline_valid;
} Owned;
static Cpu cpus[MAX_CPU];
static Owned owners[MAX_CPU];
static unsigned lock,queue_state,closing,catalogue_ready,discovery_failed;
static uint64_t worker;
static R4PowerRequest queued;
static R4PowerInfo completion;
static int completion_result;
static uint64_t enter(void) {uint64_t f=r4_irq_disable();while(__atomic_exchange_n(&lock,1,__ATOMIC_ACQUIRE)){}return f;}
static void leave(uint64_t f) {__atomic_store_n(&lock,0,__ATOMIC_RELEASE);r4_irq_restore(f);}
static uint64_t hash(uint64_t h,uint64_t value) {for(unsigned n=0;n<8;n++){h^=(value>>(8*n))&255;h*=1099511628211ULL;}return h;}
static uint64_t fingerprint(uint64_t values[MAX_CPU][REGS],uint64_t mask) {
    uint64_t h=1469598103934665603ULL;
    for(unsigned c=0;c<MAX_CPU;c++)if(mask&(1ULL<<c))for(unsigned n=0;n<REGS;n++)h=hash(h,values[c][n]);
    return h?h:1;
}
static int equal(uint64_t a[MAX_CPU][REGS],uint64_t b[MAX_CPU][REGS],uint64_t mask) {
    for(unsigned c=0;c<MAX_CPU;c++)if(mask&(1ULL<<c))for(unsigned n=0;n<REGS;n++)if(a[c][n]!=b[c][n])return 0;
    return 1;
}
static uint64_t gas_identity(uint64_t h,Gas gas) {
    h=hash(h,gas.address);return hash(h,gas.space|((uint64_t)gas.bits<<8)|((uint64_t)gas.shift<<16)|((uint64_t)gas.bytes<<24));
}
static int integer(uacpi_object *object,uint32_t *out) {
    uint64_t v=0;if(uacpi_object_get_integer(object,&v)||v>UINT32_MAX)return INVALID;*out=(uint32_t)v;return 0;
}
static int package(uacpi_object *object,uacpi_object_array *out,unsigned count) {
    return uacpi_object_get_package(object,out)||out->count!=count?INVALID:0;
}
static int gas(uacpi_object *object,Gas *out,int optional) {
    uacpi_data_view view={0};if(uacpi_object_get_buffer(object,&view))return INVALID;
    const uint8_t *p=view.bytes;
    if(view.length!=17||p[0]!=0x82||p[1]!=12||p[2]!=0||p[15]!=0x79)return INVALID;
    if(p[16]){unsigned sum=0;for(unsigned n=0;n<17;n++)sum+=p[n];if(sum&255)return INVALID;}
    uint64_t address=0;for(unsigned n=0;n<8;n++)address|=(uint64_t)p[7+n]<<(8*n);
    if(!p[3]&&!p[4]&&!p[5]&&!p[6]&&!address){memset(out,0,sizeof(*out));return optional?0:UNSUPPORTED;}
    if(p[3]>1)return UNSUPPORTED; // PCC/FFH need a separately qualified transport.
    if(!address||!p[4]||p[4]>64||p[5]>=64||p[4]+p[5]>64||p[6]>4)return INVALID;
    unsigned bytes=p[6]?1U<<(p[6]-1):1;
    if(!p[6])while(bytes*8<p[4]+p[5])bytes*=2;
    if(bytes*8<p[4]+p[5]||(p[3]==1&&(bytes>4||address>65536-bytes))||
       address>UINT64_MAX-bytes||(p[3]==0&&(address&(bytes-1))))return INVALID;
    *out=(Gas){p[3],p[4],p[5],bytes,address};return 0;
}
static uint64_t bits(Gas g) {return g.bits==64?UINT64_MAX:(1ULL<<g.bits)-1;}
static int raw_read(Gas g,uint64_t *value) {
    if(!g.bytes)return UNSUPPORTED;
    if(g.space==1){*value=r4_in((uint16_t)g.address,g.bytes);return 0;}
    volatile void *p=r4_view(g.address,g.bytes);if(!p)return FAULT;
    switch(g.bytes){case 1:*value=*(volatile uint8_t*)p;break;case 2:*value=*(volatile uint16_t*)p;break;
      case 4:*value=*(volatile uint32_t*)p;break;case 8:*value=*(volatile uint64_t*)p;break;default:return INVALID;}
    return 0;
}
static int field_read(Gas g,uint32_t *value) {
    uint64_t v;int rc=raw_read(g,&v);if(rc)return rc;v=(v>>g.shift)&bits(g);if(v>UINT32_MAX)return INVALID;*value=(uint32_t)v;return 0;
}
static int raw_write(Gas g,uint64_t expected,uint64_t value) {
    uint64_t current;int rc=raw_read(g,&current);if(rc)return rc;if(current!=expected)return BUSY;
    if(g.space==1)r4_out((uint16_t)g.address,g.bytes,(uint32_t)value);
    else {
        volatile void *p=r4_view(g.address,g.bytes);if(!p)return FAULT;
        switch(g.bytes){case 1:*(volatile uint8_t*)p=(uint8_t)value;break;case 2:*(volatile uint16_t*)p=(uint16_t)value;break;
          case 4:*(volatile uint32_t*)p=(uint32_t)value;break;case 8:*(volatile uint64_t*)p=value;break;default:return INVALID;}
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rc=raw_read(g,&current);return rc?rc:current==value?0:FAULT;
}
static int overlap(Gas a,Gas b) {
    return a.bytes&&b.bytes&&a.space==b.space&&a.address<b.address+b.bytes&&b.address<a.address+a.bytes;
}
static int same(Gas a,Gas b) {return a.space==b.space&&a.bits==b.bits&&a.shift==b.shift&&a.bytes==b.bytes&&a.address==b.address;}
static int cppc(Cpu *cpu,uacpi_object *object) {
    uacpi_object_array a={0};if(uacpi_object_get_package(object,&a)||a.count<2)return INVALID;
    uint32_t count,revision;if(integer(a.objects[0],&count)||integer(a.objects[1],&revision)||a.count!=count||
       !((revision==1&&count==17)||(revision==2&&count==21)||(revision==3&&count==23)))return INVALID;
    cpu->kind=1;cpu->revision=revision;
    for(unsigned n=2;n<count;n++) {
        if(n==12||n>=20){uint32_t value;if(integer(a.objects[n],&value))return INVALID;continue;}
        if(n<6||n==17){if(uacpi_object_get_type(a.objects[n])==UACPI_OBJECT_INTEGER){uint32_t value;if(integer(a.objects[n],&value))return INVALID;continue;}}
        Gas descriptor;int rc=gas(a.objects[n],&descriptor,1);if(rc)return rc;
    }
    for(unsigned n=0;n<4;n++) {
        if(uacpi_object_get_type(a.objects[n+2])==UACPI_OBJECT_INTEGER) {
            if(integer(a.objects[n+2],&cpu->values[n]))return INVALID;
        } else {int rc=gas(a.objects[n+2],&cpu->caps[n],0);if(rc)return rc;cpu->caps_are_registers|=1U<<n;}
    }
    // Initial reversible profile: explicit min/max/desired with an already
    // autonomous interface. Never activate a reset-only interface implicitly.
    static const unsigned indexes[]={8,9,7,19};
    for(unsigned n=0;n<REGS;n++) {
        if(indexes[n]>=count){if(n==3)break;return UNSUPPORTED;}
        int rc=gas(a.objects[indexes[n]],&cpu->control[n],n==3);if(rc)return rc;
        if(cpu->control[n].bits>32)return INVALID;
        for(unsigned j=0;j<n;j++)if(overlap(cpu->control[n],cpu->control[j]))return UNSUPPORTED;
    }
    int rc=gas(a.objects[16],&cpu->enable,1);if(rc)return rc;
    if(count<=17)return UNSUPPORTED;
    if(uacpi_object_get_type(a.objects[17])==UACPI_OBJECT_INTEGER) {
        uint32_t value;if(integer(a.objects[17],&value)||value!=1)return UNSUPPORTED;
    } else {rc=gas(a.objects[17],&cpu->autonomous,0);if(rc)return rc;}
    return 0;
}
static int pss(Cpu *cpu,uacpi_object *object) {
    uacpi_object_array a={0};if(uacpi_object_get_package(object,&a)||!a.count||a.count>MAX_PSS)return INVALID;
    cpu->kind=2;cpu->count=(unsigned)a.count;
    for(unsigned n=0;n<a.count;n++) {
        uacpi_object_array state={0};if(package(a.objects[n],&state,6))return INVALID;
        uint32_t p[6];for(unsigned j=0;j<6;j++)if(integer(state.objects[j],&p[j]))return INVALID;
        cpu->states[n]=(Pstate){p[0],p[1],p[2],p[3],p[4],p[5]};
        if(!p[0]||p[2]>1000000||p[3]>1000000||(n&&p[0]>=cpu->states[n-1].mhz))return INVALID;
        for(unsigned j=0;j<n;j++)if(cpu->states[j].control==p[4]||cpu->states[j].status==p[5])return INVALID;
    }
    uacpi_object *pct=0;uacpi_status status=uacpi_eval(cpu->node,"_PCT",0,&pct);if(status)return status==UACPI_STATUS_NOT_FOUND?UNSUPPORTED:INVALID;
    uacpi_object_array regs={0};int rc=package(pct,&regs,2);
    if(!rc)rc=gas(regs.objects[0],&cpu->control[0],0);
    if(!rc)rc=gas(regs.objects[1],&cpu->status,0);
    if(!rc&&(cpu->control[0].bits>32||cpu->status.bits>32))rc=INVALID;
    for(unsigned n=0;!rc&&n<cpu->count;n++)if(cpu->states[n].control>bits(cpu->control[0])||cpu->states[n].status>bits(cpu->status))rc=INVALID;
    uacpi_object_unref(pct);return rc;
}
static int psd(Cpu *cpu) {
    uacpi_namespace_node *method=0;uacpi_status status=uacpi_namespace_node_find(cpu->node,"_PSD",&method);
    if(status==UACPI_STATUS_NOT_FOUND){cpu->members=1;return 0;}if(status)return INVALID;
    uacpi_object *obj=0;status=uacpi_eval(method,0,0,&obj);if(status)return INVALID;
    uacpi_object_array a={0},b={0};int rc=package(obj,&a,1);uint32_t v[5]={0};
    if(!rc)rc=package(a.objects[0],&b,5);
    for(unsigned n=0;!rc&&n<5;n++)rc=integer(b.objects[n],&v[n]);
    if(!rc&&(v[0]!=5||v[1]||v[3]<0xfc||v[3]>0xfe||!v[4]||v[4]>MAX_CPU))rc=INVALID;
    if(!rc){cpu->domain=v[2];cpu->coord=v[3];cpu->members=v[4];}
    uacpi_object_unref(obj);return rc;
}
static int map_uid(uint32_t uid,uint32_t *apic) {
    uacpi_table table;uacpi_status status=uacpi_table_find_by_signature("APIC",&table);if(status)return UNSUPPORTED;
    const uint8_t *p=table.ptr;size_t n=table.hdr->length;int found=0,rc=0;
    if(n<44)rc=INVALID;
    for(size_t offset=44;!rc&&offset<n;) {
        if(n-offset<2||p[offset+1]<2||p[offset+1]>n-offset){rc=INVALID;break;}
        uint32_t id=0,u=0,flags=0;unsigned type=p[offset],length=p[offset+1];
        if(type==0) {if(length!=8){rc=INVALID;break;}u=p[offset+2];id=p[offset+3];memcpy(&flags,p+offset+4,4);}
        else if(type==9){if(length!=16){rc=INVALID;break;}memcpy(&id,p+offset+4,4);memcpy(&flags,p+offset+8,4);memcpy(&u,p+offset+12,4);}
        if((type==0||type==9)&&(flags&1)&&u==uid){if(found++){rc=INVALID;break;}*apic=id;}
        offset+=length;
    }
    uacpi_table_unref(&table);return rc?rc:found?0:UNSUPPORTED;
}
static uacpi_iteration_decision discover(void *context,uacpi_namespace_node *node,uacpi_u32 depth) {
    (void)context;(void)depth;uacpi_object_type type;uint32_t uid,apic,cpu=MAX_CPU,sta;
    if(uacpi_namespace_node_type(node,&type))return UACPI_ITERATION_DECISION_CONTINUE;
    if(type==UACPI_OBJECT_DEVICE) {
        static const char *const ids[]={"ACPI0007",0};
        if(!uacpi_device_matches_pnp_id(node,ids))return UACPI_ITERATION_DECISION_CONTINUE;
        uint64_t value;if(uacpi_eval_simple_integer(node,"_UID",&value)||value>UINT32_MAX)return UACPI_ITERATION_DECISION_CONTINUE;uid=(uint32_t)value;
    } else {
        uacpi_object *obj=0;uacpi_processor_info info;
        if(uacpi_eval(node,0,0,&obj))return UACPI_ITERATION_DECISION_CONTINUE;
        uacpi_status status=uacpi_object_get_processor_info(obj,&info);uacpi_object_unref(obj);
        if(status)return UACPI_ITERATION_DECISION_CONTINUE;uid=info.id;
    }
    if(uacpi_eval_sta(node,&sta)||!(sta&1)||map_uid(uid,&apic))return UACPI_ITERATION_DECISION_CONTINUE;
    for(unsigned c=0;c<MAX_CPU;c++){uint32_t id;uint64_t epoch;if(!r4_power_cpu(c,&id,&epoch)&&id==apic){if(cpu!=MAX_CPU)return UACPI_ITERATION_DECISION_CONTINUE;cpu=c;}}
    if(cpu==MAX_CPU)return UACPI_ITERATION_DECISION_CONTINUE;
    if(cpus[cpu].node){cpus[cpu].invalid=1;return UACPI_ITERATION_DECISION_CONTINUE;}
    Cpu *entry=&cpus[cpu];entry->node=node;entry->uid=uid;entry->apic=apic;
    uacpi_object *obj=0;uacpi_namespace_node *method=0;
    uacpi_status status=uacpi_namespace_node_find(node,"_CPC",&method);int rc=0;
    if(!status){entry->kind=1;status=uacpi_eval(method,0,0,&obj);if(status)rc=INVALID;else{rc=cppc(entry,obj);uacpi_object_unref(obj);}}
    else if(status==UACPI_STATUS_NOT_FOUND) {
        status=uacpi_eval(node,"_PSS",0,&obj);
        if(!status){rc=pss(entry,obj);uacpi_object_unref(obj);}else rc=status==UACPI_STATUS_NOT_FOUND?UNSUPPORTED:INVALID;
    } else {rc=INVALID;discovery_failed=1;}
    if(!rc)rc=psd(entry);
    entry->invalid=rc!=0;return UACPI_ITERATION_DECISION_CONTINUE;
}
void r4_power_discover(void) {
    worker=r4_thread();
    uacpi_status status=uacpi_namespace_for_each_child(uacpi_namespace_root(),discover,0,UACPI_OBJECT_DEVICE_BIT|UACPI_OBJECT_PROCESSOR_BIT,UACPI_MAX_DEPTH_ANY,0);
    if(status||discovery_failed){for(unsigned c=0;c<MAX_CPU;c++)cpus[c].invalid=1;}
    unsigned has_cppc=0,has_pss=0;
    for(unsigned c=0;c<MAX_CPU;c++)if(cpus[c].node){has_cppc|=cpus[c].kind==1;has_pss|=cpus[c].kind==2;}
    for(unsigned c=0;c<MAX_CPU;c++) {
        Cpu *cpu=&cpus[c];if(!cpu->node||cpu->invalid)continue;
        if(has_cppc&&has_pss){cpu->invalid=1;continue;}
        uint64_t mask=0;
        for(unsigned j=0;j<MAX_CPU;j++)if(j==c||(cpu->coord&&cpus[j].coord&&cpu->domain==cpus[j].domain)) {
            Cpu *other=&cpus[j];uint32_t apic;uint64_t epoch;
            if(other->invalid||other->kind!=cpu->kind||other->coord!=cpu->coord||other->members!=cpu->members||
               r4_power_cpu(j,&apic,&epoch)||apic!=other->apic){cpu->invalid=1;break;}
            mask|=1ULL<<j;
            if(j!=c) {
                if(cpu->count!=other->count||cpu->revision!=other->revision||cpu->caps_are_registers!=other->caps_are_registers||
                   memcmp(cpu->values,other->values,sizeof(cpu->values))||memcmp(cpu->states,other->states,sizeof(cpu->states)))cpu->invalid=1;
                for(unsigned a=0;a<REGS;a++)for(unsigned b=0;b<REGS;b++) {
                    if(cpu->coord==0xfd) {if(a==b&&!same(cpu->control[a],other->control[b]))cpu->invalid=1;}
                    else if(overlap(cpu->control[a],other->control[b]))cpu->invalid=1;
                }
            }
        }
        if(__builtin_popcountll(mask)!=cpu->members)cpu->invalid=1;
        cpu->mask=mask;
        uint64_t identity=hash(hash(hash(1469598103934665603ULL,cpu->uid),cpu->apic),mask);
        identity=hash(hash(hash(identity,cpu->kind),cpu->revision),cpu->domain);identity=hash(hash(identity,cpu->coord),cpu->members);
        for(unsigned n=0;n<REGS;n++){identity=gas_identity(identity,cpu->control[n]);identity=gas_identity(identity,cpu->caps[n]);identity=hash(identity,cpu->values[n]);}
        identity=gas_identity(gas_identity(gas_identity(identity,cpu->status),cpu->enable),cpu->autonomous);
        for(unsigned n=0;n<cpu->count;n++){Pstate p=cpu->states[n];identity=hash(hash(hash(hash(hash(hash(identity,p.mhz),p.power),p.latency),p.bus_latency),p.control),p.status);}
        cpu->identity=identity?identity:1;
    }
    // Different domains must never alias a writable register. Only an exact
    // SW_ANY shared descriptor within the same validated domain may do so.
    for(unsigned c=0;c<MAX_CPU;c++)for(unsigned j=c+1;j<MAX_CPU;j++)if(cpus[c].node&&cpus[j].node) {
        for(unsigned a=0;a<REGS;a++)for(unsigned b=0;b<REGS;b++)if(overlap(cpus[c].control[a],cpus[j].control[b])&&
          !(cpus[c].coord==0xfd&&cpus[j].coord==0xfd&&cpus[c].mask==cpus[j].mask&&a==b&&same(cpus[c].control[a],cpus[j].control[b])))cpus[c].invalid=cpus[j].invalid=1;
    }
    if(has_cppc) {
        unsigned complete=1;
        for(unsigned c=0;c<MAX_CPU;c++){uint32_t id;uint64_t epoch;if(!r4_power_cpu(c,&id,&epoch)&&(!cpus[c].node||cpus[c].kind!=1||cpus[c].invalid))complete=0;}
        if(!complete)for(unsigned c=0;c<MAX_CPU;c++)if(cpus[c].kind==1)cpus[c].invalid=1;
    }
    __atomic_store_n(&catalogue_ready,1,__ATOMIC_RELEASE);
}
static int capabilities(Cpu *cpu,R4PowerInfo *info) {
    uint32_t v[4];for(unsigned n=0;n<4;n++){v[n]=cpu->values[n];if(cpu->caps_are_registers&(1U<<n)){int rc=field_read(cpu->caps[n],&v[n]);if(rc)return rc;}}
    if(cpu->kind==1) {
        if(!v[3]||v[3]>v[2]||v[2]>v[1]||v[1]>v[0]||v[0]>UINT16_MAX)return INVALID;
        for(unsigned n=0;n<3;n++)if(v[0]>bits(cpu->control[n]))return INVALID;
        info->lowest=v[3];info->highest=v[0];info->nominal=v[1];info->efficient=v[2];
        uint32_t enabled=1;int rc=0;
        if(cpu->enable.bytes)rc=field_read(cpu->enable,&enabled);
        if(rc)return rc;if(enabled!=1)return UNSUPPORTED;
        if(cpu->autonomous.bytes){rc=field_read(cpu->autonomous,&enabled);if(rc)return rc;if(enabled!=1)return UNSUPPORTED;}
        if(cpu->control[3].bytes){if(bits(cpu->control[3])<255)return INVALID;info->flags|=1;info->valid_mask|=8;}
    } else {info->lowest=1;info->highest=cpu->count;info->nominal=cpu->count;}
    info->valid_mask|=1;return 0;
}
static int capture(uint64_t mask,uint64_t values[MAX_CPU][REGS]) {
    for(unsigned c=0;c<MAX_CPU;c++)if(mask&(1ULL<<c))for(unsigned n=0;n<REGS;n++) {
        values[c][n]=0;if(cpus[c].control[n].bytes){int rc=raw_read(cpus[c].control[n],&values[c][n]);if(rc)return rc;}
    }
    return 0;
}
static unsigned leader(uint64_t mask) {return mask?(unsigned)__builtin_ctzll(mask):MAX_CPU;}
static void decode_controls(unsigned c,uint64_t values[MAX_CPU][REGS],R4PowerInfo *info) {
    Cpu *cpu=&cpus[c];
    if(cpu->kind==1) {
        info->minimum=(uint32_t)((values[c][0]>>cpu->control[0].shift)&bits(cpu->control[0]));
        info->maximum=(uint32_t)((values[c][1]>>cpu->control[1].shift)&bits(cpu->control[1]));
        info->desired=(uint32_t)((values[c][2]>>cpu->control[2].shift)&bits(cpu->control[2]));
        if(cpu->control[3].bytes){info->epp=(uint32_t)((values[c][3]>>cpu->control[3].shift)&bits(cpu->control[3]));info->valid_mask|=8;info->flags|=1;}
    } else {
        uint32_t control=(uint32_t)((values[c][0]>>cpu->control[0].shift)&bits(cpu->control[0]));
        for(unsigned n=0;n<cpu->count;n++)if(cpu->states[n].control==control){info->desired=cpu->count-n;return;}
        info->valid_mask&=~2U;
    }
}
static int status_match(Cpu *cpu,uint32_t control) {
    unsigned n;for(n=0;n<cpu->count;n++)if(cpu->states[n].control==control)break;
    if(n==cpu->count)return UNSUPPORTED;
    uint64_t duration=(uint64_t)cpu->states[n].latency*1000+10000000,end=r4_now()+duration;
    do {uint32_t value;int rc=field_read(cpu->status,&value);if(rc)return rc;if(value==cpu->states[n].status)return 0;r4_sleep(1);}while(r4_now()<end);
    return FAULT;
}
static int validate_domain(Cpu *cpu,R4PowerInfo *info) {
    for(unsigned c=0;c<MAX_CPU;c++)if(cpu->mask&(1ULL<<c)) {
        uint32_t apic;uint64_t epoch;if(r4_power_cpu(c,&apic,&epoch)||apic!=cpus[c].apic||__atomic_load_n(&cpus[c].invalid,__ATOMIC_ACQUIRE))return UNSUPPORTED;
        R4PowerInfo peer={0};int rc=capabilities(&cpus[c],&peer);if(rc)return rc;
        if(peer.lowest!=info->lowest||peer.highest!=info->highest||peer.nominal!=info->nominal||peer.efficient!=info->efficient||peer.flags!=info->flags)return UNSUPPORTED;
    }
    return 0;
}
static void execute(const R4PowerRequest *request,R4PowerInfo *info) {
    unsigned c=request->cpu_index;Cpu *cpu=&cpus[c];Owned *own=&owners[c];
    *info=(R4PowerInfo){.version=1,.size=192,.cpu_index=c,.epoch=request->epoch,.captured_ns=r4_now(),.valid_mask=4};
    if(!cpu->node||!cpu->mask)return;
    info->backend=cpu->kind;info->coordination=cpu->coord;info->domain_mask=cpu->mask;info->domain_id=cpu->domain;
    info->domain_cpus=cpu->members;info->pstate_count=cpu->count;info->package_revision=cpu->revision;info->descriptor_identity=cpu->identity;info->valid_mask|=16;
    if(!own->token)for(unsigned j=0;j<MAX_CPU;j++)if(cpu->mask&(1ULL<<j)) {
        uint32_t id;uint64_t current_epoch;
        if(r4_power_cpu(j,&id,&current_epoch)||id!=cpus[j].apic||__atomic_load_n(&cpus[j].invalid,__ATOMIC_ACQUIRE)) {
            info->valid_mask&=~16U;info->last_error=UNSUPPORTED;return;
        }
    }
    for(unsigned n=0;n<REGS;n++)if(cpu->control[n].bytes)info->transport_spaces|=1U<<cpu->control[n].space;
    // One explicit leader controls a whole validated domain. Followers remain
    // passive; the service expands only a leader's confirmed scope receipt.
    if(c!=leader(cpu->mask))return;
    uint32_t apic;uint64_t epoch;int rc=r4_power_cpu(c,&apic,&epoch);
    if(rc||epoch!=request->epoch||apic!=cpu->apic){info->last_error=EPOCH;return;}
    info->lease=own->token;info->revision=own->revision;
    if(own->token){info->baseline_request=fingerprint(own->baseline,own->mask);info->last_written=fingerprint(own->last,own->mask);info->state=own->uncertain?5:3;}
    if(request->operation!=2&&__atomic_load_n(&cpu->invalid,__ATOMIC_ACQUIRE)) {
        if(own->token)info->state=5;else info->state=0;
        info->last_error=UNSUPPORTED;return;
    }
    // Return is sensor-independent: no _CPC/_PPC evaluation or optional
    // capability read may prevent restoration of an exact retained baseline.
    if(request->operation==2) {
        if(!own->token||request->lease!=own->token||request->expected_revision!=own->revision||own->epoch!=request->epoch){info->last_error=EPOCH;return;}
        if(!own->baseline_valid) {
            uint64_t returned=own->token;rc=r4_power_control(2,c,own->epoch,own->mask,own->token,&returned);
            if(rc||returned){if(!rc)rc=BUSY;goto failed;}
            memset(own,0,sizeof(*own));info->lease=0;info->revision=0;info->state=4;info->flags|=4;return;
        }
        for(unsigned j=0;j<MAX_CPU;j++)if(own->mask&(1ULL<<j)) {
            if(cpu->coord==0xfd&&j!=c)continue;
            unsigned order[REGS]={0,1,2,3};
            if(cpu->kind==1){uint32_t maximum;rc=field_read(cpus[j].control[1],&maximum);if(rc)goto failed;
                if(((own->baseline[j][0]>>cpus[j].control[0].shift)&bits(cpus[j].control[0]))>maximum){order[0]=1;order[1]=0;}}
            for(unsigned k=0;k<REGS;k++){unsigned n=order[k];if(cpus[j].control[n].bytes) {
                uint64_t current;rc=raw_read(cpus[j].control[n],&current);if(rc)goto failed;
                if(current==own->baseline[j][n])continue;
                if(current!=own->last[j][n]){rc=BUSY;goto failed;}
                rc=raw_write(cpus[j].control[n],current,own->baseline[j][n]);if(rc)goto failed;
            }}
        }
        // SW_ALL hardware may not acknowledge any transition until every
        // member has received its request. Restore all, then await all statuses.
        if(cpu->kind==2)for(unsigned j=0;j<MAX_CPU;j++)if(own->mask&(1ULL<<j)) {
            uint32_t control=(uint32_t)((own->baseline[j][0]>>cpus[j].control[0].shift)&bits(cpus[j].control[0]));rc=status_match(&cpus[j],control);if(rc)goto failed;
        }
        {uint64_t values[MAX_CPU][REGS];rc=capture(own->mask,values);if(rc)goto failed;
         if(!equal(values,own->baseline,own->mask)){rc=BUSY;goto failed;}}
        {uint64_t returned=own->token;rc=r4_power_control(2,c,own->epoch,own->mask,own->token,&returned);if(rc||returned){if(!rc)rc=BUSY;goto failed;}}
        info->valid_mask|=2;decode_controls(c,own->baseline,info);
        memset(own,0,sizeof(*own));info->lease=0;info->revision=0;info->flags|=4;info->state=4;info->observed_request=info->baseline_request;return;
    }
    rc=capabilities(cpu,info);if(rc){if(!own->token)info->state=rc==UNSUPPORTED?1:0;info->last_error=rc;return;}
    if(!own->token)info->state=2;
    uint64_t current[MAX_CPU][REGS];rc=capture(cpu->mask,current);if(rc)goto failed;
    info->observed_request=fingerprint(current,cpu->mask);info->valid_mask|=2;
    decode_controls(c,current,info);
    if(own->token) {
        if(cpu->kind==2){info->minimum=own->minimum;info->maximum=own->maximum;}
        if(!equal(current,own->last,own->mask)){info->state=6;info->last_error=BUSY;return;}
    }
    if(request->operation==0)return;
    if(request->operation!=1){info->last_error=UNSUPPORTED;return;}
    if(__atomic_load_n(&closing,__ATOMIC_ACQUIRE)){info->last_error=BUSY;return;}
    if((request->flags&~1U)||request->epp>255||(!(request->flags&1)&&request->epp)||
       ((request->flags&1)&&!(info->flags&1))||request->minimum<info->lowest||request->maximum>info->highest||
       request->minimum>request->maximum||(request->desired&&(request->desired<request->minimum||request->desired>request->maximum))||
       (cpu->kind==2&&(!request->desired||request->flags))){info->last_error=INVALID;return;}
    rc=validate_domain(cpu,info);if(rc){info->last_error=rc;return;}
    if(cpu->kind==2)for(unsigned j=0;j<MAX_CPU;j++)if(cpu->mask&(1ULL<<j)) {
        uint64_t ppc=0;uacpi_namespace_node *method=0;uacpi_status s=uacpi_namespace_node_find(cpus[j].node,"_PPC",&method);
        if(!s){s=uacpi_eval_simple_integer(method,0,&ppc);if(s){info->last_error=UNSUPPORTED;return;}}
        else if(s!=UACPI_STATUS_NOT_FOUND){info->last_error=UNSUPPORTED;return;}
        if(ppc>=cpu->count||request->desired>cpu->count-ppc){info->last_error=UNSUPPORTED;return;}
        rc=status_match(&cpus[j],(uint32_t)((current[j][0]>>cpus[j].control[0].shift)&bits(cpus[j].control[0])));if(rc){info->last_error=rc;return;}
    }
    if(own->token) {
        if(request->lease!=own->token||request->expected_revision!=own->revision||own->epoch!=request->epoch||own->revision==UINT64_MAX){info->last_error=EPOCH;return;}
    } else {
        if(request->lease||request->expected_revision){info->last_error=EPOCH;return;}
        uint64_t token=0;rc=r4_power_control(1,c,epoch,cpu->mask,0,&token);if(rc||!token){info->last_error=rc?rc:BUSY;return;}
        own->token=token;own->epoch=epoch;own->mask=cpu->mask;
        // Establish the real original state AFTER shared direct/firmware
        // ownership admission. A preceding observation is not a baseline.
        rc=capture(cpu->mask,current);
        if(!rc&&cpu->kind==2)for(unsigned j=0;j<MAX_CPU;j++)if(cpu->mask&(1ULL<<j)) {
            rc=status_match(&cpus[j],(uint32_t)((current[j][0]>>cpus[j].control[0].shift)&bits(cpus[j].control[0])));if(rc)break;
        }
        if(rc) {
            // No write attempted: returning this new admission needs no
            // invented register restoration, even if the sensor is unreadable.
            uint64_t returned=token;int release=r4_power_control(2,c,epoch,cpu->mask,token,&returned);
            if(!release&&!returned)memset(own,0,sizeof(*own));
            else {own->uncertain=1;info->lease=token;info->state=5;}
            info->last_error=release?release:rc;return;
        }
        memcpy(own->baseline,current,sizeof(current));memcpy(own->last,current,sizeof(current));own->baseline_valid=1;
    }
    own->revision++;own->uncertain=1;own->minimum=request->minimum;own->maximum=request->maximum;own->desired=request->desired;own->epp=request->epp;own->flags=request->flags;
    info->lease=own->token;info->revision=own->revision;info->baseline_request=fingerprint(own->baseline,own->mask);
    // Validate every intended field before the first write. Preserve complete
    // original storage words, including unrelated bits and reset values.
    for(unsigned j=0;j<MAX_CPU;j++)if(own->mask&(1ULL<<j))for(unsigned n=0;n<REGS;n++)if(cpus[j].control[n].bytes) {
        Gas g=cpus[j].control[n];uint64_t field;
        if(cpu->kind==2)field=cpu->states[cpu->count-request->desired].control;
        else field=n==0?request->minimum:n==1?request->maximum:n==2?request->desired:request->epp;
        if(n==3&&!(request->flags&1))continue;
        own->last[j][n]=(current[j][n]&~(bits(g)<<g.shift))|((field&bits(g))<<g.shift);
    }
    info->last_written=fingerprint(own->last,own->mask);
    if(cpu->kind==1&&cpu->control[3].bytes)own->epp=(uint32_t)((own->last[c][3]>>cpu->control[3].shift)&bits(cpu->control[3]));
    for(unsigned j=0;j<MAX_CPU;j++)if(own->mask&(1ULL<<j)) {
        if(cpu->coord==0xfd&&j!=c)continue;
        unsigned order[REGS]={0,1,2,3};
        // Widen maximum before raising minimum past the old maximum.
        if(cpu->kind==1&&request->minimum>((current[j][1]>>cpus[j].control[1].shift)&bits(cpus[j].control[1]))){order[0]=1;order[1]=0;}
        for(unsigned k=0;k<REGS;k++){unsigned n=order[k];if(cpus[j].control[n].bytes&&current[j][n]!=own->last[j][n]){rc=raw_write(cpus[j].control[n],current[j][n],own->last[j][n]);if(rc)goto failed;}}
    }
    // Initiate all coordinated transitions before observing completion.
    if(cpu->kind==2)for(unsigned j=0;j<MAX_CPU;j++)if(own->mask&(1ULL<<j)) {
        rc=status_match(&cpus[j],cpu->states[cpu->count-request->desired].control);if(rc)goto failed;
    }
    rc=capture(own->mask,current);if(rc)goto failed;
    info->observed_request=fingerprint(current,own->mask);
    if(!equal(current,own->last,own->mask)){rc=FAULT;goto failed;}
    own->uncertain=0;info->state=3;info->minimum=request->minimum;info->maximum=request->maximum;info->desired=request->desired;info->epp=own->epp;return;
failed:
    if(own->token){own->uncertain=1;info->state=rc==BUSY?6:5;info->lease=own->token;info->revision=own->revision;info->baseline_request=fingerprint(own->baseline,own->mask);info->last_written=fingerprint(own->last,own->mask);}
    info->last_error=rc;
}
void r4_power_reset(void) {
    memset(cpus,0,sizeof(cpus));memset(owners,0,sizeof(owners));lock=queue_state=closing=catalogue_ready=discovery_failed=0;worker=0;
}
void r4_power_stop(void) {__atomic_store_n(&closing,1,__ATOMIC_RELEASE);r4_host_wake(3);}
int r4_power_idle(void) {
    for(unsigned n=0;n<MAX_CPU;n++)if(owners[n].token)return 0;
    uint64_t f=enter();int idle=queue_state==0;leave(f);return idle;
}
int r4_power_finish(void) {
    uint64_t f=enter();
    if(queue_state){leave(f);return 0;}
    __atomic_store_n(&catalogue_ready,0,__ATOMIC_RELEASE);leave(f);return 1;
}
void r4_power_notify(uacpi_namespace_node *node,uint64_t value) {
    if(value!=0x80&&value!=0x82&&value!=0x85)return;
    for(unsigned c=0;c<MAX_CPU;c++)if(cpus[c].node==node) {
        // Immutable catalogue is withdrawn until module restart. Exact owned
        // baseline stays available for sensor-independent return.
        for(unsigned j=0;j<MAX_CPU;j++)if(cpus[c].mask&(1ULL<<j))__atomic_store_n(&cpus[j].invalid,1,__ATOMIC_RELEASE);
    }
}
void r4_power_poll(void) {
    uint64_t f=enter();if(queue_state!=1){leave(f);return;}queue_state=2;R4PowerRequest request=queued;leave(f);
    R4PowerInfo info;execute(&request,&info);
    f=enter();completion=info;completion_result=0;queue_state=3;leave(f);
}
int r4_power_call(const R4PowerRequest *request,R4PowerInfo *out) {
    if(!request||!out||request->version!=1||request->size<64||out->version!=1||out->size<192||request->cpu_index>=MAX_CPU||request->reserved0)return INVALID;
    if(!__atomic_load_n(&catalogue_ready,__ATOMIC_ACQUIRE)||r4_thread()==worker)return BUSY;
    if(request->operation>3||(request->operation!=1&&(request->minimum||request->maximum||request->desired||request->epp||request->flags))||
       (request->operation==0&&(request->lease||request->expected_revision)))return INVALID;
    uint64_t deadline=r4_now()+2000000000ULL,f;
    for(;;) {
        f=enter();
        if(!__atomic_load_n(&catalogue_ready,__ATOMIC_ACQUIRE)||(__atomic_load_n(&closing,__ATOMIC_ACQUIRE)&&request->operation!=2)){leave(f);return BUSY;}
        if(!queue_state)break;
        leave(f);if(r4_now()>=deadline)return BUSY;r4_power_pause(1);
    }
    queued=*request;queue_state=1;leave(f);r4_host_wake(3);
    // No timeout may free this resident cell or the caller's pinned callback
    // while AML still owns it. Individual AML loops/host waits remain bounded.
    for(;;){f=enter();if(queue_state==3){*out=completion;int result=completion_result;queue_state=0;leave(f);r4_host_wake(3);return result;}leave(f);r4_power_pause(1);}
}
