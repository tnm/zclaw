//! File-based key-value persistent memory store.
//! Replaces ESP-IDF NVS (Non-Volatile Storage).
//! Stores data as JSON files under the data_dir, one per namespace.

const std = @import("std");
const config = @import("config.zig");

const log = std.log.scoped(.memory);

/// One store file per namespace (e.g. "zclaw.json", "zc_cron.json")
var g_data_dir: []const u8 = "";
var g_mutex: std.Thread.Mutex = .{};
var g_allocator: std.mem.Allocator = undefined;

pub fn init(allocator: std.mem.Allocator, data_dir: []const u8) !void {
    g_allocator = allocator;
    g_data_dir = data_dir;

    // Ensure directory exists
    std.fs.makeDirAbsolute(data_dir) catch |err| switch (err) {
        error.PathAlreadyExists => {},
        else => return err,
    };

    log.info("Memory store initialized at: {s}", .{data_dir});
}

fn storeFilePath(allocator: std.mem.Allocator, namespace: []const u8) ![]const u8 {
    const filename = try std.fmt.allocPrint(allocator, "{s}.json", .{namespace});
    defer allocator.free(filename);
    return std.fs.path.join(allocator, &.{ g_data_dir, filename });
}

/// Load the JSON object for a namespace. Caller owns returned parsed value.
fn loadNamespace(allocator: std.mem.Allocator, namespace: []const u8) !std.json.Parsed(std.json.Value) {
    const path = try storeFilePath(allocator, namespace);
    defer allocator.free(path);

    const file_contents = std.fs.cwd().readFileAlloc(allocator, path, 1 << 20) catch |err| switch (err) {
        error.FileNotFound => return std.json.parseFromSlice(std.json.Value, allocator, "{}", .{}),
        else => return err,
    };
    defer allocator.free(file_contents);

    return std.json.parseFromSlice(std.json.Value, allocator, file_contents, .{}) catch {
        log.warn("Corrupt store file {s}, resetting", .{path});
        return std.json.parseFromSlice(std.json.Value, allocator, "{}", .{});
    };
}

/// Save an object to a namespace file atomically.
fn saveNamespace(allocator: std.mem.Allocator, namespace: []const u8, obj: std.json.ObjectMap) !void {
    const path = try storeFilePath(allocator, namespace);
    defer allocator.free(path);

    var buf = std.ArrayList(u8).init(allocator);
    defer buf.deinit();

    try std.json.stringify(std.json.Value{ .object = obj }, .{ .whitespace = .indent_2 }, buf.writer());

    // Write atomically via temp file
    const tmp_path = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp_path);

    const file = try std.fs.cwd().createFile(tmp_path, .{});
    try file.writeAll(buf.items);
    file.close();

    try std.fs.cwd().rename(tmp_path, path);
}

/// Set a key in the given namespace.
pub fn memSet(namespace: []const u8, key: []const u8, value: []const u8) !void {
    g_mutex.lock();
    defer g_mutex.unlock();

    var arena = std.heap.ArenaAllocator.init(g_allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    var parsed = try loadNamespace(alloc, namespace);
    defer parsed.deinit();

    var obj = parsed.value.object;
    const key_copy = try alloc.dupe(u8, key);
    const val_json = std.json.Value{ .string = value };
    try obj.put(key_copy, val_json);

    try saveNamespace(alloc, namespace, obj);

    if (isSensitiveKey(key)) {
        log.info("Stored: {s}:{s} = <redacted>", .{ namespace, key });
    } else {
        log.info("Stored: {s}:{s} = {s}", .{ namespace, key, value });
    }
}

/// Get a key from the given namespace. Returns allocated slice or null.
pub fn memGet(allocator: std.mem.Allocator, namespace: []const u8, key: []const u8) !?[]u8 {
    g_mutex.lock();
    defer g_mutex.unlock();

    var arena = std.heap.ArenaAllocator.init(g_allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    var parsed = try loadNamespace(alloc, namespace);
    defer parsed.deinit();

    const obj = parsed.value.object;
    const val = obj.get(key) orelse return null;

    switch (val) {
        .string => |s| {
            const result = try allocator.dupe(u8, s);
            if (isSensitiveKey(key)) {
                log.info("Retrieved: {s}:{s} = <redacted>", .{ namespace, key });
            } else {
                log.info("Retrieved: {s}:{s} = {s}", .{ namespace, key, result });
            }
            return result;
        },
        else => return null,
    }
}

/// Delete a key from the given namespace.
pub fn memDelete(namespace: []const u8, key: []const u8) !void {
    g_mutex.lock();
    defer g_mutex.unlock();

    var arena = std.heap.ArenaAllocator.init(g_allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    var parsed = try loadNamespace(alloc, namespace);
    defer parsed.deinit();

    var obj = parsed.value.object;
    _ = obj.fetchRemove(key);

    try saveNamespace(alloc, namespace, obj);
    log.info("Deleted: {s}:{s}", .{ namespace, key });
}

/// List all keys in a namespace matching an optional prefix filter.
pub fn memList(allocator: std.mem.Allocator, namespace: []const u8, prefix: ?[]const u8) ![][]u8 {
    g_mutex.lock();
    defer g_mutex.unlock();

    var arena = std.heap.ArenaAllocator.init(g_allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    var parsed = try loadNamespace(alloc, namespace);
    defer parsed.deinit();

    const obj = parsed.value.object;
    var keys = std.ArrayList([]u8).init(allocator);
    errdefer {
        for (keys.items) |k| allocator.free(k);
        keys.deinit();
    }

    var it = obj.iterator();
    while (it.next()) |entry| {
        const k = entry.key_ptr.*;
        if (prefix) |pfx| {
            if (!std.mem.startsWith(u8, k, pfx)) continue;
        }
        try keys.append(try allocator.dupe(u8, k));
    }

    return keys.toOwnedSlice();
}

/// Save a raw blob (as hex-encoded string for simplicity) to a namespace.
pub fn memSetBlob(namespace: []const u8, key: []const u8, data: []const u8) !void {
    var arena = std.heap.ArenaAllocator.init(g_allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    // Encode as base64
    const encoder = std.base64.standard.Encoder;
    const encoded_len = encoder.calcSize(data.len);
    const encoded = try alloc.alloc(u8, encoded_len);
    _ = encoder.encode(encoded, data);

    try memSet(namespace, key, encoded);
}

/// Get a raw blob from namespace. Returns allocated decoded slice or null.
pub fn memGetBlob(allocator: std.mem.Allocator, namespace: []const u8, key: []const u8) !?[]u8 {
    const encoded = try memGet(g_allocator, namespace, key) orelse return null;
    defer g_allocator.free(encoded);

    const decoder = std.base64.standard.Decoder;
    const decoded_len = try decoder.calcSizeForSlice(encoded);
    const decoded = try allocator.alloc(u8, decoded_len);
    errdefer allocator.free(decoded);

    try decoder.decode(decoded, encoded);
    return decoded;
}

fn isSensitiveKey(key: []const u8) bool {
    const sensitive = &[_][]const u8{
        config.KEY_LLM_API_KEY,
        config.KEY_TELEGRAM_TOKEN,
        "password",
        "secret",
        "token",
    };
    for (sensitive) |s| {
        if (std.mem.eql(u8, key, s)) return true;
    }
    return false;
}
