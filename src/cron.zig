//! Cron/scheduler subsystem for desktop.
//! Replaces cron.c. No NTP needed (desktop has system time).
//! Uses std.Thread for background checking.

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");

const log = std.log.scoped(.cron);

pub const CronType = enum(u8) {
    periodic = 0,
    daily = 1,
    once = 2,
};

pub const CronEntry = struct {
    id: u8 = 0,
    type: CronType = .periodic,
    enabled: bool = false,
    last_run: i64 = 0, // Unix timestamp
    interval_minutes: u16 = 0,
    hour: u8 = 0,
    minute: u8 = 0,
    action: [config.CRON_MAX_ACTION_LEN]u8 = std.mem.zeroes([config.CRON_MAX_ACTION_LEN]u8),

    pub fn getAction(self: *const CronEntry) []const u8 {
        return std.mem.sliceTo(&self.action, 0);
    }
};

var g_entries: [config.CRON_MAX_ENTRIES]CronEntry = undefined;
var g_entries_mutex: std.Thread.Mutex = .{};
var g_timezone: [config.TIMEZONE_MAX_LEN]u8 = std.mem.zeroes([config.TIMEZONE_MAX_LEN]u8);
var g_allocator: std.mem.Allocator = undefined;
// Callback for firing events: fn(action: []const u8, id: u8) void
var g_fire_callback: ?*const fn (action: []const u8, id: u8) void = null;

fn copyTrunc(dest: []u8, src: []const u8) void {
    const n = @min(src.len, dest.len - 1);
    @memcpy(dest[0..n], src[0..n]);
    dest[n] = 0;
}

pub fn init(allocator: std.mem.Allocator) void {
    g_allocator = allocator;
    g_entries = std.mem.zeroes([config.CRON_MAX_ENTRIES]CronEntry);
    copyTrunc(&g_timezone, config.DEFAULT_TIMEZONE);
    loadEntries();
    loadTimezoneFromMemory();
}

pub fn setFireCallback(cb: *const fn (action: []const u8, id: u8) void) void {
    g_fire_callback = cb;
}

fn saveEntries() !void {
    var buf = std.ArrayList(u8).init(g_allocator);
    defer buf.deinit();

    try buf.appendSlice("[");
    var first = true;
    for (g_entries) |*entry| {
        if (entry.id == 0) continue;
        if (!first) try buf.appendSlice(",");
        first = false;

        try buf.writer().print(
            "{{\"id\":{},\"type\":{},\"enabled\":{},\"last_run\":{},\"interval_minutes\":{},\"hour\":{},\"minute\":{},\"action\":\"",
            .{
                entry.id,
                @intFromEnum(entry.type),
                if (entry.enabled) @as(u8, 1) else @as(u8, 0),
                entry.last_run,
                entry.interval_minutes,
                entry.hour,
                entry.minute,
            },
        );

        // Escape action string manually
        for (entry.getAction()) |c| {
            switch (c) {
                '"' => try buf.appendSlice("\\\""),
                '\\' => try buf.appendSlice("\\\\"),
                '\n' => try buf.appendSlice("\\n"),
                else => try buf.append(c),
            }
        }
        try buf.appendSlice("\"}");
    }
    try buf.appendSlice("]");

    try memory.memSet(config.MEMORY_NAMESPACE_CRON, "entries", buf.items);
}

fn loadEntries() void {
    const json_str = memory.memGet(g_allocator, config.MEMORY_NAMESPACE_CRON, "entries") catch return orelse return;
    defer g_allocator.free(json_str);

    const parsed = std.json.parseFromSlice(std.json.Value, g_allocator, json_str, .{}) catch {
        log.warn("Failed to parse cron entries from storage", .{});
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

        const id_val = obj.get("id") orelse continue;
        const id: u8 = switch (id_val) {
            .integer => |n| @intCast(@min(n, 255)),
            else => continue,
        };
        if (id == 0) continue;

        // Find free slot
        var slot: ?usize = null;
        for (g_entries, 0..) |*e, i| {
            if (e.id == 0) {
                slot = i;
                break;
            }
        }
        const s = slot orelse continue;

        var entry = &g_entries[s];
        entry.id = id;

        if (obj.get("type")) |t| {
            entry.type = switch (t) {
                .integer => |n| switch (n) {
                    0 => .periodic,
                    1 => .daily,
                    2 => .once,
                    else => .periodic,
                },
                else => .periodic,
            };
        }

        if (obj.get("enabled")) |e| {
            entry.enabled = switch (e) {
                .integer => |n| n != 0,
                .bool => |b| b,
                else => true,
            };
        } else {
            entry.enabled = true;
        }

        if (obj.get("last_run")) |lr| {
            entry.last_run = switch (lr) {
                .integer => |n| n,
                else => 0,
            };
        }

        if (obj.get("interval_minutes")) |im| {
            entry.interval_minutes = switch (im) {
                .integer => |n| @intCast(@min(n, 65535)),
                else => 0,
            };
        }

        if (obj.get("hour")) |h| {
            entry.hour = switch (h) {
                .integer => |n| @intCast(@min(n, 23)),
                else => 0,
            };
        }

        if (obj.get("minute")) |m| {
            entry.minute = switch (m) {
                .integer => |n| @intCast(@min(n, 59)),
                else => 0,
            };
        }

        if (obj.get("action")) |a| {
            switch (a) {
                .string => |s_str| copyTrunc(&entry.action, s_str),
                else => {},
            }
        }
    }
    log.info("Loaded cron entries", .{});
}

