// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
#include "port.h"
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/utilities.h>
#include <uacpi/context.h>

static unsigned stop_requested,ready,capabilities;
static int shutdown_status;
static uacpi_namespace_node *lid,*video[16],*adapters[8];
static unsigned video_count,adapter_count;
static uacpi_status discovery_failure;
void r4_platform_reset(void) {
    shutdown_status=0;stop_requested=0;ready=0;capabilities=0;lid=0;video_count=0;adapter_count=0;discovery_failure=0;
}
int r4_platform_shutdown_status(void) {return __atomic_load_n(&shutdown_status,__ATOMIC_ACQUIRE);}
void r4_platform_stop(void) {__atomic_store_n(&stop_requested,1,__ATOMIC_RELEASE);}
static void sample_lid(void) {
    if(!lid)return;
    uacpi_u64 value=0;uacpi_status rc=uacpi_eval_simple_integer(lid,"_LID",&value);
    r4_input(3,rc?0:value?1:2);
}
static uacpi_status notify(void *context,uacpi_namespace_node *node,uacpi_u64 value) {
    (void)context;
    if(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE)||__atomic_load_n(&stop_requested,__ATOMIC_ACQUIRE))return UACPI_STATUS_OK;
    if(node==lid&&value==0x80){sample_lid();return UACPI_STATUS_OK;}
    // ACPI Appendix B notifications are accepted only from discovered video
    // output nodes; vendor VPC notifications and arbitrary values are not keys.
    for(unsigned i=0;i<video_count;i++)if(node==video[i]) {
        if(value==0x86)r4_input(1,1);
        else if(value==0x87)r4_input(2,1);
        break;
    }
    return UACPI_STATUS_OK;
}
static uacpi_status dos(uacpi_namespace_node *node,uint64_t value) {
    uacpi_object *arg=uacpi_object_create_integer(value);if(!arg)return UACPI_STATUS_OUT_OF_MEMORY;
    uacpi_object_array args={.objects=&arg,.count=1};
    uacpi_status rc=uacpi_execute(node,"_DOS",&args);uacpi_object_unref(arg);return rc;
}
static uacpi_iteration_decision discover(void *context,uacpi_namespace_node *node,uacpi_u32 depth) {
    (void)context;(void)depth;
    static const char *const lid_ids[]={"PNP0C0D",0};
    static const char *const vendor_ids[]={"VPC2004",0};
    uint32_t sta=0;
    if(uacpi_eval_sta(node,&sta)||!(sta&1))return UACPI_ITERATION_DECISION_CONTINUE;
    if(uacpi_device_matches_pnp_id(node,lid_ids)) {
        if(lid){discovery_failure=UACPI_STATUS_ALREADY_EXISTS;return UACPI_ITERATION_DECISION_BREAK;}
        lid=node;
    }
    if(uacpi_device_matches_pnp_id(node,vendor_ids))
        r4_log(UACPI_LOG_WARN,"ACPIPLAT VPC2004 present; vendor Fn mapping requires firmware evidence, no scancodes inferred");
    uacpi_namespace_node *bcl=0,*bcm=0;
    if(uacpi_namespace_node_find(node,"_BCL",&bcl)||uacpi_namespace_node_find(node,"_BCM",&bcm))return UACPI_ITERATION_DECISION_CONTINUE;
    if(video_count==16){discovery_failure=UACPI_STATUS_OUT_OF_MEMORY;return UACPI_ITERATION_DECISION_BREAK;}
    uacpi_namespace_node *adapter=uacpi_namespace_node_parent(node),*method=0;
    for(unsigned n=0;adapter&&n<8;n++,adapter=uacpi_namespace_node_parent(adapter))
        if(!uacpi_namespace_node_find(adapter,"_DOS",&method))break;
    if(!adapter||!method){r4_log(UACPI_LOG_WARN,"ACPIPLAT video output lacks _DOS ownership; brightness events unavailable");return UACPI_ITERATION_DECISION_CONTINUE;}
    unsigned i;for(i=0;i<adapter_count;i++)if(adapters[i]==adapter)break;
    if(i==adapter_count) {
        if(adapter_count==8){discovery_failure=UACPI_STATUS_OUT_OF_MEMORY;return UACPI_ITERATION_DECISION_BREAK;}
        // OS owns output switching and brightness; AMD remains the only PWM/AUX
        // writer. Never invoke _BCM in competition with the graphics driver.
        uacpi_status rc=dos(adapter,4);
        if(rc){r4_log(UACPI_LOG_WARN,"ACPIPLAT _DOS ownership failed; brightness events unavailable");return UACPI_ITERATION_DECISION_CONTINUE;}
        adapters[adapter_count++]=adapter;
    }
    video[video_count++]=node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
int r4_platform_run(void) {
    int result=-1;unsigned initialized=0,notify_installed=0;
    uacpi_context_set_loop_timeout(2);uacpi_context_set_max_call_stack_depth(64);
    uacpi_status rc=UACPI_STATUS_OK;
    if(__atomic_load_n(&stop_requested,__ATOMIC_ACQUIRE)){result=0;goto done;}
    rc=uacpi_initialize(UACPI_FLAG_BAD_CSUM_FATAL|UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL);
    if(rc)goto done;
    initialized=1;
    if((rc=r4_ec_early())){r4_log(UACPI_LOG_ERROR,"ACPIPLAT ECDT setup failed");goto done;}
    if((rc=uacpi_namespace_load())){r4_log(UACPI_LOG_ERROR,"ACPIPLAT namespace load failed");goto done;}
    if((rc=r4_ec_discover())){r4_log(UACPI_LOG_ERROR,"ACPIPLAT EC discovery failed");goto done;}
    if((rc=uacpi_namespace_initialize())){r4_log(UACPI_LOG_ERROR,"ACPIPLAT namespace initialization failed");goto done;}
    rc=uacpi_namespace_for_each_child(uacpi_namespace_root(),discover,0,UACPI_OBJECT_DEVICE_BIT,UACPI_MAX_DEPTH_ANY,0);
    if(rc||discovery_failure){if(!rc)rc=discovery_failure;goto done;}
    rc=uacpi_install_notify_handler(uacpi_namespace_root(),notify,0);if(rc)goto done;
    notify_installed=1;
    capabilities=(video_count?1:0)|(lid?2:0);
    if(capabilities&&r4_input(4,capabilities)!=0){rc=UACPI_STATUS_DENIED;goto done;}
    if(lid)sample_lid();
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
    rc=uacpi_finalize_gpe_initialization();if(rc)goto done;
    r4_log(UACPI_LOG_INFO,"ACPIPLAT namespace ready; qualified brightness/lid sources published; SCI polling active");
    result=0;
    uint64_t next_lid=r4_now()+1000000000;
    while(!__atomic_load_n(&stop_requested,__ATOMIC_ACQUIRE)&&!r4_host_fault()) {
        r4_ec_poll();
        if(lid&&r4_now()>=next_lid){sample_lid();next_lid=r4_now()+1000000000;}
        r4_sleep(10);
    }
    if(r4_host_fault())result=-1;
done:
    __atomic_store_n(&ready,0,__ATOMIC_RELEASE);
    if(rc){result=-1;r4_log(UACPI_LOG_ERROR,uacpi_status_to_string(rc));}
    if(initialized) {
        // namespace_load may already have reset the interpreter on failure.
        if(uacpi_get_current_init_level()>=UACPI_INIT_LEVEL_NAMESPACE_LOADED) {
            r4_ec_stop();uacpi_disable_all_gpes();
            uacpi_kernel_wait_for_work_completion();
            if(notify_installed)uacpi_uninstall_notify_handler(uacpi_namespace_root(),notify);
            for(unsigned i=0;i<adapter_count;i++)if(dos(adapters[i],1)) {
                result=-1;__atomic_store_n(&shutdown_status,-1,__ATOMIC_RELEASE);
                r4_log(UACPI_LOG_ERROR,"ACPIPLAT _DOS restore failed; firmware handoff unconfirmed");
            }
        }
        uacpi_state_reset();r4_ec_release();
    }
    r4_input(4,0);
    r4_log(UACPI_LOG_INFO,"ACPIPLAT callbacks quiesced; platform input withdrawn");
    return result;
}
