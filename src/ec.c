// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
// ACPI EC protocol: the ports come exclusively from ECDT or PNP0C09 _CRS.
#include "port.h"
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/resources.h>
#include <uacpi/utilities.h>
#include <uacpi/opregion.h>
#include <uacpi/event.h>

typedef struct {
    uacpi_namespace_node *node,*gpe_node;
    uint64_t mutex;
    uint16_t data,command,gpe;
    unsigned global_lock,installed,gpe_installed,pending,failed;
} Ec;
static Ec controllers[4];
static unsigned count,stopping;
static uacpi_status failure;

static uacpi_status wait_status(Ec *e,unsigned mask,unsigned want,uint64_t deadline) {
    for(;;) {
        unsigned status=r4_in(e->command,1);
        if((status&mask)==want)return UACPI_STATUS_OK;
        if(r4_now()>=deadline) {
            __atomic_store_n(&e->failed,1,__ATOMIC_RELEASE);
            r4_log(UACPI_LOG_ERROR,"ACPIPLAT EC timeout; controller retained and quarantined");
            return UACPI_STATUS_HARDWARE_TIMEOUT;
        }
        r4_sleep(1);
    }
}
static uacpi_status byte(Ec *e,unsigned op,unsigned address,uint8_t *value) {
    if(__atomic_load_n(&e->failed,__ATOMIC_ACQUIRE))return UACPI_STATUS_HARDWARE_TIMEOUT;
    const uint64_t deadline=r4_now()+500000000;
    uacpi_status rc=wait_status(e,2,0,deadline);if(rc)return rc;
    // Unexpected OBF means the stream belongs to an unfinished operation.
    // Do not consume that byte or blindly issue another command.
    if(r4_in(e->command,1)&1) {
        __atomic_store_n(&e->failed,1,__ATOMIC_RELEASE);return UACPI_STATUS_DENIED;
    }
    r4_out(e->command,1,op);
    if(op!=0x84) {
        rc=wait_status(e,2,0,deadline);if(rc)return rc;
        r4_out(e->data,1,address);
    }
    if(op==0x81) {
        rc=wait_status(e,2,0,deadline);if(rc)return rc;
        r4_out(e->data,1,*value);
        return wait_status(e,2,0,deadline);
    }
    rc=wait_status(e,1,1,deadline);if(rc)return rc;
    *value=r4_in(e->data,1);return UACPI_STATUS_OK;
}
static uacpi_status acquire(Ec *e,uint32_t *sequence) {
    *sequence=0;
    if(e->global_lock) {
        uacpi_status rc=uacpi_acquire_global_lock(1000,sequence);
        if(rc)return rc;
    }
    if(r4_acquire(e->mutex,1000)!=0) {
        if(e->global_lock)uacpi_release_global_lock(*sequence);
        return UACPI_STATUS_TIMEOUT;
    }
    return UACPI_STATUS_OK;
}
static void release(Ec *e,uint32_t sequence) {
    r4_release(e->mutex);
    if(e->global_lock)uacpi_release_global_lock(sequence);
}
static uacpi_status region(uacpi_region_op op,void *data) {
    if(op==UACPI_REGION_OP_ATTACH) {
        uacpi_region_attach_data *d=data;
        if(!d->generic_info.length||d->generic_info.base>255||d->generic_info.length>256-d->generic_info.base)return UACPI_STATUS_INVALID_ARGUMENT;
        d->out_region_context=d->handler_context;return UACPI_STATUS_OK;
    }
    if(op==UACPI_REGION_OP_DETACH)return UACPI_STATUS_OK;
    if(op!=UACPI_REGION_OP_READ&&op!=UACPI_REGION_OP_WRITE)return UACPI_STATUS_UNIMPLEMENTED;
    uacpi_region_rw_data *d=data;Ec *e=d->region_context;
    if(!e||!d->byte_width||d->byte_width>8||d->address>255||d->byte_width>256-d->address)return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t sequence;uacpi_status rc=acquire(e,&sequence);if(rc)return rc;
    uint64_t result=0;
    for(unsigned i=0;i<d->byte_width;i++) {
        uint8_t value=d->value>>(8*i);
        rc=byte(e,op==UACPI_REGION_OP_READ?0x80:0x81,d->address+i,&value);
        if(rc)break;result|=(uint64_t)value<<(8*i);
    }
    release(e,sequence);
    if(!rc&&op==UACPI_REGION_OP_READ)d->value=result;
    return rc;
}
static uacpi_status install(Ec *e,uacpi_namespace_node *node) {
    if(!e->mutex)e->mutex=r4_semaphore(1,1);
    if(!e->mutex)return UACPI_STATUS_OUT_OF_MEMORY;
    uacpi_status rc=uacpi_install_address_space_handler(node,UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,region,e);
    if(!rc)e->installed=1;
    return rc;
}
uacpi_status r4_ec_early(void) {
    count=0;stopping=0;failure=UACPI_STATUS_OK;
    for(unsigned i=0;i<4;i++)controllers[i]=(Ec){0};
    uacpi_table t;uacpi_status rc=uacpi_table_find_by_signature("ECDT",&t);
    if(rc==UACPI_STATUS_NOT_FOUND)return UACPI_STATUS_OK;
    if(rc)return rc;
    struct acpi_ecdt *d=t.ptr;
    if(d->hdr.length<sizeof(*d)||d->ec_control.address_space_id!=1||d->ec_data.address_space_id!=1||
       d->ec_control.register_bit_width!=8||d->ec_data.register_bit_width!=8||d->ec_control.register_bit_offset||d->ec_data.register_bit_offset||
       !d->ec_control.address||d->ec_control.address>65535||!d->ec_data.address||d->ec_data.address>65535||d->ec_control.address==d->ec_data.address)rc=UACPI_STATUS_INVALID_ARGUMENT;
    else {
        Ec *e=&controllers[count++];e->data=d->ec_data.address;e->command=d->ec_control.address;e->gpe=d->gpe_bit;e->global_lock=1;
        rc=install(e,uacpi_namespace_root());
    }
    uacpi_table_unref(&t);return rc;
}
typedef struct {uint16_t ports[2];unsigned count,bad;} Ports;
static uacpi_iteration_decision resource(void *context,uacpi_resource *r) {
    Ports *p=context;unsigned port=0,length=0;
    if(r->type==UACPI_RESOURCE_TYPE_IO) {
        if(r->io.minimum!=r->io.maximum)p->bad=1;
        port=r->io.minimum;length=r->io.length;
    } else if(r->type==UACPI_RESOURCE_TYPE_FIXED_IO) {port=r->fixed_io.address;length=r->fixed_io.length;}
    else return UACPI_ITERATION_DECISION_CONTINUE;
    if(!port||length!=1||p->count>=2)p->bad=1;
    else p->ports[p->count++]=port;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_interrupt_ret gpe(void *context,uacpi_namespace_node *node,uacpi_u16 idx) {
    (void)node;(void)idx;Ec *e=context;
    __atomic_store_n(&e->pending,1,__ATOMIC_RELEASE);
    return UACPI_INTERRUPT_HANDLED; // reenable only after the query worker
}
static uacpi_iteration_decision discover(void *context,uacpi_namespace_node *node,uacpi_u32 depth) {
    (void)context;(void)depth;Ports ports={0};
    uacpi_status rc=uacpi_for_each_device_resource(node,"_CRS",resource,&ports);
    if(rc||ports.bad||ports.count!=2||ports.ports[0]==ports.ports[1]){failure=rc?rc:UACPI_STATUS_INVALID_ARGUMENT;return UACPI_ITERATION_DECISION_CONTINUE;}
    Ec *e=0;
    for(unsigned i=0;i<count;i++)if(controllers[i].data==ports.ports[0]&&controllers[i].command==ports.ports[1]){e=&controllers[i];break;}
    if(!e) {
        if(count==4){failure=UACPI_STATUS_OUT_OF_MEMORY;return UACPI_ITERATION_DECISION_BREAK;}
        e=&controllers[count++];e->data=ports.ports[0];e->command=ports.ports[1];
    }
    if(e->node){failure=UACPI_STATUS_ALREADY_EXISTS;return UACPI_ITERATION_DECISION_BREAK;}
    e->node=node;
    uacpi_u64 glk=0;rc=uacpi_eval_simple_integer(node,"_GLK",&glk);
    if(rc&&rc!=UACPI_STATUS_NOT_FOUND){failure=rc;return UACPI_ITERATION_DECISION_BREAK;}
    e->global_lock=glk!=0;
    // ECDT installed a root handler before AML existed. Bind the discovered
    // PNP0C09 node as its own region owner before running that node's _REG.
    rc=install(e,node);if(rc){failure=rc;return UACPI_ITERATION_DECISION_BREAK;}
    rc=uacpi_reg_all_opregions(node,UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER);
    if(rc){failure=rc;return UACPI_ITERATION_DECISION_BREAK;}
    uacpi_object *obj=0;rc=uacpi_eval_simple(node,"_GPE",&obj);
    if(rc==UACPI_STATUS_NOT_FOUND) {r4_log(UACPI_LOG_WARN,"ACPIPLAT EC has no _GPE; status polling only");return UACPI_ITERATION_DECISION_CONTINUE;}
    if(rc){failure=rc;return UACPI_ITERATION_DECISION_BREAK;}
    uacpi_u64 idx=0;rc=uacpi_object_get_integer(obj,&idx);
    if(rc) {
        uacpi_object_array p;
        rc=uacpi_object_get_package(obj,&p);
        if(!rc&&p.count==2) {
            rc=uacpi_object_resolve_as_aml_namepath(p.objects[0],node,&e->gpe_node);
            if(!rc)rc=uacpi_object_get_integer(p.objects[1],&idx);
        } else rc=UACPI_STATUS_TYPE_MISMATCH;
    }
    uacpi_object_unref(obj);
    if(rc||idx>65535){failure=rc?rc:UACPI_STATUS_INVALID_ARGUMENT;return UACPI_ITERATION_DECISION_BREAK;}
    e->gpe=idx;
    rc=uacpi_install_gpe_handler(e->gpe_node,e->gpe,UACPI_GPE_TRIGGERING_EDGE,gpe,e);
    if(!rc){e->gpe_installed=1;rc=uacpi_enable_gpe(e->gpe_node,e->gpe);}
    if(rc){failure=rc;return UACPI_ITERATION_DECISION_BREAK;}
    return UACPI_ITERATION_DECISION_CONTINUE;
}
uacpi_status r4_ec_discover(void) {
    uacpi_status rc=uacpi_find_devices("PNP0C09",discover,0);
    return rc?rc:failure;
}
void r4_ec_poll(void) {
    if(stopping)return;
    for(unsigned i=0;i<count;i++) {
        Ec *e=&controllers[i];if(!e->node||__atomic_load_n(&e->failed,__ATOMIC_ACQUIRE))continue;
        // Only the EC owner reads status while another AML EC transaction is
        // running. A status read can acknowledge SCI_EVT on some firmware.
        unsigned more=0;
        for(unsigned attempt=0;attempt<16;attempt++) {
            uint32_t sequence;uacpi_status rc=acquire(e,&sequence);if(rc)break;
            uint8_t query=0;unsigned status=r4_in(e->command,1);
            more=(status&0x20)!=0;
            if(more)rc=byte(e,0x84,0,&query);
            release(e,sequence);
            if(rc)break;
            if(!more||!query){more=0;break;}
            const char hex[]="0123456789ABCDEF";char method[5]={'_','Q',hex[query>>4],hex[query&15],0};
            // AML may access the EC again, so no EC mutex or Global Lock spans _Qxx.
            rc=uacpi_execute_simple(e->node,method);
            if(rc&&rc!=UACPI_STATUS_NOT_FOUND)r4_log(UACPI_LOG_WARN,"ACPIPLAT EC query AML failed");
            r4_sleep(1); // bounded event-clear guard; no status-read spin
        }
        if(!more&&!__atomic_load_n(&e->failed,__ATOMIC_ACQUIRE)&&__atomic_exchange_n(&e->pending,0,__ATOMIC_ACQ_REL))
            uacpi_finish_handling_gpe(e->gpe_node,e->gpe);
    }
}
void r4_ec_stop(void) {
    stopping=1;
    for(unsigned i=0;i<count;i++)if(controllers[i].gpe_installed)uacpi_disable_gpe(controllers[i].gpe_node,controllers[i].gpe);
}
void r4_ec_release(void) {
    // Namespace and every callback were retired by uacpi_state_reset first.
    for(unsigned i=0;i<count;i++)if(controllers[i].mutex)r4_destroy(controllers[i].mutex);
    count=0;
}
