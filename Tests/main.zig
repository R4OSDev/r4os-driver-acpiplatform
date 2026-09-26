const std = @import("std");
extern fn r4_test() c_int;
extern fn r4_host_worker(u32) c_int;
extern fn r4_platform_run() c_int;
extern fn r4_host_stop() void;
extern fn r4_host_release() void;
var workers: [4]?std.Thread = @splat(null);
fn worker(kind: u32) void {
    if (kind < 3) { _ = r4_host_worker(kind); } else { _ = r4_platform_run(); r4_host_stop(); }
}
export fn r4_test_start(kind: u32) callconv(.c) c_int {
    workers[kind] = std.Thread.spawn(.{}, worker, .{kind}) catch return -1;
    return 0;
}
export fn r4_test_join() callconv(.c) void {
    for (&workers) |*thread| if (thread.*) |value| { value.join(); thread.* = null; };
    r4_host_release();
}
export fn r4_now() callconv(.c) u64 {
    return @intCast(std.Io.Clock.awake.now(std.Io.Threaded.global_single_threaded.io()).nanoseconds);
}
export fn r4_sleep(ms: u64) callconv(.c) void {
    std.Io.sleep(std.Io.Threaded.global_single_threaded.io(), .fromMilliseconds(@intCast(ms)), .awake) catch {};
}
export fn r4_thread() callconv(.c) u64 { return @as(u64, std.Thread.getCurrentId()) + 1; }
export fn r4_irq_disable() callconv(.c) u64 { return 0; }
export fn r4_irq_restore(_: u64) callconv(.c) void {}
pub fn main() !void { if (r4_test() != 0) return error.AcpiPlatformIntegration; }
