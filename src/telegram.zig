//! Telegram bot long-polling for desktop.
//! Replaces telegram.c. Uses std.http.Client instead of esp_http_client.
//! Runs as a background thread. Requires ZEDCLAW_TELEGRAM_TOKEN env var.

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");
const channel = @import("channel.zig");

const log = std.log.scoped(.telegram);

var g_token: []const u8 = "";
var g_last_update_id: i64 = 0;
var g_input_queue: *channel.MessageQueue = undefined;
var g_allocator: std.mem.Allocator = undefined;
var g_telegram_output: ?*TelegramOutputQueue = null;

pub const TelegramMsg = struct {
    text: [config.TELEGRAM_MAX_MSG_LEN]u8 = std.mem.zeroes([config.TELEGRAM_MAX_MSG_LEN]u8),
    chat_id: i64 = 0,

    pub fn getText(self: *const TelegramMsg) []const u8 {
        return std.mem.sliceTo(&self.text, 0);
    }
};

pub const TelegramOutputQueue = struct {
    mutex: std.Thread.Mutex = .{},
    cond: std.Thread.Condition = .{},
    items: [16]TelegramMsg = undefined,
    head: usize = 0,
    tail: usize = 0,
    len: usize = 0,

    pub fn push(self: *TelegramOutputQueue, msg: TelegramMsg) bool {
        self.mutex.lock();
        defer self.mutex.unlock();
        if (self.len >= 16) return false;
        self.items[self.tail] = msg;
        self.tail = (self.tail + 1) % 16;
        self.len += 1;
        self.cond.signal();
        return true;
    }

    pub fn pop(self: *TelegramOutputQueue) TelegramMsg {
        self.mutex.lock();
        defer self.mutex.unlock();
        while (self.len == 0) self.cond.wait(&self.mutex);
        const msg = self.items[self.head];
        self.head = (self.head + 1) % 16;
        self.len -= 1;
        return msg;
    }
};

pub fn isConfigured() bool {
    return g_token.len > 0;
}

pub fn init(
    allocator: std.mem.Allocator,
    token: []const u8,
    input_queue: *channel.MessageQueue,
    output_queue: *TelegramOutputQueue,
) void {
    g_allocator = allocator;
    g_token = token;
    g_input_queue = input_queue;
    g_telegram_output = output_queue;

    if (config.TELEGRAM_FLUSH_ON_START) {
        flushBacklog() catch |err| {
            log.warn("Failed to flush Telegram backlog: {}", .{err});
        };
    }

    log.info("Telegram initialized", .{});
}

fn buildApiUrl(allocator: std.mem.Allocator, method: []const u8) ![]u8 {
    return std.fmt.allocPrint(allocator, "{s}{s}/{s}", .{
        config.TELEGRAM_API_BASE,
        g_token,
        method,
    });
}

/// Flush pending updates at startup to avoid processing stale messages.
fn flushBacklog() !void {
    var client = std.http.Client{ .allocator = g_allocator };
    defer client.deinit();

    const url = try buildApiUrl(g_allocator, "getUpdates?timeout=0&offset=-1");
    defer g_allocator.free(url);

    var body = std.ArrayList(u8).init(g_allocator);
    defer body.deinit();

    const result = try client.fetch(.{
        .location = .{ .url = url },
        .method = .GET,
        .response_storage = .{ .dynamic = &body },
        .max_append_size = 65536,
    });

    if (result.status != .ok) return;

    const parsed = std.json.parseFromSlice(std.json.Value, g_allocator, body.items, .{}) catch return;
    defer parsed.deinit();

    const root_obj = switch (parsed.value) {
        .object => |o| o,
        else => return,
    };

    const updates = root_obj.get("result") orelse return;
    const arr = switch (updates) {
        .array => |a| a,
        else => return,
    };

    if (arr.items.len > 0) {
        const last = arr.items[arr.items.len - 1];
        if (switch (last) {
            .object => |o| o.get("update_id"),
            else => null,
        }) |uid| {
            g_last_update_id = switch (uid) {
                .integer => |n| n + 1,
                else => 0,
            };
            log.info("Flushed Telegram backlog, next offset: {}", .{g_last_update_id});
        }
    }
}