fn loadTimezoneFromMemory() void {
    const tz = memory.memGet(g_allocator, config.MEMORY_NAMESPACE, config.KEY_TIMEZONE) catch return orelse return;
    defer g_allocator.free(tz);
    copyTrunc(&g_timezone, tz);
    log.info("Loaded timezone: {s}", .{g_timezone[0..std.mem.indexOfScalar(u8, &g_timezone, 0) orelse g_timezone.len]});
}

fn nextEntryId() u8 {
    var used: [256]bool = std.mem.zeroes([256]bool);
    for (g_entries) |*e| {
        if (e.id != 0) used[e.id] = true;
    }
    var id: u8 = 1;
    while (id < 255 and used[id]) : (id += 1) {}
    return if (used[id]) 0 else id;
}

pub fn cronSet(cron_type: CronType, interval_or_hour: u16, minute: u8, action: []const u8) u8 {
    g_entries_mutex.lock();
    defer g_entries_mutex.unlock();

    if (action.len == 0) {
        log.err("Cannot create cron entry: empty action", .{});
        return 0;
    }

    // Find free slot
    var slot: ?usize = null;
    for (g_entries, 0..) |*e, i| {
        if (e.id == 0) {
            slot = i;
            break;
        }
    }
    const s = slot orelse {
        log.err("No free cron slots", .{});
        return 0;
    };

    const id = nextEntryId();
    if (id == 0) {
        log.err("No free cron IDs", .{});
        return 0;
    }

    var entry = &g_entries[s];
    entry.id = id;
    entry.type = cron_type;
    entry.enabled = true;
    entry.last_run = 0;

    switch (cron_type) {
        .periodic, .once => {
            entry.interval_minutes = interval_or_hour;
            entry.hour = 0;
            entry.minute = 0;
        },
        .daily => {
            entry.interval_minutes = 0;
            entry.hour = @intCast(@min(interval_or_hour, 23));
            entry.minute = minute;
        },
    }

    if (cron_type == .once) {
        entry.last_run = std.time.timestamp();
    }

    copyTrunc(&entry.action, action);

    saveEntries() catch |err| {
        log.err("Failed to save cron entry: {}", .{err});
        entry.id = 0;
        return 0;
    };

    log.info("Created cron entry {}: type={s} action={s}", .{ id, @tagName(cron_type), action });
    return id;
}

pub fn cronList(allocator: std.mem.Allocator) ![]u8 {
    g_entries_mutex.lock();
    defer g_entries_mutex.unlock();

    var buf = std.ArrayList(u8).init(allocator);

    try buf.appendSlice("[");
    var first = true;
    for (g_entries) |*entry| {
        if (entry.id == 0) continue;
        if (!first) try buf.appendSlice(",");
        first = false;

        try buf.writer().print("{{\"id\":{},\"type\":\"{s}\",", .{
            entry.id, @tagName(entry.type),
        });

        switch (entry.type) {
            .periodic => {
                try buf.writer().print("\"interval_minutes\":{},", .{entry.interval_minutes});
            },
            .once => {
                try buf.writer().print("\"delay_minutes\":{},", .{entry.interval_minutes});
            },
            .daily => {
                try buf.writer().print("\"time\":\"{:0>2}:{:0>2}\",", .{ entry.hour, entry.minute });
            },
        }

        try buf.appendSlice("\"action\":\"");
        for (entry.getAction()) |c| {
            switch (c) {
                '"' => try buf.appendSlice("\\\""),
                '\\' => try buf.appendSlice("\\\\"),
                '\n' => try buf.appendSlice("\\n"),
                else => try buf.append(c),
            }
        }
        try buf.appendSlice("\",\"enabled\":");
        try buf.appendSlice(if (entry.enabled) "true" else "false");
        try buf.appendSlice(",\"timezone\":\"");
        const tz_end = std.mem.indexOfScalar(u8, &g_timezone, 0) orelse g_timezone.len;
        try buf.appendSlice(g_timezone[0..tz_end]);
        try buf.appendSlice("\"}");
    }
    try buf.appendSlice("]");

    return buf.toOwnedSlice();
}

