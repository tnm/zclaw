//! Stdin/stdout I/O channel for desktop CLI.
//! Replaces channel.c (USB serial/UART replaced with stdin/stdout).
//! Provides a thread-safe message queue for the agent.

const std = @import("std");
const config = @import("config.zig");

const log = std.log.scoped(.channel);

pub const MessageSource = enum(u8) {
    channel = 0,
    telegram = 1,
    cron = 2,
};

pub const Message = struct {
    text: [config.CHANNEL_RX_BUF_SIZE]u8 = std.mem.zeroes([config.CHANNEL_RX_BUF_SIZE]u8),
    source: MessageSource = .channel,
    chat_id: i64 = 0,

    pub fn getText(self: *const Message) []const u8 {
        return std.mem.sliceTo(&self.text, 0);
    }

    pub fn setText(self: *Message, text: []const u8) void {
        const n = @min(text.len, config.CHANNEL_RX_BUF_SIZE - 1);
        @memcpy(self.text[0..n], text[0..n]);
        self.text[n] = 0;
    }
};

/// Thread-safe bounded message queue.
pub const MessageQueue = struct {
    mutex: std.Thread.Mutex = .{},
    cond: std.Thread.Condition = .{},
    items: [config.INPUT_QUEUE_MAX_LEN]Message = undefined,
    head: usize = 0,
    tail: usize = 0,
    len: usize = 0,

    pub fn push(self: *MessageQueue, msg: Message) bool {
        self.mutex.lock();
        defer self.mutex.unlock();

        if (self.len >= config.INPUT_QUEUE_MAX_LEN) {
            log.warn("Input queue full, dropping message", .{});
            return false;
        }

        self.items[self.tail] = msg;
        self.tail = (self.tail + 1) % config.INPUT_QUEUE_MAX_LEN;
        self.len += 1;
        self.cond.signal();
        return true;
    }

    /// Block until a message is available, then return it.
    pub fn pop(self: *MessageQueue) Message {
        self.mutex.lock();
        defer self.mutex.unlock();

        while (self.len == 0) {
            self.cond.wait(&self.mutex);
        }

        const msg = self.items[self.head];
        self.head = (self.head + 1) % config.INPUT_QUEUE_MAX_LEN;
        self.len -= 1;
        return msg;
    }

    /// Non-blocking pop. Returns null if empty.
    pub fn tryPop(self: *MessageQueue) ?Message {
        self.mutex.lock();
        defer self.mutex.unlock();

        if (self.len == 0) return null;

        const msg = self.items[self.head];
        self.head = (self.head + 1) % config.INPUT_QUEUE_MAX_LEN;
        self.len -= 1;
        return msg;
    }
};

var g_input_queue: *MessageQueue = undefined;
var g_stdout: std.io.AnyWriter = undefined;
var g_stdout_mutex: std.Thread.Mutex = .{};

pub fn init(input_queue: *MessageQueue) void {
    g_input_queue = input_queue;
    g_stdout = std.io.getStdOut().writer().any();
    log.info("Channel initialized (stdin/stdout)", .{});
}

/// Write text to stdout (thread-safe).
pub fn write(text: []const u8) void {
    g_stdout_mutex.lock();
    defer g_stdout_mutex.unlock();

    g_stdout.writeAll(text) catch {};
    g_stdout.writeAll("\n\n") catch {};
}

/// Push a message into the input queue (for external sources like cron/telegram).
pub fn pushMessage(text: []const u8, source: MessageSource, chat_id: i64) void {
    var msg = Message{};
    msg.setText(text);
    msg.source = source;
    msg.chat_id = chat_id;
    _ = g_input_queue.push(msg);
}

/// Read loop: reads lines from stdin and pushes to input queue.
/// Runs until EOF.
pub fn readLoop() void {
    const stdin = std.io.getStdIn().reader();
    var line_buf: [config.CHANNEL_RX_BUF_SIZE]u8 = undefined;

    write("zedclaw ready. Type a message and press Enter. Ctrl-D or /exit to quit.\n");

    while (true) {
        // Print prompt
        {
            g_stdout_mutex.lock();
            g_stdout.writeAll("> ") catch {};
            g_stdout_mutex.unlock();
        }

        const line = stdin.readUntilDelimiter(&line_buf, '\n') catch |err| switch (err) {
            error.EndOfStream => {
                log.info("EOF received, shutting down", .{});
                // Push an exit signal
                pushMessage("/exit", .channel, 0);
                return;
            },
            else => {
                log.err("Read error: {}", .{err});
                return;
            },
        };

        // Trim carriage return
        const trimmed = std.mem.trimRight(u8, line, "\r");
        if (trimmed.len == 0) continue;

        pushMessage(trimmed, .channel, 0);

        // Check for exit command
        if (std.mem.eql(u8, trimmed, "/exit") or std.mem.eql(u8, trimmed, "/quit")) {
            return;
        }
    }
}
