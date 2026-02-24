//! LLM HTTP client for desktop.
//! Replaces llm.c (esp_http_client replaced with std.http.Client).
//! Supports Anthropic, OpenAI, OpenRouter, Ollama backends.

const std = @import("std");
const config = @import("config.zig");

const log = std.log.scoped(.llm);

var g_cfg: *const config.RuntimeConfig = undefined;
var g_allocator: std.mem.Allocator = undefined;

pub fn init(allocator: std.mem.Allocator, cfg: *const config.RuntimeConfig) void {
    g_allocator = allocator;
    g_cfg = cfg;
    log.info("LLM backend: {s}, model: {s}", .{ cfg.llm_backend.name(), cfg.model() });
}

/// Send a JSON request to the LLM API and return the response body.
/// Caller owns the returned slice.
pub fn request(allocator: std.mem.Allocator, request_json: []const u8) ![]u8 {
    const url = g_cfg.apiUrl();

    log.info("Sending request to {s} ({} bytes)...", .{ g_cfg.llm_backend.name(), request_json.len });

    var client = std.http.Client{ .allocator = allocator };
    defer client.deinit();

    var response_body = std.ArrayList(u8).init(allocator);
    errdefer response_body.deinit();

    // Build headers
    var headers_buf = std.ArrayList(std.http.Header).init(allocator);
    defer headers_buf.deinit();

    try headers_buf.append(.{ .name = "content-type", .value = "application/json" });

    // Backend-specific auth headers
    switch (g_cfg.llm_backend) {
        .anthropic => {
            try headers_buf.append(.{ .name = "x-api-key", .value = g_cfg.llm_api_key });
            try headers_buf.append(.{ .name = "anthropic-version", .value = "2023-06-01" });
        },
        .openai, .openrouter, .ollama => {
            if (g_cfg.llm_api_key.len > 0) {
                const bearer = try std.fmt.allocPrint(allocator, "Bearer {s}", .{g_cfg.llm_api_key});
                defer allocator.free(bearer);
                try headers_buf.append(.{ .name = "authorization", .value = bearer });
            }
            if (g_cfg.llm_backend == .openrouter) {
                try headers_buf.append(.{ .name = "http-referer", .value = "https://github.com/tnm/zedclaw" });
                try headers_buf.append(.{ .name = "x-title", .value = "zedclaw" });
            }
        },
    }

    const result = try client.fetch(.{
        .location = .{ .url = url },
        .method = .POST,
        .payload = request_json,
        .extra_headers = headers_buf.items,
        .response_storage = .{ .dynamic = &response_body },
        .max_append_size = config.LLM_RESPONSE_BUF_SIZE,
    });

    if (result.status != .ok) {
        log.err("LLM API error: HTTP {}", .{@intFromEnum(result.status)});
        if (response_body.items.len > 0) {
            log.err("Response body: {s}", .{response_body.items[0..@min(response_body.items.len, 512)]});
        }
        return error.LlmApiError;
    }

    log.info("Response: {} bytes", .{response_body.items.len});
    return response_body.toOwnedSlice();
}

/// Make a request with retry logic and exponential backoff.
pub fn requestWithRetry(allocator: std.mem.Allocator, request_json: []const u8) ![]u8 {
    if (g_cfg.llm_backend.requiresApiKey() and g_cfg.llm_api_key.len == 0) {
        log.err("No API key configured for {s}", .{g_cfg.llm_backend.name()});
        return error.NoApiKey;
    }

    var retry_delay_ms: u64 = config.LLM_RETRY_BASE_MS;
    const budget_start = std.time.milliTimestamp();

    var attempt: usize = 0;
    while (attempt < config.LLM_MAX_RETRIES) : (attempt += 1) {
        const elapsed = std.time.milliTimestamp() - budget_start;
        if (attempt > 0 and elapsed >= config.LLM_RETRY_BUDGET_MS) {
            log.warn("LLM retry budget exhausted ({} ms)", .{elapsed});
            break;
        }

        const result = request(allocator, request_json) catch |err| {
            if (attempt >= config.LLM_MAX_RETRIES - 1) {
                log.err("LLM request failed after {} attempts: {}", .{ attempt + 1, err });
                return err;
            }

            const remaining_budget: i64 = @intCast(config.LLM_RETRY_BUDGET_MS);
            const elapsed_now = std.time.milliTimestamp() - budget_start;
            if (elapsed_now >= remaining_budget) break;

            const delay = @min(retry_delay_ms, @as(u64, @intCast(remaining_budget - elapsed_now)));
            log.warn("LLM attempt {}/{} failed: {}. Retrying in {} ms...", .{
                attempt + 1, config.LLM_MAX_RETRIES, err, delay,
            });

            std.time.sleep(delay * std.time.ns_per_ms);
            retry_delay_ms = @min(retry_delay_ms * 2, config.LLM_RETRY_MAX_MS);
            continue;
        };

        return result;
    }

    return error.LlmRetryExhausted;
}
