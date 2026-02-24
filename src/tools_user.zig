//! User-defined custom tool management.
//! Replaces user_tools.c. Persists to file-based memory instead of NVS.

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");

const log = std.log.scoped(.user_tools);

pub const UserTool = struct {
    name: [config.TOOL_NAME_MAX_LEN]u8 = std.mem.zeroes([config.TOOL_NAME_MAX_LEN]u8),
    description: [config.TOOL_DESC_MAX_LEN]u8 = std.mem.zeroes([config.TOOL_DESC_MAX_LEN]u8),
    action: [config.CRON_MAX_ACTION_LEN]u8 = std.mem.zeroes([config.CRON_MAX_ACTION_LEN]u8),

    pub fn getName(self: *const UserTool) []const u8 {
        return std.mem.sliceTo(&self.name, 0);
    }
    pub fn getDescription(self: *const UserTool) []const u8 {
        return std.mem.sliceTo(&self.description, 0);
    }
    pub fn getAction(self: *const UserTool) []const u8 {
        return std.mem.sliceTo(&self.action, 0);
    }
};

var g_tools: [config.MAX_DYNAMIC_TOOLS]UserTool = undefined;
var g_tool_count: usize = 0;
var g_mutex: std.Thread.Mutex = .{};
var g_allocator: std.mem.Allocator = undefined;

/// Builtin tool names to reject conflicts with
const BUILTIN_TOOL_NAMES = &[_][]const u8{
    "memory_set", "memory_get", "memory_list", "memory_delete",
    "cron_set", "cron_list", "cron_delete",
    "get_time", "set_timezone", "get_timezone",
    "set_persona", "get_persona", "reset_persona",
    "get_version", "get_health",
    "create_tool", "list_user_tools", "delete_user_tool",
};

fn nameConflictsWithBuiltin(name: []const u8) bool {
    for (BUILTIN_TOOL_NAMES) |builtin| {
        if (std.mem.eql(u8, name, builtin)) return true;
    }
    return false;
}

fn nameIsValid(name: []const u8) bool {
    if (name.len == 0 or name.len >= config.TOOL_NAME_MAX_LEN) return false;
    for (name) |c| {
        if (!std.ascii.isAlphanumeric(c) and c != '_') return false;
    }
    return true;
}

fn saveToMemory() !void {
    // Save as JSON array in memory namespace
    var buf = std.ArrayList(u8).init(g_allocator);
    defer buf.deinit();

    try buf.appendSlice("[");
    for (g_tools[0..g_tool_count], 0..) |*tool, i| {
        if (i > 0) try buf.appendSlice(",");
        try buf.appendSlice("{\"name\":");
        try writeJsonString(buf.writer(), tool.getName());
        try buf.appendSlice(",\"description\":");
        try writeJsonString(buf.writer(), tool.getDescription());
        try buf.appendSlice(",\"action\":");
        try writeJsonString(buf.writer(), tool.getAction());
        try buf.appendSlice("}");
    }
    try buf.appendSlice("]");

    try memory.memSet(config.MEMORY_NAMESPACE_TOOLS, "tools", buf.items);
}

fn writeJsonString(writer: anytype, s: []const u8) !void {
    try writer.writeByte('"');
    for (s) |c| {
        switch (c) {
            '"' => try writer.writeAll("\\\""),
            '\\' => try writer.writeAll("\\\\"),
            '\n' => try writer.writeAll("\\n"),
            '\r' => try writer.writeAll("\\r"),
            '\t' => try writer.writeAll("\\t"),
            else => try writer.writeByte(c),
        }
    }
    try writer.writeByte('"');
}

fn loadFromMemory() void {
    const json_str = memory.memGet(g_allocator, config.MEMORY_NAMESPACE_TOOLS, "tools") catch return orelse return;
    defer g_allocator.free(json_str);

    const parsed = std.json.parseFromSlice(std.json.Value, g_allocator, json_str, .{}) catch {
        log.warn("Failed to parse user tools from storage", .{});
        return;
    };
    defer parsed.deinit();

    const arr = switch (parsed.value) {
        .array => |a| a,
        else => return,
    };

    for (arr.items) |item| {
        const obj = switch (item) {
            .object => |o| o,
            else => continue,
        };

        const name_val = obj.get("name") orelse continue;
        const desc_val = obj.get("description") orelse continue;
        const action_val = obj.get("action") orelse continue;

        const name = switch (name_val) {
            .string => |s| s,
            else => continue,
        };
        const desc = switch (desc_val) {
            .string => |s| s,
            else => continue,
        };
        const action = switch (action_val) {
            .string => |s| s,
            else => continue,
        };

        if (!nameIsValid(name) or nameConflictsWithBuiltin(name)) continue;
        if (g_tool_count >= config.MAX_DYNAMIC_TOOLS) break;

        var tool = UserTool{};
        copyTrunc(&tool.name, name);
        copyTrunc(&tool.description, desc);
        copyTrunc(&tool.action, action);
        g_tools[g_tool_count] = tool;
        g_tool_count += 1;
        log.info("Loaded user tool: {s}", .{name});
    }

    log.info("Loaded {} user tools", .{g_tool_count});
}

