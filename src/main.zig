//! zedclaw - AI agent CLI for desktop.
//! Migrated from ESP32/FreeRTOS C to Zig desktop.
//! Usage: zedclaw [--no-telegram]
//!
//! Configuration via environment variables:
//!   ZEDCLAW_LLM_BACKEND  - anthropic | openai | openrouter | ollama (default: anthropic)
//!   ZEDCLAW_LLM_API_KEY  - API key for the selected backend
//!   ZEDCLAW_LLM_MODEL    - Optional model override
//!   ZEDCLAW_LLM_API_URL  - Optional API URL override (for Ollama endpoint etc.)
//!   ZEDCLAW_TELEGRAM_TOKEN - Optional Telegram bot token
//!   ZEDCLAW_DATA_DIR     - Data directory (default: ~/.config/zedclaw)
//!   ZEDCLAW_TIMEZONE     - Timezone label (default: UTC)
//!
//! Or load from .env file in current directory.

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");
const ratelimit = @import("ratelimit.zig");
const tools = @import("tools.zig");
const tools_persona = @import("tools_persona.zig");
const cron = @import("cron.zig");
const channel = @import("channel.zig");
const telegram = @import("telegram.zig");
const agent = @import("agent.zig");
const llm = @import("llm.zig");

const log = std.log.scoped(.main);

// Global message queue (used by all producers: stdin, cron, telegram)
var g_input_queue: channel.MessageQueue = .{};
var g_telegram_output: telegram.TelegramOutputQueue = .{};

/// Load .env file if present in current directory.
fn loadDotEnv(allocator: std.mem.Allocator) void {
    const file = std.fs.cwd().openFile(".env", .{}) catch return;
    defer file.close();

    const content = file.readToEndAlloc(allocator, 64 * 1024) catch return;
    defer allocator.free(content);

    var it = std.mem.splitScalar(u8, content, '\n');
    while (it.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (trimmed.len == 0 or trimmed[0] == '#') continue;

        const eq = std.mem.indexOfScalar(u8, trimmed, '=') orelse continue;
        const key = std.mem.trim(u8, trimmed[0..eq], " \t");
        const val = std.mem.trim(u8, trimmed[eq + 1 ..], " \t\"'");

        if (key.len == 0) continue;

        // Only set if not already set in environment
        if (std.posix.getenv(key) == null) {
            // setenv is available on POSIX
            std.posix.setenv(key, val) catch {};
        }
    }
}

/// Cron fire callback: pushes triggered actions to the agent input queue.
fn cronFireCallback(action: []const u8, id: u8) void {
    var buf: [config.CHANNEL_RX_BUF_SIZE]u8 = undefined;
    const msg_text = std.fmt.bufPrint(&buf, "[CRON {}] {s}", .{ id, action }) catch return;
    channel.pushMessage(msg_text, .cron, 0);
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // Print banner
    const stderr = std.io.getStdErr().writer();
    try stderr.print(
        "\n" ++
        "╔══════════════════════════════════════╗\n" ++
        "║  zedclaw v{s:<26} ║\n" ++
        "║  AI Agent CLI                        ║\n" ++
        "╚══════════════════════════════════════╝\n\n",
        .{config.VERSION},
    );

    // Load .env file if present
    loadDotEnv(allocator);

    // Load runtime configuration
    const cfg = try config.RuntimeConfig.load(allocator);

    try stderr.print("Backend: {s} | Model: {s}\n", .{ cfg.llm_backend.name(), cfg.model() });
    try stderr.print("Data dir: {s}\n\n", .{cfg.data_dir});

    // Initialize memory store
    try memory.init(allocator, cfg.data_dir);

    // Initialize rate limiter
    ratelimit.init(allocator);

    // Initialize tools (including user tools)
    tools.init(allocator);

    // Initialize persona
    tools_persona.init(allocator);

    // Initialize cron scheduler
    cron.init(allocator);
    cron.setFireCallback(cronFireCallback);

    // Initialize LLM client
    llm.init(allocator, &cfg);

    // Initialize channel (stdin/stdout)
    channel.init(&g_input_queue);

    // Initialize agent
    const telegram_output: ?*telegram.TelegramOutputQueue = if (cfg.telegram_token.len > 0)
        &g_telegram_output
    else
        null;

    agent.init(allocator, &cfg, &g_input_queue, telegram_output);

    // Start cron background thread
    const cron_thread = try std.Thread.spawn(.{}, cron.runBackground, .{});
    cron_thread.detach();

    // Start Telegram if configured
    if (cfg.telegram_token.len > 0) {
        telegram.init(allocator, cfg.telegram_token, &g_input_queue, &g_telegram_output);
        try stderr.print("Telegram: enabled\n\n", .{});

        const tg_poll_thread = try std.Thread.spawn(.{}, telegram.pollLoop, .{});
        tg_poll_thread.detach();

        const tg_send_thread = try std.Thread.spawn(.{}, telegram.sendLoop, .{});
        tg_send_thread.detach();
    } else {
        try stderr.print("Telegram: disabled (set ZEDCLAW_TELEGRAM_TOKEN to enable)\n\n", .{});
    }

    // Start agent loop in background thread (processes messages from queue)
    const agent_thread = try std.Thread.spawn(.{}, agent.run, .{});
    agent_thread.detach();

    // Main thread: read from stdin and push to input queue
    // This blocks until EOF or /exit
    channel.readLoop();

    log.info("zedclaw exiting", .{});
}

// Standard library logging override
pub const std_options = std.Options{
    .log_level = .info,
    .logFn = logFn,
};

fn logFn(
    comptime level: std.log.Level,
    comptime scope: @TypeOf(.enum_literal),
    comptime format: []const u8,
    args: anytype,
) void {
    const prefix = switch (level) {
        .err => "[ERR]",
        .warn => "[WRN]",
        .info => "[INF]",
        .debug => "[DBG]",
    };

    const scope_str = @tagName(scope);
    const stderr = std.io.getStdErr().writer();
    nosuspend stderr.print(prefix ++ " [{s}] " ++ format ++ "\n", .{scope_str} ++ args) catch {};
}
