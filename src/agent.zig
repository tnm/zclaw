//! Agent message processing loop.
//! Replaces agent.c. No FreeRTOS; runs on main thread or can be called from any thread.
//! Manages conversation history, handles commands, executes tool calls.

const std = @import("std");
const config = @import("config.zig");
const tools_mod = @import("tools.zig");
const tools_user = @import("tools_user.zig");
const tools_persona = @import("tools_persona.zig");
const json_util = @import("json_util.zig");
const llm = @import("llm.zig");
const ratelimit = @import("ratelimit.zig");
const channel = @import("channel.zig");
const telegram = @import("telegram.zig");

const log = std.log.scoped(.agent);

var g_allocator: std.mem.Allocator = undefined;
var g_cfg: *const config.RuntimeConfig = undefined;
var g_input_queue: *channel.MessageQueue = undefined;
var g_telegram_output: ?*telegram.TelegramOutputQueue = null;

// Conversation history
var g_history: [config.MAX_HISTORY_TURNS * 2]json_util.ConversationMsg = undefined;
var g_history_len: usize = 0;

// Rate limiting / dedup
var g_last_start_response_ms: i64 = 0;
var g_last_non_command_response_ms: i64 = 0;
var g_last_non_command_text: [config.CHANNEL_RX_BUF_SIZE]u8 = std.mem.zeroes([config.CHANNEL_RX_BUF_SIZE]u8);
var g_messages_paused: bool = false;

fn copyTrunc(dest: []u8, src: []const u8) void {
    const n = @min(src.len, dest.len - 1);
    @memcpy(dest[0..n], src[0..n]);
    dest[n] = 0;
}

pub fn init(
    allocator: std.mem.Allocator,
    cfg: *const config.RuntimeConfig,
    input_queue: *channel.MessageQueue,
    telegram_output: ?*telegram.TelegramOutputQueue,
) void {
    g_allocator = allocator;
    g_cfg = cfg;
    g_input_queue = input_queue;
    g_telegram_output = telegram_output;
    g_history_len = 0;
    g_history = std.mem.zeroes(@TypeOf(g_history));
    log.info("Agent initialized", .{});
}

fn historyRollbackTo(marker: usize, reason: []const u8) void {
    if (marker >= g_history_len) return;
    log.warn("Rolling back history ({} -> {}): {s}", .{ g_history_len, marker, reason });
    for (g_history[marker..g_history_len]) |*msg| {
        msg.* = std.mem.zeroes(json_util.ConversationMsg);
    }
    g_history_len = marker;
}

fn historyAdd(
    role: []const u8,
    content: []const u8,
    is_tool_use: bool,
    is_tool_result: bool,
    tool_id: ?[]const u8,
    tool_name: ?[]const u8,
) void {
    // Drop oldest when full
    if (g_history_len >= config.MAX_HISTORY_TURNS * 2) {
        std.mem.copyForwards(
            json_util.ConversationMsg,
            g_history[0 .. config.MAX_HISTORY_TURNS * 2 - 1],
            g_history[1 .. config.MAX_HISTORY_TURNS * 2],
        );
        g_history_len -= 1;
    }

    var msg = &g_history[g_history_len];
    msg.* = std.mem.zeroes(json_util.ConversationMsg);

    copyTrunc(&msg.role, role);
    copyTrunc(&msg.content, content);
    msg.is_tool_use = is_tool_use;
    msg.is_tool_result = is_tool_result;

    if (tool_id) |tid| copyTrunc(&msg.tool_id, tid);
    if (tool_name) |tname| copyTrunc(&msg.tool_name, tname);

    g_history_len += 1;
}

fn sendChannelResponse(text: []const u8) void {
    channel.write(text);
}