fn copyTrunc(dest: []u8, src: []const u8) void {
    const n = @min(src.len, dest.len - 1);
    @memcpy(dest[0..n], src[0..n]);
    dest[n] = 0;
}

pub fn init(allocator: std.mem.Allocator) void {
    g_allocator = allocator;
    g_tool_count = 0;
    g_tools = undefined;
    loadFromMemory();
}

pub fn create(name: []const u8, description: []const u8, action: []const u8) !bool {
    g_mutex.lock();
    defer g_mutex.unlock();

    if (!nameIsValid(name)) {
        log.warn("Invalid tool name: {s}", .{name});
        return false;
    }
    if (nameConflictsWithBuiltin(name)) {
        log.warn("Tool name conflicts with builtin: {s}", .{name});
        return false;
    }
    if (description.len == 0 or action.len == 0) {
        log.warn("Tool description/action must be non-empty", .{});
        return false;
    }

    // Check for duplicate
    for (g_tools[0..g_tool_count]) |*tool| {
        if (std.mem.eql(u8, std.mem.sliceTo(&tool.name, 0), name)) {
            log.warn("Tool already exists: {s}", .{name});
            return false;
        }
    }

    if (g_tool_count >= config.MAX_DYNAMIC_TOOLS) {
        log.warn("Max user tools reached ({})", .{config.MAX_DYNAMIC_TOOLS});
        return false;
    }

    var tool = UserTool{};
    copyTrunc(&tool.name, name);
    copyTrunc(&tool.description, description);
    copyTrunc(&tool.action, action);
    g_tools[g_tool_count] = tool;
    g_tool_count += 1;

    saveToMemory() catch |err| {
        g_tool_count -= 1;
        log.err("Failed to persist user tool: {}", .{err});
        return false;
    };

    log.info("Created user tool: {s}", .{name});
    return true;
}

pub fn delete(name: []const u8) !bool {
    g_mutex.lock();
    defer g_mutex.unlock();

    for (g_tools[0..g_tool_count], 0..) |*tool, i| {
        if (std.mem.eql(u8, std.mem.sliceTo(&tool.name, 0), name)) {
            // Shift remaining
            var j = i;
            while (j < g_tool_count - 1) : (j += 1) {
                g_tools[j] = g_tools[j + 1];
            }
            g_tool_count -= 1;

            try saveToMemory();
            log.info("Deleted user tool: {s}", .{name});
            return true;
        }
    }
    return false;
}

pub fn find(name: []const u8) ?*const UserTool {
    g_mutex.lock();
    defer g_mutex.unlock();

    for (g_tools[0..g_tool_count]) |*tool| {
        if (std.mem.eql(u8, std.mem.sliceTo(&tool.name, 0), name)) {
            return tool;
        }
    }
    return null;
}

pub fn count() usize {
    g_mutex.lock();
    defer g_mutex.unlock();
    return g_tool_count;
}

pub fn getAll() []const UserTool {
    return g_tools[0..g_tool_count];
}

pub fn list(buf: []u8) usize {
    g_mutex.lock();
    defer g_mutex.unlock();

    if (g_tool_count == 0) {
        const msg = "No user tools defined";
        const n = @min(msg.len, buf.len - 1);
        @memcpy(buf[0..n], msg[0..n]);
        buf[n] = 0;
        return n;
    }

    var stream = std.io.fixedBufferStream(buf);
    const writer = stream.writer();

    writer.print("User tools ({}):", .{g_tool_count}) catch {};
    for (g_tools[0..g_tool_count]) |*tool| {
        writer.print("\n  {s} - {s}", .{ tool.getName(), tool.getDescription() }) catch break;
    }

    const written = stream.pos;
    if (written < buf.len) buf[written] = 0;
    return written;
}
