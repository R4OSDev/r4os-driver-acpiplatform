// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <uacpi/uacpi.h>
void *r4_alloc(size_t);
void r4_free(void *);
uint64_t r4_rsdp(void);
void *r4_view(uint64_t, uint64_t);
uint64_t r4_now(void);
void r4_sleep(uint64_t);
uint64_t r4_thread(void);
void r4_log(uint32_t, const char *);
uint32_t r4_in(uint16_t, uint32_t);
void r4_out(uint16_t, uint32_t, uint32_t);
uint64_t r4_irq_disable(void);
void r4_irq_restore(uint64_t);
uint64_t r4_semaphore(uint32_t, uint32_t);
int r4_acquire(uint64_t, uint16_t);
void r4_release(uint64_t);
void r4_destroy(uint64_t);
int r4_input(uint32_t, uint32_t);

void r4_host_reset(void);
void r4_host_stop(void);
int r4_host_fault(void);
int r4_host_worker(uint32_t);
int r4_platform_run(void);
int r4_platform_shutdown_status(void);
void r4_platform_stop(void);
void r4_platform_reset(void);
uacpi_status r4_ec_early(void);
uacpi_status r4_ec_discover(void);
void r4_ec_poll(void);
void r4_ec_stop(void);
void r4_ec_release(void);