/// Long-poll for updates and push to input queue.
fn pollUpdates() !void {
    var client = std.http.Client{ .allocator = g_allocator };
    defer client.deinit();

    const url = try std.fmt.allocPrint(g_allocator,
        "{s}{s}/getUpdates?timeout={}&offset={}",
        .{
            config.TELEGRAM_API_BASE,
            g_token,
            config.TELEGRAM_POLL_TIMEOUT,
            g_last_update_id,
        });
    defer g_allocator.free(url);

    var body = std.ArrayList(u8).init(g_allocator);
    defer body.deinit();

    const result = try client.fetch(.{
        .location = .{ .url = url },
        .method = .GET,
        .response_storage = .{ .dynamic = &body },
        .max_append_size = 65536,
    });

    if (result.status != .ok) {
        log.warn("Telegram poll failed: HTTP {}", .{@intFromEnum(result.status)});
        return;
    }

    if (body.items.len == 0) return;

    const parsed = std.json.parseFromSlice(std.json.Value, g_allocator, body.items, .{}) catch |err| {
        log.warn("Failed to parse Telegram response: {}", .{err});
        return;
    };
    defer parsed.deinit();

    const root_obj = switch (parsed.value) {
        .object => |o| o,
        else => return,
    };

    const ok_val = root_obj.get("ok") orelse return;
    const is_ok = switch (ok_val) {
        .bool => |b| b,
        else => false,
    };
    if (!is_ok) return;

    const updates = root_obj.get("result") orelse return;
    const arr = switch (updates) {
        .array => |a| a,
        else => return,
    };

    for (arr.items) |update| {
        const upd_obj = switch (update) {
            .object => |o| o,
            else => continue,
        };

        const uid = upd_obj.get("update_id") orelse continue;
        const update_id = switch (uid) {
            .integer => |n| n,
            else => continue,
        };

        g_last_update_id = update_id + 1;

        const msg_val = upd_obj.get("message") orelse continue;
        const msg_obj = switch (msg_val) {
            .object => |o| o,
            else => continue,
        };

        const chat_val = msg_obj.get("chat") orelse continue;
        const chat_obj = switch (chat_val) {
            .object => |o| o,
            else => continue,
        };
        const chat_id_val = chat_obj.get("id") orelse continue;
        const chat_id = switch (chat_id_val) {
            .integer => |n| n,
            else => continue,
        };

        const text_val = msg_obj.get("text") orelse continue;
        const text = switch (text_val) {
            .string => |s| s,
            else => continue,
        };

        // Push to agent input queue
        var in_msg = channel.Message{};
        in_msg.setText(text);
        in_msg.source = .telegram;
        in_msg.chat_id = chat_id;
        _ = g_input_queue.push(in_msg);

        log.info("Received Telegram message from chat {}: {s}", .{ chat_id, text[0..@min(text.len, 64)] });
    }
}

/// Send a message to a Telegram chat.
pub fn sendMessage(chat_id: i64, text: []const u8) !void {
    var client = std.http.Client{ .allocator = g_allocator };
    defer client.deinit();

    const url = try buildApiUrl(g_allocator, "sendMessage");
    defer g_allocator.free(url);

    // Build request body
    var req_body = std.ArrayList(u8).init(g_allocator);
    defer req_body.deinit();

    try req_body.appendSlice("{\"chat_id\":");
    try req_body.writer().print("{}", .{chat_id});
    try req_body.appendSlice(",\"text\":\"");
    for (text) |c| {
        switch (c) {
            '"' => try req_body.appendSlice("\\\""),
            '\\' => try req_body.appendSlice("\\\\"),
            '\n' => try req_body.appendSlice("\\n"),
            else => try req_body.append(c),
        }
    }
    try req_body.appendSlice("\"}");

    var body = std.ArrayList(u8).init(g_allocator);
    defer body.deinit();

    const result = try client.fetch(.{
        .location = .{ .url = url },
        .method = .POST,
        .payload = req_body.items,
        .extra_headers = &.{.{ .name = "content-type", .value = "application/json" }},
        .response_storage = .{ .dynamic = &body },
        .max_append_size = 4096,
    });

    if (result.status != .ok) {
        log.warn("Failed to send Telegram message: HTTP {}", .{@intFromEnum(result.status)});
    }
}

/// Background polling loop.
pub fn pollLoop() void {
    if (!isConfigured()) {
        log.warn("Telegram not configured, skipping poll loop", .{});
        return;
    }

    log.info("Telegram polling started", .{});

    while (true) {
        pollUpdates() catch |err| {
            log.warn("Telegram poll error: {}", .{err});
            std.time.sleep(config.TELEGRAM_POLL_INTERVAL_MS * std.time.ns_per_ms * 10);
        };
    }
}

/// Background send loop.
pub fn sendLoop() void {
    if (!isConfigured()) return;
    const output = g_telegram_output orelse return;

    while (true) {
        const msg = output.pop();
        sendMessage(msg.chat_id, msg.getText()) catch |err| {
            log.warn("Failed to send Telegram message: {}", .{err});
        };
    }
}

/// Send a startup notification.
pub fn sendStartupNotification(chat_id: i64) void {
    sendMessage(chat_id, "zedclaw online.") catch {};
}
