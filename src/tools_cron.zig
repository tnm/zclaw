//! Cron tool handlers (cron_set, cron_list, cron_delete, get_time, set_timezone, get_timezone).

const std = @import("std");
const config = @import("config.zig");
const cron = @import("cron.zig");

const log = std.log.scoped(.tools_cron);

pub fn cronSetHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    const obj = switch (input) {
        .object => |o| o,
        else => {
            _ = std.fmt.bufPrint(result, "Error: invalid input", .{}) catch {};
            return false;
        },
    };

    const type_val = obj.get("type") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'type' required (periodic/daily/once)", .{}) catch {};
        return false;
    };
    const action_val = obj.get("action") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'action' required", .{}) catch {};
        return false;
    };

    const type_str = switch (type_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'type' must be a string", .{}) catch {};
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

    const cron_type: cron.CronType = if (std.mem.eql(u8, type_str, "periodic"))
        .periodic
    else if (std.mem.eql(u8, type_str, "daily"))
        .daily
    else if (std.mem.eql(u8, type_str, "once"))
        .once
    else {
        _ = std.fmt.bufPrint(result, "Error: type must be 'periodic', 'daily', or 'once'", .{}) catch {};
        return false;
    };

    var interval_or_hour: u16 = 0;
    var minute: u8 = 0;

    switch (cron_type) {
        .periodic => {
            const iv = obj.get("interval_minutes") orelse {
                _ = std.fmt.bufPrint(result, "Error: 'interval_minutes' required for periodic", .{}) catch {};
                return false;
            };
            interval_or_hour = switch (iv) {
                .integer => |n| @intCast(@min(n, 65535)),
                else => {
                    _ = std.fmt.bufPrint(result, "Error: 'interval_minutes' must be integer", .{}) catch {};
                    return false;
                },
            };
            if (interval_or_hour == 0) {
                _ = std.fmt.bufPrint(result, "Error: interval_minutes must be > 0", .{}) catch {};
                return false;
            }
        },
        .once => {
            const dv = obj.get("delay_minutes") orelse {
                _ = std.fmt.bufPrint(result, "Error: 'delay_minutes' required for once", .{}) catch {};
                return false;
            };
            interval_or_hour = switch (dv) {
                .integer => |n| @intCast(@min(n, 65535)),
                else => {
                    _ = std.fmt.bufPrint(result, "Error: 'delay_minutes' must be integer", .{}) catch {};
                    return false;
                },
            };
        },
        .daily => {
            const hv = obj.get("hour") orelse {
                _ = std.fmt.bufPrint(result, "Error: 'hour' required for daily (0-23)", .{}) catch {};
                return false;
            };
            const mv = obj.get("minute") orelse {
                _ = std.fmt.bufPrint(result, "Error: 'minute' required for daily (0-59)", .{}) catch {};
                return false;
            };
            const hour_val: u16 = switch (hv) {
                .integer => |n| @intCast(@min(n, 23)),
                else => {
                    _ = std.fmt.bufPrint(result, "Error: 'hour' must be integer 0-23", .{}) catch {};
                    return false;
                },
            };
            interval_or_hour = hour_val;
            minute = switch (mv) {
                .integer => |n| @intCast(@min(n, 59)),
                else => {
                    _ = std.fmt.bufPrint(result, "Error: 'minute' must be integer 0-59", .{}) catch {};
                    return false;
                },
            };
        },
    }

    _ = allocator;
    const id = cron.cronSet(cron_type, interval_or_hour, minute, action);
    if (id == 0) {
        _ = std.fmt.bufPrint(result, "Error: failed to create schedule (limit reached?)", .{}) catch {};
        return false;
    }

    switch (cron_type) {
        .periodic => {
            _ = std.fmt.bufPrint(result, "Created schedule {} (every {} minutes): {s}", .{
                id, interval_or_hour, action,
            }) catch {};
        },
        .once => {
            _ = std.fmt.bufPrint(result, "Created one-time schedule {} (in {} minutes): {s}", .{
                id, interval_or_hour, action,
            }) catch {};
        },
        .daily => {
            _ = std.fmt.bufPrint(result, "Created daily schedule {} (at {:0>2}:{:0>2}): {s}", .{
                id, interval_or_hour, minute, action,
            }) catch {};
        },
    }

    return true;
}

pub fn cronListHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = input;

    const json_list = cron.cronList(allocator) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: {}", .{err}) catch {};
        return false;
    };
    defer allocator.free(json_list);

    const n = @min(json_list.len, result.len - 1);
    @memcpy(result[0..n], json_list[0..n]);
    result[n] = 0;
    return true;
}

pub fn cronDeleteHandler(
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

    const id_val = obj.get("id") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'id' required", .{}) catch {};
        return false;
    };

    const id: u8 = switch (id_val) {
        .integer => |n| @intCast(@min(n, 255)),
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'id' must be integer", .{}) catch {};
            return false;
        },
    };

    if (cron.cronDelete(id)) {
        _ = std.fmt.bufPrint(result, "Deleted schedule {}", .{id}) catch {};
        return true;
    }

    _ = std.fmt.bufPrint(result, "Schedule {} not found", .{id}) catch {};
    return false;
}

pub fn getTimeHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    const written = cron.getTimeStr(result);
    if (written == 0) {
        const msg = "Error: could not get time";
        @memcpy(result[0..msg.len], msg);
        result[msg.len] = 0;
        return false;
    }
    return true;
}

pub fn setTimezoneHandler(
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

    const tz_val = obj.get("timezone") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'timezone' required", .{}) catch {};
        return false;
    };

    const tz = switch (tz_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'timezone' must be a string", .{}) catch {};
            return false;
        },
    };

    cron.setTimezone(tz) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: invalid timezone: {}", .{err}) catch {};
        return false;
    };

    _ = std.fmt.bufPrint(result, "Timezone set to: {s}", .{tz}) catch {};
    return true;
}

pub fn getTimezoneHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    var tz_buf: [config.TIMEZONE_MAX_LEN]u8 = undefined;
    const len = cron.getTimezone(&tz_buf);

    _ = std.fmt.bufPrint(result, "Timezone: {s}", .{tz_buf[0..len]}) catch {};
    return true;
}
