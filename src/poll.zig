// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
// Private C port binding to the existing pure SDK value. This creates no
// kernel resource and does not acknowledge an EC operation or firmware lease.
const poll = @import("r4os").driver_poll_budget;
export fn r4_poll_init(out: *poll.DriverPollBudget, iterations: u64, no_sleep: u32) callconv(.c) void {
    out.* = poll.DriverPollBudget.initIterations(iterations, if (no_sleep != 0) .no_sleep else .task);
}
export fn r4_poll_step(budget: *poll.DriverPollBudget) callconv(.c) u32 {
    return @intFromEnum(budget.step(null, false));
}