pub fn cronDelete(id: u8) bool {
    g_entries_mutex.lock();
    defer g_entries_mutex.unlock();

    for (g_entries) |*entry| {
        if (entry.id == id) {
            entry.id = 0;
            saveEntries() catch |err| {
                log.err("Failed to save after delete: {}", .{err});
                entry.id = id; // rollback
                return false;
            };
            log.info("Deleted cron entry {}", .{id});
            return true;
        }
    }
    return false;
}

pub fn setTimezone(tz: []const u8) !void {
    if (tz.len == 0 or tz.len >= config.TIMEZONE_MAX_LEN) return error.InvalidTimezone;
    copyTrunc(&g_timezone, tz);
    try memory.memSet(config.MEMORY_NAMESPACE, config.KEY_TIMEZONE, tz);
    log.info("Timezone set to: {s}", .{tz});
}

pub fn getTimezone(buf: []u8) usize {
    const end = std.mem.indexOfScalar(u8, &g_timezone, 0) orelse g_timezone.len;
    const n = @min(end, buf.len - 1);
    @memcpy(buf[0..n], g_timezone[0..n]);
    buf[n] = 0;
    return n;
}

pub fn getTimeStr(buf: []u8) usize {
    const ts = std.time.timestamp();
    const epoch = std.time.epoch.EpochSeconds{ .secs = @intCast(ts) };
    const day_s = epoch.getDaySeconds();
    const epoch_day = epoch.getEpochDay();
    const year_day = epoch_day.calculateYearDay();
    const month_day = year_day.calculateMonthDay();

    const written = std.fmt.bufPrint(buf,
        "{:0>4}-{:0>2}-{:0>2} {:0>2}:{:0>2}:{:0>2} {}",
        .{
            year_day.year,
            @intFromEnum(month_day.month),
            month_day.day_index + 1,
            day_s.getHoursIntoDay(),
            day_s.getMinutesIntoHour(),
            day_s.getSecondsIntoMinute(),
            "UTC",
        }) catch return 0;
    return written.len;
}

/// Background thread: checks entries every CRON_CHECK_INTERVAL_MS
pub fn runBackground() void {
    log.info("Cron background thread started", .{});

    while (true) {
        std.time.sleep(config.CRON_CHECK_INTERVAL_MS * std.time.ns_per_ms);
        checkAndFire();
    }
}

fn checkAndFire() void {
    const now = std.time.timestamp();
    const epoch = std.time.epoch.EpochSeconds{ .secs = @intCast(now) };
    const day_s = epoch.getDaySeconds();
    const current_hour = day_s.getHoursIntoDay();
    const current_minute = day_s.getMinutesIntoHour();

    g_entries_mutex.lock();

    var to_fire_buf: [config.CRON_MAX_ENTRIES]struct { id: u8, action: [config.CRON_MAX_ACTION_LEN]u8 } = undefined;
    var fire_count: usize = 0;

    for (&g_entries) |*entry| {
        if (entry.id == 0 or !entry.enabled) continue;

        var should_fire = false;

        switch (entry.type) {
            .periodic => {
                const interval_secs: i64 = @intCast(entry.interval_minutes * 60);
                if (now - entry.last_run >= interval_secs) {
                    should_fire = true;
                }
            },
            .once => {
                const delay_secs: i64 = @intCast(entry.interval_minutes * 60);
                if (entry.last_run > 0 and now >= entry.last_run and
                    now - entry.last_run >= delay_secs)
                {
                    should_fire = true;
                }
            },
            .daily => {
                if (current_hour == entry.hour and current_minute == entry.minute) {
                    const minute_start = now - @as(i64, @intCast(day_s.getSecondsIntoMinute()));
                    if (entry.last_run < minute_start) {
                        should_fire = true;
                    }
                }
            },
        }

        if (should_fire and fire_count < config.CRON_MAX_ENTRIES) {
            to_fire_buf[fire_count].id = entry.id;
            copyTrunc(&to_fire_buf[fire_count].action, entry.getAction());
            fire_count += 1;

            if (entry.type == .once) {
                entry.id = 0; // Remove one-shot entry
            } else {
                entry.last_run = now;
            }
        }
    }

    g_entries_mutex.unlock();

    if (fire_count > 0) {
        saveEntries() catch |err| {
            log.warn("Failed to persist cron state after firing: {}", .{err});
        };
    }

    // Fire events (outside lock)
    for (to_fire_buf[0..fire_count]) |*item| {
        const action = std.mem.sliceTo(&item.action, 0);
        log.info("Firing cron {}: {s}", .{ item.id, action });
        if (g_fire_callback) |cb| {
            cb(action, item.id);
        }
    }
}
