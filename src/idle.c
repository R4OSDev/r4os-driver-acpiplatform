// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
// ACPI6.5 chapter8 and Intel vendor ACPI section3.3. Only the ordinary ACPI
// worker evaluates objects. There is no register/instruction access here.
#include "port.h"
#include "power.h"
#include "idle.h"
#include <uacpi/utilities.h>
#include <uacpi/internal/stdlib.h>
enum { ABSENT=1,MALFORMED=2,COORDINATION=3,LPI_CONTEXT=4,ENTRY=5,WITHDRAWN=6,MAX_CPUS=64,MAX_STATES=32 };
typedef struct { uint8_t space,width,offset,access; uint64_t address; } Register;
typedef struct { uacpi_namespace_node *node; R4IdleDescriptor value; unsigned cst_present,csd_dependency; } Cpu;
static Cpu cpus[MAX_CPUS];
static unsigned withdrawal,complete,drained;
static int integer(uacpi_object *o,uint32_t *v) {uint64_t n;if(uacpi_object_get_integer(o,&n)||n>UINT32_MAX)return -1;*v=(uint32_t)n;return 0;}
static uint64_t hash(uint64_t h,uint64_t v) {for(unsigned n=0;n<8;n++){h^=(v>>(8*n))&255;h*=1099511628211ULL;}return h;}
static int reg(uacpi_object *o,Register *r) {
    uacpi_data_view v={0};if(uacpi_object_get_buffer(o,&v)||v.length!=17)return -1;
    const uint8_t *p=v.bytes;
    if(p[0]!=0x82||p[1]!=12||p[2]!=0||p[15]!=0x79)return -1;
    if(p[16]){unsigned sum=0;for(unsigned n=0;n<17;n++)sum+=p[n];if(sum&255)return -1;}
    uint64_t a=0;for(unsigned n=0;n<8;n++)a|=(uint64_t)p[n+7]<<(8*n);
    if(p[3]>10&&p[3]!=0x7f)return -1;
    *r=(Register){p[3],p[4],p[5],p[6],a};return 0;
}
static int eval(uacpi_namespace_node *n,const char *name,uacpi_object **out,int *present) {
    uacpi_namespace_node *method=0;uacpi_status rc=uacpi_namespace_node_find(n,name,&method);
    *present=rc!=UACPI_STATUS_NOT_FOUND;if(rc)return rc==UACPI_STATUS_NOT_FOUND?1:-1;
    return uacpi_eval(method,0,0,out)?-1:0;
}
static int cst(Cpu *cpu) {
    uacpi_object *o=0;int present=0,rc=eval(cpu->node,"_CST",&o,&present);
    cpu->cst_present=present;if(rc)return rc==1?ABSENT:MALFORMED;
    uacpi_object_array p;uint32_t count;int result=MALFORMED;
    if(uacpi_object_get_package(o,&p)||p.count<2||integer(p.objects[0],&count)||!count||count>MAX_STATES||p.count!=count+1)goto done;
    Register registers[MAX_STATES];unsigned previous=0;int found=0;
    cpu->value.state_count=count;cpu->value.source=1;
    for(unsigned n=0;n<count;n++) {
        uacpi_object_array entry;uint32_t type,latency,power;
        if(uacpi_object_get_package(p.objects[n+1],&entry)||entry.count!=4||reg(entry.objects[0],&registers[n])||
            integer(entry.objects[1],&type)||!type||type>3||type<previous||(!n&&type!=1)||
            integer(entry.objects[2],&latency)||latency>UINT16_MAX||integer(entry.objects[3],&power))goto done;
        previous=type;Register r=registers[n];
        for(unsigned j=0;j<n;j++)if(registers[j].space==r.space&&registers[j].address==r.address)goto done;
        // Reject malformed FFH encodings, including unknown reserved arguments.
        if(r.space==0x7f) {
            if(r.width==0) {if(r.offset||r.access||r.address||type!=1)goto done;}
            else if(r.width==1&&r.offset==1) {if(r.access||!r.address||r.address>65535||type!=1)goto done;}
            else if(r.width==1&&r.offset==2) {if(r.address>255||r.access&~3U)goto done;}
            else goto done;
        } else if(r.space==1) {if(r.width!=8||r.offset||r.access>1||!r.address||r.address>65535)goto done;}
        // Other address spaces are syntax-only and never read on idle.
        cpu->value.descriptor_identity=hash(hash(cpu->value.descriptor_identity,r.address),
            r.space|((uint64_t)r.width<<8)|((uint64_t)r.offset<<16)|((uint64_t)r.access<<24));
        cpu->value.descriptor_identity=hash(hash(hash(cpu->value.descriptor_identity,type),latency),power);
        if(type==2&&r.space==0x7f&&r.width==1&&r.offset==2&&r.access==1&&r.address>=16&&r.address<112&&
            (!found||latency<cpu->value.latency_us)) {
            found=1;cpu->value.selected_index=n;cpu->value.cstate_type=2;cpu->value.entry_method=1;
            cpu->value.entry_hint=(uint32_t)r.address;cpu->value.latency_us=latency;
            cpu->value.coordination=r.access;cpu->value.declared_power_mw=power;
        }
    }
    result=found?0:ENTRY;
done:
    uacpi_object_unref(o);return result;
}
static int csd(Cpu *cpu) {
    uacpi_object *o=0;int present=0,rc=eval(cpu->node,"_CSD",&o,&present);
    // An invalid present dependency has unknown scope; it cannot grant a
    // supposedly independent entry on another CPU in this catalogue.
    cpu->csd_dependency=present;
    if(rc)return rc==1?0:MALFORMED;
    uacpi_object_array p;int result=MALFORMED;
    if(uacpi_object_get_package(o,&p)||p.count>MAX_STATES)goto done;
    for(unsigned n=0;n<p.count;n++) {
        uacpi_object_array e;uint32_t v[6];
        if(uacpi_object_get_package(p.objects[n],&e)||e.count!=6)goto done;
        for(unsigned k=0;k<6;k++)if(integer(e.objects[k],&v[k]))goto done;
        if(v[0]!=6||v[1]||v[3]<0xfc||v[3]>0xfe||!v[4]||v[4]>64||v[5]>=cpu->value.state_count)goto done;
        for(unsigned k=0;k<6;k++)cpu->value.descriptor_identity=hash(cpu->value.descriptor_identity,v[k]);
    }
    cpu->csd_dependency=p.count!=0;result=p.count?COORDINATION:0;
done:
    uacpi_object_unref(o);return result;
}
static int lpi(Cpu *cpu) {
    // A parent _LPI implies a hierarchy too; never silently use an unrelated
    // _CST entry when parent coordination/context requirements are present.
    for(uacpi_namespace_node *node=cpu->node;node;node=uacpi_namespace_node_parent(node)) {
        uacpi_object *o=0;int present=0,rc=eval(node,"_LPI",&o,&present);
        if(rc==1)continue;
        // A present LPI declaration has its own state identity and units.
        // Never attach its minimum/context to an earlier CST candidate.
        cpu->value.source=2;cpu->value.state_count=cpu->value.selected_index=0;
        cpu->value.cstate_type=cpu->value.entry_method=cpu->value.entry_hint=0;
        cpu->value.latency_us=cpu->value.min_residency_us=0;
        cpu->value.coordination=cpu->value.context_loss=cpu->value.declared_power_mw=0;
        cpu->value.lpi_state_count=0;
        if(rc)return MALFORMED;
        uacpi_object_array p;uint32_t revision,level,count,first[6]={0};int result=MALFORMED;
        if(uacpi_object_get_package(o,&p)||p.count<4||integer(p.objects[0],&revision)||revision||integer(p.objects[1],&level)||
            integer(p.objects[2],&count)||!count||count>MAX_STATES||p.count!=count+3)goto done;
        cpu->value.descriptor_identity=hash(hash(hash(hash(cpu->value.descriptor_identity,2),revision),level),count);
        for(unsigned n=0;n<count;n++) {
            uacpi_object_array e;uint32_t v[6];Register r;uint64_t entry;uacpi_data_view name;
            if(uacpi_object_get_package(p.objects[n+3],&e)||e.count!=10)goto done;
            for(unsigned k=0;k<6;k++)if(integer(e.objects[k],&v[k]))goto done;
            if(v[2]&~1U||uacpi_object_get_string(e.objects[9],&name)||name.length>256)goto done;
            for(unsigned k=0;k<6;k++)cpu->value.descriptor_identity=hash(cpu->value.descriptor_identity,v[k]);
            if(!uacpi_object_get_integer(e.objects[6],&entry)) {
                cpu->value.descriptor_identity=hash(hash(cpu->value.descriptor_identity,0),entry);
            } else {
                if(reg(e.objects[6],&r))goto done;
                cpu->value.descriptor_identity=hash(hash(hash(cpu->value.descriptor_identity,1),r.address),
                    r.space|((uint64_t)r.width<<8)|((uint64_t)r.offset<<16)|((uint64_t)r.access<<24));
            }
            for(unsigned k=7;k<9;k++) {
                if(reg(e.objects[k],&r))goto done;
                cpu->value.descriptor_identity=hash(hash(cpu->value.descriptor_identity,r.address),
                    r.space|((uint64_t)r.width<<8)|((uint64_t)r.offset<<16)|((uint64_t)r.access<<24));
            }
            cpu->value.descriptor_identity=hash(cpu->value.descriptor_identity,name.length);
            for(unsigned k=0;k<name.length;k++)cpu->value.descriptor_identity=hash(cpu->value.descriptor_identity,name.bytes[k]);
            if(n==0)for(unsigned k=0;k<6;k++)first[k]=v[k];
        }
        cpu->value.state_count=cpu->value.lpi_state_count=count;
        cpu->value.min_residency_us=first[0];cpu->value.latency_us=first[1];cpu->value.context_loss=first[3];
        result=LPI_CONTEXT;
done:
        uacpi_object_unref(o);return result;
    }
    return 0;
}
void r4_idle_reset(void) {uacpi_memset(cpus,0,sizeof(cpus));withdrawal=complete=drained=0;}
void r4_idle_discover_cpu(uacpi_namespace_node *node,uint32_t cpu,uint32_t uid,uint32_t apic,uint64_t epoch) {
    if(cpu>=MAX_CPUS)return;
    Cpu *p=&cpus[cpu];if(p->node){p->value.reason=MALFORMED;return;}
    p->node=node;p->value=(R4IdleDescriptor){.version=1,.size=96,.cpu_index=cpu,.epoch=epoch,
        .descriptor_identity=hash(hash(1469598103934665603ULL,uid),apic)};
    p->value.reason=cst(p);int dependency=csd(p),low=lpi(p);
    if(dependency)p->value.reason=dependency;
    if(low)p->value.reason=low;
    if(!p->value.descriptor_identity)p->value.descriptor_identity=1;
}
void r4_idle_discovery_complete(int failed) {
    unsigned any=0,all=1,dependency=0;
    for(unsigned c=0;c<MAX_CPUS;c++)if(cpus[c].node){
        any|=cpus[c].cst_present;
        if(cpus[c].csd_dependency&&dependency!=LPI_CONTEXT)dependency=COORDINATION;
        if(cpus[c].value.source==2)dependency=LPI_CONTEXT;
    }
    for(unsigned c=0;c<MAX_CPUS;c++) {uint32_t id;uint64_t epoch;
        if(!r4_power_cpu(c,&id,&epoch)&&any&&(!cpus[c].node||!cpus[c].cst_present))all=0;
    }
    for(unsigned c=0;c<MAX_CPUS;c++) {
        Cpu *p=&cpus[c];if(!p->node)continue;
        if(failed||!all)p->value.reason=MALFORMED;
        else if(dependency&&!p->value.reason)p->value.reason=dependency;
        if(__atomic_load_n(&withdrawal,__ATOMIC_ACQUIRE)){p->value.reason=WITHDRAWN;p->value.source=0;}
        if(r4_idle_submit(&p->value))p->value.reason=WITHDRAWN;
    }
    __atomic_store_n(&complete,1,__ATOMIC_RELEASE);
}
void r4_idle_notify(uacpi_namespace_node *node,uint64_t value) {
    if(value!=0x81||!node||!__atomic_load_n(&complete,__ATOMIC_ACQUIRE))return;
    unsigned related=0;
    for(unsigned c=0;c<MAX_CPUS;c++)if(cpus[c].node) {
        for(uacpi_namespace_node *p=cpus[c].node;p;p=uacpi_namespace_node_parent(p))if(p==node){related=1;break;}
    }
    if(!related)return;
    // A changed hierarchy invalidates the whole old catalogue, including
    // dependent siblings. Never re-evaluate or republish behind live entries.
    __atomic_store_n(&withdrawal,1,__ATOMIC_RELEASE);
    for(unsigned c=0;c<MAX_CPUS;c++)if(cpus[c].node) {
        R4IdleDescriptor v=cpus[c].value;v.reason=WITHDRAWN;v.source=0;
        (void)r4_idle_submit(&v); // Busy already closes admission and wakes.
    }
    r4_host_wake(3);
}
void r4_idle_stop(void) {__atomic_store_n(&withdrawal,1,__ATOMIC_RELEASE);r4_host_wake(3);}
int r4_idle_finish(void) {
    if(!__atomic_load_n(&complete,__ATOMIC_ACQUIRE))return 1;
    if(__atomic_load_n(&drained,__ATOMIC_ACQUIRE))return 1;
    if(!__atomic_load_n(&withdrawal,__ATOMIC_ACQUIRE))return 1;
    unsigned busy=0;
    for(unsigned c=0;c<MAX_CPUS;c++)if(cpus[c].node) {
        R4IdleDescriptor v=cpus[c].value;v.reason=WITHDRAWN;v.source=0;
        int rc=r4_idle_submit(&v);if(rc)busy=1;
    }
    if(!busy)__atomic_store_n(&drained,1,__ATOMIC_RELEASE);
    return !busy;
}
void r4_idle_poll(void) {if(__atomic_load_n(&withdrawal,__ATOMIC_ACQUIRE))(void)r4_idle_finish();}
