// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
const std = @import("std");
const r = @import("r4os");
const a = r.abi;
var driver: r.r4dev.DriverContext = undefined;
var heap: r.r4dev.DriverHeapContext = undefined;
var resources: r.r4dev.DriverResourceContext = undefined;
var platform: r.r4dev.DriverPlatformContext = undefined;
var threads: r.r4dev.DriverThreadContext = undefined;
var semaphores: r.r4dev.DriverSemaphoreContext = undefined;
var handles: [4]u64 = @splat(0);
var started: bool = false;
extern fn r4_host_reset() void;
extern fn r4_host_stop() void;
extern fn r4_host_worker(u32) c_int;
extern fn r4_platform_run() c_int;
extern fn r4_platform_shutdown_status() c_int;
extern fn r4_platform_reset() void;
extern fn r4_platform_stop() void;
comptime { asm (r.r4dev.driverEntriesAsm("acpiplat_init", "acpiplat_shutdown")); }

export fn acpiplat_init(api: *const r.r4dev.DriverApi) callconv(.c) i32 {
    if (started) return -1;
    driver = r.r4dev.DriverContext.init(api);
    resources = driver.resources() orelse return -1;
    platform = resources.platform() orelse return -1;
    heap = driver.heap() orelse return -1;
    threads = driver.threads() orelse return -1;
    semaphores = driver.semaphores() orelse return -1;
    if (platform.rsdp() == 0 or resources.nowNs() == 0 or resources.nowNs() == std.math.maxInt(u64) or driver.timerFrequency() == 0) return -1;
    r4_host_reset(); r4_platform_reset();
    started = true;
    for (0..handles.len) |i| {
        // No parallel flag: firmware AML and the polling SCI stay on CPU0.
        if (threads.start(run, i, 0, &handles[i]) != 0) {
            r4_platform_stop(); r4_host_stop();
            _ = acpiplat_shutdown(); return -1;
        }
    }
    driver.logInfo("ACPIPLAT external ACPI/EC startup; SCI polling; no system suspend");
    return 0;
}
fn run(kind: usize) callconv(.c) i32 {
    if (kind < 3) return r4_host_worker(@intCast(kind));
    const result = r4_platform_run();
    r4_host_stop();
    return result;
}
export fn acpiplat_shutdown() callconv(.c) i32 {
    if (!started) return 0;
    r4_platform_stop();
    // Never cancel or free a task inside AML. A timed-out stop retains the
    // module and all callback storage; a later shutdown can join it again.
    var result: i32 = 0;
    if (handles[3] != 0 and threads.join(handles[3], ticks(5000), &result) != 0) return -1;
    r4_host_stop();
    for (&handles) |*handle| if (handle.* != 0) {
        if (threads.join(handle.*, ticks(1000), &result) != 0 or threads.release(handle.*) != 0) return -1;
        handle.* = 0;
    };
    _ = platform.input(a.platform_input_kind_capabilities, 0);
    if (r4_platform_shutdown_status() != 0) return -1;
    started = false; return 0;
}
fn ticks(ms: u64) u64 {
    const hz = @max(driver.timerFrequency(), 1);
    return @intCast(@min(std.math.maxInt(u64) - 1, @max(1, (@as(u128, ms) * hz + 999) / 1000)));
}
// Header stays resident in the authenticated driver heap until exact release.
export fn r4_alloc(bytes: usize) callconv(.c) ?*anyopaque {
    if (bytes == 0 or bytes > 16 * 1024 * 1024) return null;
    var allocation: a.DriverHeapAllocation = .{};
    if (heap.allocate(bytes + 16, 16, &allocation) != 0) return null;
    const header: *u64 = @ptrFromInt(allocation.cpu_address);
    header.* = allocation.handle;
    return @ptrFromInt(allocation.cpu_address + 16);
}
export fn r4_free(address: ?*anyopaque) callconv(.c) void {
    const value = address orelse return;
    const header: *const u64 = @ptrFromInt(@intFromPtr(value) - 16);
    _ = heap.release(header.*);
}
export fn r4_rsdp() callconv(.c) u64 { return platform.rsdp(); }
export fn r4_view(physical: u64, bytes: u64) callconv(.c) ?*anyopaque {
    return @ptrCast(platform.physicalView(physical, bytes) orelse return null);
}
export fn r4_now() callconv(.c) u64 { return resources.nowNs(); }
export fn r4_sleep(ms: u64) callconv(.c) void {
    if (ms == 0) { _ = threads.sleepTicks(1); return; }
    // Bound every scheduler wait even for an arbitrarily long AML Sleep.
    var remaining = ms;
    while (remaining != 0) {
        const part = @min(remaining, 1000);
        _ = threads.sleepTicks(ticks(part)); remaining -= part;
    }
}
export fn r4_thread() callconv(.c) u64 { return threads.current(); }
export fn r4_log(level: u32, message: [*:0]const u8) callconv(.c) void {
    if (level <= 1) driver.logError(message) else if (level == 2) driver.logWarn(message) else driver.logInfo(message);
}
export fn r4_in(port: u16, width: u32) callconv(.c) u32 {
    return switch (width) { 1 => driver.portInb(port), 2 => driver.portInw(port), 4 => driver.portInl(port), else => unreachable };
}
export fn r4_out(port: u16, width: u32, value: u32) callconv(.c) void {
    switch (width) { 1 => driver.portOutb(port, @truncate(value)), 2 => driver.portOutw(port, @truncate(value)), 4 => driver.portOutl(port, value), else => unreachable }
}
export fn r4_irq_disable() callconv(.c) u64 {
    return asm volatile ("pushfq; popq %[flags]; cli" : [flags] "=r" (-> u64) :: .{ .memory = true });
}
export fn r4_irq_restore(flags: u64) callconv(.c) void {
    if (flags & 0x200 != 0) asm volatile ("sti" ::: .{ .memory = true });
}
export fn r4_semaphore(initial: u32, maximum: u32) callconv(.c) u64 {
    var handle: u64 = 0;
    if (semaphores.create(initial, maximum, &handle) != 0) return 0;
    return handle;
}
export fn r4_acquire(handle: u64, ms: u16) callconv(.c) c_int {
    const result = semaphores.acquire(handle, if (ms == 0) 0 else if (ms == 0xffff) std.math.maxInt(u64) else ticks(ms));
    return if (result == 0) 0 else if (result == a.driver_semaphore_error_timeout) 1 else -1;
}
export fn r4_release(handle: u64) callconv(.c) void { _ = semaphores.release(handle); }
export fn r4_destroy(handle: u64) callconv(.c) void { _ = semaphores.destroy(handle); }
export fn r4_input(kind: u32, value: u32) callconv(.c) c_int { return platform.input(kind, value); }