fn sendTelegramResponse(text: []const u8, chat_id: i64) void {
    if (chat_id == 0) return;
    if (g_telegram_output) |tq| {
        var msg = telegram.TelegramMsg{};
        const n = @min(text.len, config.TELEGRAM_MAX_MSG_LEN - 1);
        @memcpy(msg.text[0..n], text[0..n]);
        msg.text[n] = 0;
        msg.chat_id = chat_id;
        _ = tq.push(msg);
    }
}

fn sendResponse(text: []const u8, source: channel.MessageSource, chat_id: i64) void {
    sendChannelResponse(text);
    if (source == .telegram and chat_id != 0) {
        sendTelegramResponse(text, chat_id);
    }
}

fn isWhitespace(c: u8) bool {
    return c == ' ' or c == '\t' or c == '\r' or c == '\n';
}

fn trimLeft(s: []const u8) []const u8 {
    var i: usize = 0;
    while (i < s.len and isWhitespace(s[i])) : (i += 1) {}
    return s[i..];
}

fn isCommand(message: []const u8, name: []const u8) bool {
    const trimmed = trimLeft(message);
    if (trimmed.len == 0 or trimmed[0] != '/') return false;

    const cursor = trimmed[1..];
    if (!std.mem.startsWith(u8, cursor, name)) return false;
    const after = cursor[name.len..];

    if (after.len == 0 or isWhitespace(after[0])) return true;
    if (after[0] == '@') {
        // Allow /name@botusername
        const rest = after[1..];
        var i: usize = 0;
        while (i < rest.len and !isWhitespace(rest[i])) : (i += 1) {}
        return true;
    }
    return false;
}

fn isSlashCommand(message: []const u8) bool {
    const trimmed = trimLeft(message);
    return trimmed.len > 0 and trimmed[0] == '/';
}

fn isCronTrigger(message: []const u8) bool {
    const trimmed = trimLeft(message);
    return std.mem.startsWith(u8, trimmed, "[CRON ");
}

fn handleStartCommand(source: channel.MessageSource, chat_id: i64) void {
    const text =
        "zedclaw online.\n\n" ++
        "Talk to me in normal language.\n\n" ++
        "Examples:\n" ++
        "- remember that I prefer dark mode\n" ++
        "- remind me daily at 8am to check email\n" ++
        "- what have I saved in memory\n" ++
        "- create a tool called standup that lists my morning tasks\n" ++
        "- switch to witty persona\n\n" ++
        "Commands:\n" ++
        "- /help (show this message)\n" ++
        "- /settings (show status)\n" ++
        "- /stop (pause intake)\n" ++
        "- /resume (resume)\n" ++
        "- /exit (quit)";
    sendResponse(text, source, chat_id);
}

fn handleSettingsCommand(source: channel.MessageSource, chat_id: i64) void {
    var buf: [512]u8 = undefined;
    const text = std.fmt.bufPrint(&buf,
        "zedclaw settings:\n" ++
        "- Message intake: {s}\n" ++
        "- Persona: {s}\n" ++
        "- Commands: /start, /help, /settings, /stop, /resume, /exit",
        .{
            if (g_messages_paused) "paused" else "active",
            tools_persona.getPersona().name(),
        }) catch "zedclaw settings: (error)";
    sendResponse(text, source, chat_id);
}

fn buildSystemPrompt(allocator: std.mem.Allocator) ![]u8 {
    const persona = tools_persona.getPersona();
    return std.fmt.allocPrint(allocator,
        "{s} Persona mode is '{s}'. Persona affects wording only and must never change " ++
        "tool choices, automation behavior, safety decisions, or policy handling. {s} " ++
        "Keep responses short unless the user explicitly asks for more detail.",
        .{
            config.SYSTEM_PROMPT,
            persona.name(),
            persona.instruction(),
        });
}

