//! Request rate limiting.
//! Sliding window counters per hour and per day.
//! Replaces ratelimit.c (ESP-IDF NVS calls replaced with file-based memory).

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");

const log = std.log.scoped(.ratelimit);

var g_mutex: std.Thread.Mutex = .{};
var g_requests_this_hour: u32 = 0;
var g_requests_today: u32 = 0;
var g_last_hour: i32 = -1;
var g_last_day: i32 = -1;
var g_last_year: i32 = -1;
var g_allocator: std.mem.Allocator = undefined;

pub fn init(allocator: std.mem.Allocator) void {
    g_allocator = allocator;

    // Load persisted daily count
    if (memory.memGet(allocator, config.MEMORY_NAMESPACE, config.KEY_RL_DAILY) catch null) |v| {
        defer allocator.free(v);
        g_requests_today = std.fmt.parseInt(u32, v, 10) catch 0;
    }
    if (memory.memGet(allocator, config.MEMORY_NAMESPACE, config.KEY_RL_DAY) catch null) |v| {
        defer allocator.free(v);
        g_last_day = std.fmt.parseInt(i32, v, 10) catch -1;
    }
    if (memory.memGet(allocator, config.MEMORY_NAMESPACE, config.KEY_RL_YEAR) catch null) |v| {
        defer allocator.free(v);
        g_last_year = std.fmt.parseInt(i32, v, 10) catch -1;
    }

    log.info("Rate limiter initialized: {} requests today", .{g_requests_today});
}

fn updateTimeWindow() void {
    const ts = std.time.timestamp();
    const epoch_seconds = std.time.epoch.EpochSeconds{ .secs = @intCast(ts) };
    const day_seconds = epoch_seconds.getDaySeconds();
    const epoch_day = epoch_seconds.getEpochDay();
    const year_day = epoch_day.calculateYearDay();

    const current_hour: i32 = @intCast(day_seconds.getHoursIntoDay());
    const current_day: i32 = @intCast(year_day.day);
    const current_year: i32 = @intCast(year_day.year);

    if (current_hour != g_last_hour) {
        g_requests_this_hour = 0;
        g_last_hour = current_hour;
    }

    if (current_day != g_last_day or current_year != g_last_year) {
        g_requests_today = 0;
        g_last_day = current_day;
        g_last_year = current_year;

        // Persist
        var buf: [32]u8 = undefined;
        const day_str = std.fmt.bufPrint(&buf, "{}", .{current_day}) catch "0";
        memory.memSet(config.MEMORY_NAMESPACE, config.KEY_RL_DAY, day_str) catch {};
        const year_str = std.fmt.bufPrint(&buf, "{}", .{current_year}) catch "0";
        memory.memSet(config.MEMORY_NAMESPACE, config.KEY_RL_YEAR, year_str) catch {};
        memory.memSet(config.MEMORY_NAMESPACE, config.KEY_RL_DAILY, "0") catch {};

        log.info("Daily rate limit reset", .{});
    }
}

/// Check if a request is allowed. Returns null if allowed, error message if not.
pub fn check(buf: []u8) ?[]const u8 {
    if (!config.RATELIMIT_ENABLED) return null;

    g_mutex.lock();
    defer g_mutex.unlock();

    updateTimeWindow();

    if (g_requests_this_hour >= config.RATELIMIT_MAX_PER_HOUR) {
        return std.fmt.bufPrint(buf,
            "Rate limited: {}/{} requests this hour. Try again later.",
            .{ g_requests_this_hour, config.RATELIMIT_MAX_PER_HOUR }) catch "Rate limited: hourly limit reached.";
    }

    if (g_requests_today >= config.RATELIMIT_MAX_PER_DAY) {
        return std.fmt.bufPrint(buf,
            "Daily limit reached: {}/{} requests today. Resets at midnight.",
            .{ g_requests_today, config.RATELIMIT_MAX_PER_DAY }) catch "Rate limited: daily limit reached.";
    }

    return null;
}

pub fn recordRequest() void {
    g_mutex.lock();
    defer g_mutex.unlock();

    updateTimeWindow();

    g_requests_this_hour += 1;
    g_requests_today += 1;

    // Persist daily count
    var buf: [32]u8 = undefined;
    const daily_str = std.fmt.bufPrint(&buf, "{}", .{g_requests_today}) catch "0";
    memory.memSet(config.MEMORY_NAMESPACE, config.KEY_RL_DAILY, daily_str) catch {};
}

pub fn getRequestsToday() u32 {
    g_mutex.lock();
    defer g_mutex.unlock();
    return g_requests_today;
}

pub fn getRequestsThisHour() u32 {
    g_mutex.lock();
    defer g_mutex.unlock();
    return g_requests_this_hour;
}

pub fn resetDaily() void {
    g_mutex.lock();
    defer g_mutex.unlock();
    g_requests_today = 0;
    g_requests_this_hour = 0;
    memory.memSet(config.MEMORY_NAMESPACE, config.KEY_RL_DAILY, "0") catch {};
    log.info("Rate limits manually reset", .{});
}
