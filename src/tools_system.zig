//! System tool handlers: get_version, get_health, create_tool, list_user_tools, delete_user_tool.
//! Replaces tools_system.c. No OTA/heap ESP32 metrics; uses process-level info.

const std = @import("std");
const config = @import("config.zig");
const ratelimit = @import("ratelimit.zig");
const cron = @import("cron.zig");
const tools_user = @import("tools_user.zig");

const log = std.log.scoped(.tools_system);

pub fn getVersionHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    _ = std.fmt.bufPrint(result, "zedclaw v{s} (desktop)", .{config.VERSION}) catch {};
    return true;
}

pub fn getHealthHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    const requests_hour = ratelimit.getRequestsThisHour();
    const requests_day = ratelimit.getRequestsToday();

    var time_buf: [64]u8 = undefined;
    const time_len = cron.getTimeStr(&time_buf);
    const time_str = time_buf[0..time_len];

    var tz_buf: [config.TIMEZONE_MAX_LEN]u8 = undefined;
    const tz_len = cron.getTimezone(&tz_buf);
    const tz_str = tz_buf[0..tz_len];

    _ = std.fmt.bufPrint(result,
        "Health: OK | " ++
        "Requests: {}/hr, {}/day (limits: {}/hr {}/day) | " ++
        "Time: {s} | TZ: {s} | Version: {s}",
        .{
            requests_hour,
            requests_day,
            config.RATELIMIT_MAX_PER_HOUR,
            config.RATELIMIT_MAX_PER_DAY,
            time_str,
            tz_str,
            config.VERSION,
        }) catch {};
    return true;
}

pub fn createToolHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;

    const obj = switch (input) {
        .object => |o| o,
        else => {
            _ = std.fmt.bufPrint(result, "Error: invalid input", .{}) catch {};
            return false;
        },
    };

    const name_val = obj.get("name") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'name' required (alphanumeric/underscore, no spaces)", .{}) catch {};
        return false;
    };
    const desc_val = obj.get("description") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'description' required", .{}) catch {};
        return false;
    };
    const action_val = obj.get("action") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'action' required", .{}) catch {};
        return false;
    };

    const name = switch (name_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'name' must be a string", .{}) catch {};
            return false;
        },
    };
    const desc = switch (desc_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'description' must be a string", .{}) catch {};
            return false;
        },
    };
    const action = switch (action_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'action' must be a string", .{}) catch {};
            return false;
        },
    };

    const ok = tools_user.create(name, desc, action) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: {}", .{err}) catch {};
        return false;
    };

    if (ok) {
        _ = std.fmt.bufPrint(result, "Created tool '{s}': {s}", .{ name, desc }) catch {};
        return true;
    }

    _ = std.fmt.bufPrint(result, "Error: failed to create tool (duplicate name or limit reached)", .{}) catch {};
    return false;
}

pub fn listUserToolsHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    _ = tools_user.list(result);
    return true;
}

pub fn deleteUserToolHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;

    const obj = switch (input) {
        .object => |o| o,
        else => {
            _ = std.fmt.bufPrint(result, "Error: invalid input", .{}) catch {};
            return false;
        },
    };

    const name_val = obj.get("name") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'name' required", .{}) catch {};
        return false;
    };

    const name = switch (name_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'name' must be a string", .{}) catch {};
            return false;
        },
    };

    const deleted = tools_user.delete(name) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: {}", .{err}) catch {};
        return false;
    };

    if (deleted) {
        _ = std.fmt.bufPrint(result, "Deleted tool '{s}'", .{name}) catch {};
    } else {
        _ = std.fmt.bufPrint(result, "Tool '{s}' not found", .{name}) catch {};
    }
    return true;
}