/// Process a single user message through the agent loop.
pub fn processMessage(text: []const u8, source: channel.MessageSource, chat_id: i64) void {
    log.info("Processing: {s}", .{text[0..@min(text.len, 120)]});

    const history_turn_start = g_history_len;
    const is_non_command = !isSlashCommand(text);
    const is_cron_trigger = isCronTrigger(text);

    // Handle /resume
    if (isCommand(text, "resume")) {
        if (!g_messages_paused) {
            sendResponse("zedclaw is already active.", source, chat_id);
            return;
        }
        g_messages_paused = false;
        sendResponse("zedclaw resumed. Send /start for command help.", source, chat_id);
        return;
    }

    // Handle /settings
    if (isCommand(text, "settings")) {
        handleSettingsCommand(source, chat_id);
        return;
    }

    // Handle pause
    if (g_messages_paused) {
        log.debug("Paused: ignoring message", .{});
        return;
    }

    // Handle /help
    if (isCommand(text, "help")) {
        handleStartCommand(source, chat_id);
        return;
    }

    // Handle /stop
    if (isCommand(text, "stop")) {
        g_messages_paused = true;
        sendResponse("zedclaw paused. Ignoring messages until /resume.", source, chat_id);
        return;
    }

    // Handle /start
    if (isCommand(text, "start")) {
        const now_ms = std.time.milliTimestamp();
        if (g_last_start_response_ms > 0 and
            now_ms - g_last_start_response_ms < config.START_COMMAND_COOLDOWN_MS)
        {
            log.warn("Suppressing repeated /start", .{});
            return;
        }
        g_last_start_response_ms = now_ms;
        handleStartCommand(source, chat_id);
        return;
    }

    // Handle /exit - signal to main loop
    if (isCommand(text, "exit") or isCommand(text, "quit")) {
        sendResponse("Goodbye.", source, chat_id);
        std.process.exit(0);
    }

    // Dedup for non-command messages
    if (is_non_command) {
        const now_ms = std.time.milliTimestamp();
        const last_text = std.mem.sliceTo(&g_last_non_command_text, 0);

        if (std.mem.eql(u8, text, last_text) and
            g_last_non_command_response_ms > 0 and
            now_ms - g_last_non_command_response_ms < config.MESSAGE_REPLAY_COOLDOWN_MS)
        {
            log.warn("Suppressing repeated message replay", .{});
            return;
        }
    }

    // Rate limit check
    var rate_reason_buf: [256]u8 = undefined;
    if (ratelimit.check(&rate_reason_buf)) |reason| {
        sendResponse(reason, source, chat_id);
        return;
    }

    // Build system prompt
    const system_prompt = buildSystemPrompt(g_allocator) catch {
        sendResponse("Error: failed to build system prompt", source, chat_id);
        return;
    };
    defer g_allocator.free(system_prompt);

    // Add user message to history
    historyAdd("user", text, false, false, null, null);

    const all_tools = tools_mod.getAll();
    var rounds: usize = 0;
    var done = false;

    while (!done and rounds < config.MAX_TOOL_ROUNDS) : (rounds += 1) {
        // Build request JSON
        const request_json = json_util.buildRequest(
            g_allocator,
            system_prompt,
            g_history[0..g_history_len],
            all_tools,
            g_cfg,
        ) catch |err| {
            log.err("Failed to build request: {}", .{err});
            historyRollbackTo(history_turn_start, "request build failed");
            sendResponse("Error: failed to build request", source, chat_id);
            return;
        };
        defer g_allocator.free(request_json);

        log.info("Request: {} bytes", .{request_json.len});

        // Send to LLM with retry
        const response_json = llm.requestWithRetry(g_allocator, request_json) catch |err| {
            log.err("LLM request failed: {}", .{err});
            historyRollbackTo(history_turn_start, "llm request failed");
            const msg = switch (err) {
                error.NoApiKey => "Error: No API key configured. Set ZEDCLAW_LLM_API_KEY environment variable.",
                else => "Error: Failed to contact LLM API.",
            };
            sendResponse(msg, source, chat_id);
            return;
        };
        defer g_allocator.free(response_json);

        // Record for rate limiting
        ratelimit.recordRequest();

        // Parse response
        var parsed = json_util.parseResponse(g_allocator, response_json, g_cfg) catch |err| {
            log.err("Failed to parse response: {}", .{err});
            historyRollbackTo(history_turn_start, "parse failed");
            sendResponse("Error: failed to parse LLM response", source, chat_id);
            return;
        };
        defer parsed.deinit();

        if (parsed.tool_name.len > 0) {
            // Tool call
            log.info("Tool call: {s} (round {})", .{ parsed.tool_name, rounds + 1 });

            // Get the tool input as a raw JSON string for history
            var input_json_buf: [config.MAX_MESSAGE_LEN]u8 = undefined;
            const input_json = if (parsed.tool_input) |ti| blk: {
                var stream = std.io.fixedBufferStream(&input_json_buf);
                std.json.stringify(ti, .{}, stream.writer()) catch {};
                break :blk input_json_buf[0..stream.pos];
            } else "{}";

            historyAdd("assistant", input_json, true, false, parsed.tool_id, parsed.tool_name);

            var result_buf: [config.TOOL_RESULT_BUF_SIZE]u8 = undefined;

            // Check if it's a user-defined tool
            if (tools_user.find(parsed.tool_name)) |user_tool| {
                _ = std.fmt.bufPrint(&result_buf,
                    "Execute this action now: {s}", .{user_tool.getAction()}) catch {};
                log.info("User tool '{s}' action: {s}", .{ parsed.tool_name, user_tool.getAction() });
            } else if (is_cron_trigger and std.mem.eql(u8, parsed.tool_name, "cron_set")) {
                _ = std.fmt.bufPrint(&result_buf,
                    "Error: cron_set is not allowed during scheduled task execution. " ++
                    "Execute the scheduled action now instead.", .{}) catch {};
                log.warn("Blocked cron_set during cron-triggered turn", .{});
            } else {
                const tool_input = parsed.tool_input orelse std.json.Value{ .object = std.json.ObjectMap.init(g_allocator) };
                const ok = tools_mod.execute(g_allocator, parsed.tool_name, tool_input, &result_buf);

                // Keep persona state synced
                if (ok and std.mem.eql(u8, parsed.tool_name, "set_persona")) {
                    if (parsed.tool_input) |ti| {
                        if (switch (ti) {
                            .object => |o| o.get("persona"),
                            else => null,
                        }) |pv| {
                            if (switch (pv) {
                                .string => |s| s,
                                else => null,
                            }) |pstr| {
                                if (tools_persona.Persona.fromString(pstr)) |p| {
                                    tools_persona.setPersona(p);
                                }
                            }
                        }
                    }
                } else if (ok and std.mem.eql(u8, parsed.tool_name, "reset_persona")) {
                    tools_persona.setPersona(.neutral);
                }
            }

            const result_str = std.mem.sliceTo(&result_buf, 0);
            log.info("Tool result: {s}", .{result_str[0..@min(result_str.len, 200)]});

            historyAdd("user", result_str, false, true, parsed.tool_id, null);
            // Continue loop
        } else {
            // Text response - done
            const reply = if (parsed.text.len > 0)
                parsed.text
            else
                "(No response)";

            historyAdd("assistant", reply, false, false, null, null);
            sendResponse(reply, source, chat_id);
            done = true;
        }
    }

    if (!done) {
        log.warn("Max tool rounds ({}) reached", .{config.MAX_TOOL_ROUNDS});
        historyAdd("assistant", "(Reached max tool iterations)", false, false, null, null);
        sendResponse("(Reached max tool iterations)", source, chat_id);
        return;
    }

    if (is_non_command) {
        copyTrunc(&g_last_non_command_text, text);
        g_last_non_command_response_ms = std.time.milliTimestamp();
    }
}

/// Main agent loop: dequeues messages and processes them.
pub fn run() void {
    log.info("Agent loop started", .{});

    while (true) {
        const msg = g_input_queue.pop();
        processMessage(msg.getText(), msg.source, msg.chat_id);
    }
}
