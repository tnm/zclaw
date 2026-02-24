//! Memory tool handlers (memory_set, memory_get, memory_list, memory_delete).
//! Keys must start with "u_" for user namespace safety.

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");

const log = std.log.scoped(.tools_memory);

fn validateUserKey(key: []const u8, result: []u8) bool {
    if (!std.mem.startsWith(u8, key, "u_")) {
        const msg = "Error: key must start with u_";
        const n = @min(msg.len, result.len - 1);
        @memcpy(result[0..n], msg[0..n]);
        result[n] = 0;
        return false;
    }
    if (key.len > config.MEMORY_KEY_MAX_LEN) {
        const n = std.fmt.bufPrint(result, "Error: key too long (max {} chars)", .{config.MEMORY_KEY_MAX_LEN}) catch "";
        _ = n;
        return false;
    }
    return true;
}

pub fn memorySetHandler(
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

    const key_val = obj.get("key") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'key' required", .{}) catch {};
        return false;
    };
    const value_val = obj.get("value") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'value' required", .{}) catch {};
        return false;
    };

    const key = switch (key_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'key' must be a string", .{}) catch {};
            return false;
        },
    };
    const value = switch (value_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'value' must be a string", .{}) catch {};
            return false;
        },
    };

    if (!validateUserKey(key, result)) return false;
    _ = allocator;

    memory.memSet(config.MEMORY_NAMESPACE, key, value) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: storage failed: {}", .{err}) catch {};
        return false;
    };

    _ = std.fmt.bufPrint(result, "Stored: {s} = {s}", .{ key, value }) catch {};
    return true;
}

pub fn memoryGetHandler(
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

    const key_val = obj.get("key") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'key' required", .{}) catch {};
        return false;
    };

    const key = switch (key_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'key' must be a string", .{}) catch {};
            return false;
        },
    };

    if (!validateUserKey(key, result)) return false;

    const value = memory.memGet(allocator, config.MEMORY_NAMESPACE, key) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: storage read failed: {}", .{err}) catch {};
        return false;
    };

    if (value) |v| {
        defer allocator.free(v);
        const n = @min(v.len, result.len - 1);
        @memcpy(result[0..n], v[0..n]);
        result[n] = 0;
    } else {
        _ = std.fmt.bufPrint(result, "Key not found: {s}", .{key}) catch {};
    }

    return true;
}

pub fn memoryListHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = input;

    const keys = memory.memList(allocator, config.MEMORY_NAMESPACE, "u_") catch |err| {
        _ = std.fmt.bufPrint(result, "Error: {}", .{err}) catch {};
        return false;
    };
    defer {
        for (keys) |k| allocator.free(k);
        allocator.free(keys);
    }

    if (keys.len == 0) {
        const msg = "No user memory keys stored";
        @memcpy(result[0..msg.len], msg);
        result[msg.len] = 0;
        return true;
    }

    var stream = std.io.fixedBufferStream(result);
    const writer = stream.writer();
    writer.print("Memory keys ({}):", .{keys.len}) catch {};
    for (keys) |k| {
        writer.print("\n  {s}", .{k}) catch break;
    }
    const pos = stream.pos;
    if (pos < result.len) result[pos] = 0;

    return true;
}

pub fn memoryDeleteHandler(
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

    const key_val = obj.get("key") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'key' required", .{}) catch {};
        return false;
    };

    const key = switch (key_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'key' must be a string", .{}) catch {};
            return false;
        },
    };

    if (!validateUserKey(key, result)) return false;
    _ = allocator;

    memory.memDelete(config.MEMORY_NAMESPACE, key) catch |err| {
        _ = std.fmt.bufPrint(result, "Error: delete failed: {}", .{err}) catch {};
        return false;
    };

    _ = std.fmt.bufPrint(result, "Deleted: {s}", .{key}) catch {};
    return true;
}
