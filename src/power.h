// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <uacpi/namespace.h>
// Private C mirror of independent fixed wire records, layout checked at Zig
// bridge and host C conformance. No old HWP/CPPC receipt is reinterpreted.
typedef struct {
    uint32_t version,size,cpu_index,operation;
    uint64_t epoch,lease;
    uint32_t minimum,maximum,desired,epp,flags,reserved0;
    uint64_t expected_revision;
} R4PowerRequest;
typedef struct {
    uint32_t version,size,cpu_index,state,flags,backend,valid_mask,coordination;
    uint64_t epoch,provider_generation,firmware_generation,lease,revision,domain_mask,captured_ns;
    uint32_t minimum,maximum,desired,epp,lowest,highest,efficient,nominal;
    uint64_t baseline_request,last_written,observed_request;
    uint32_t domain_id,domain_cpus,pstate_count,package_revision;
    int32_t last_error;
    uint32_t transport_spaces,reserved0,reserved1;
    uint64_t descriptor_identity,reserved2;
} R4PowerInfo;
_Static_assert(sizeof(R4PowerRequest)==64,"request ABI");
_Static_assert(sizeof(R4PowerInfo)==192,"firmware receipt ABI");
// Only authenticated driver bridge calls these. The platform worker is the
// sole interpreter/register owner; callbacks merely enqueue and await receipt.
void r4_power_reset(void);
void r4_power_discover(void);
void r4_power_poll(void);
void r4_power_stop(void);
int r4_power_idle(void);
int r4_power_finish(void);
int r4_power_call(const R4PowerRequest *,R4PowerInfo *);
void r4_power_notify(uacpi_namespace_node *,uint64_t);
// Passive exact online catalogue identity and whole-domain hardware lease.
int r4_power_cpu(uint32_t,uint32_t *,uint64_t *);
int r4_power_control(uint32_t,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t *);
void r4_power_pause(uint64_t);
