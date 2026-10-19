// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <uacpi/namespace.h>
typedef struct {
    uint32_t version,size,cpu_index,flags;
    uint64_t epoch,firmware_generation,descriptor_identity;
    uint32_t source,state_count,selected_index,cstate_type,entry_method,entry_hint;
    uint32_t latency_us,min_residency_us,coordination,context_loss,declared_power_mw;
    uint32_t reason,lpi_state_count,reserved0;
} R4IdleDescriptor;
_Static_assert(sizeof(R4IdleDescriptor)==96,"idle descriptor ABI");
void r4_idle_reset(void);
void r4_idle_discover_cpu(uacpi_namespace_node *,uint32_t,uint32_t,uint32_t,uint64_t);
void r4_idle_discovery_complete(int);
void r4_idle_notify(uacpi_namespace_node *,uint64_t);
void r4_idle_stop(void);
void r4_idle_poll(void);
int r4_idle_finish(void);
int r4_idle_submit(const R4IdleDescriptor *);
