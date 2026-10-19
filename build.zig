const std = @import("std");
const sources = [_][]const u8{
    "ThirdParty/uACPI/source/default_handlers.c",
    "ThirdParty/uACPI/source/event.c",
    "ThirdParty/uACPI/source/interpreter.c",
    "ThirdParty/uACPI/source/io.c",
    "ThirdParty/uACPI/source/mutex.c",
    "ThirdParty/uACPI/source/namespace.c",
    "ThirdParty/uACPI/source/notify.c",
    "ThirdParty/uACPI/source/opcodes.c",
    "ThirdParty/uACPI/source/opregion.c",
    "ThirdParty/uACPI/source/osi.c",
    "ThirdParty/uACPI/source/registers.c",
    "ThirdParty/uACPI/source/resources.c",
    "ThirdParty/uACPI/source/shareable.c",
    "ThirdParty/uACPI/source/sleep.c",
    "ThirdParty/uACPI/source/stdlib.c",
    "ThirdParty/uACPI/source/tables.c",
    "ThirdParty/uACPI/source/types.c",
    "ThirdParty/uACPI/source/uacpi.c",
    "ThirdParty/uACPI/source/utilities.c",
    "src/host.c",
    "src/platform.c",
    "src/ec.c",
    "src/power.c",
    "src/idle.c",
};
fn addC(b: *std.Build, target: std.Build.ResolvedTarget, host: bool) *std.Build.Module {
    const m = b.createModule(.{ .target = target, .optimize = .ReleaseSafe, .pic = host, .code_model = if (host) .default else .large, .red_zone = false, .sanitize_c = .off, .stack_check = false, .stack_protector = false, .link_libc = host });
    m.addIncludePath(b.path("ThirdParty/uACPI/include"));
    m.addIncludePath(b.path("src"));
    m.addCSourceFiles(.{ .files = &sources, .flags = &.{ "-std=c11", "-ffreestanding", "-fno-builtin", "-fvisibility=hidden", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-DUACPI_DEFAULT_LOOP_TIMEOUT_SECONDS=2", "-DUACPI_DEFAULT_MAX_CALL_STACK_DEPTH=64" } });
    return m;
}
pub fn build(b: *std.Build) void {
    const sb = b.lazyImport(@This(), "r4os_sdk") orelse return;
    const sdk = sb.sdk(b, b.dependencyFromBuildZig(sb, .{}), .{});
    const target = b.resolveTargetQuery(.{ .cpu_arch = .x86_64, .os_tag = .freestanding, .abi = .none, .ofmt = .elf });
    const archive = b.addLibrary(.{ .name = "ACPIPLAT-uACPI", .linkage = .static, .root_module = addC(b, target, false) });
    _ = sdk.addR4MFWithOptions(b.path("module.R4MF"), .{ .native_archives = &.{archive.getEmittedBin()} });
    const host = addC(b, b.graph.host, true);
    host.addImport("r4os", sdk.createR4osModule(b.graph.host, .ReleaseSafe));
    host.root_source_file = b.path("Tests/main.zig");
    host.addCMacro("R4_ACPI_TEST", "1");
    host.addCSourceFile(.{ .file = b.path("Tests/host.c"), .flags = &.{"-std=c11"} });
    const tests = b.addExecutable(.{ .name = "acpi-platform-test", .root_module = host });
    const run = b.addRunArtifact(tests);
    b.step("test", "Bounded AML, EC, events and shutdown integration").dependOn(&run.step);
}
