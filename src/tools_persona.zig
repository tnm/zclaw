//! Persona tool handlers (set_persona, get_persona, reset_persona).

const std = @import("std");
const config = @import("config.zig");
const memory = @import("memory.zig");

const log = std.log.scoped(.tools_persona);

pub const Persona = enum {
    neutral,
    friendly,
    technical,
    witty,

    pub fn fromString(s: []const u8) ?Persona {
        if (std.mem.eql(u8, s, "neutral")) return .neutral;
        if (std.mem.eql(u8, s, "friendly")) return .friendly;
        if (std.mem.eql(u8, s, "technical")) return .technical;
        if (std.mem.eql(u8, s, "witty")) return .witty;
        return null;
    }

    pub fn name(self: Persona) []const u8 {
        return @tagName(self);
    }

    pub fn instruction(self: Persona) []const u8 {
        return switch (self) {
            .neutral => "Use direct, plain wording.",
            .friendly => "Use warm, approachable wording while staying concise.",
            .technical => "Use precise technical language and concrete terminology.",
            .witty => "Use a lightly witty tone; at most one brief witty flourish per reply.",
        };
    }
};

var g_persona: Persona = .neutral;
var g_allocator: std.mem.Allocator = undefined;

pub fn init(allocator: std.mem.Allocator) void {
    g_allocator = allocator;
    loadFromMemory();
}

fn loadFromMemory() void {
    const stored = memory.memGet(g_allocator, config.MEMORY_NAMESPACE, config.KEY_PERSONA) catch return orelse return;
    defer g_allocator.free(stored);

    // Lowercase
    var lower_buf: [32]u8 = undefined;
    const n = @min(stored.len, lower_buf.len);
    for (stored[0..n], 0..) |c, i| {
        lower_buf[i] = std.ascii.toLower(c);
    }

    if (Persona.fromString(lower_buf[0..n])) |p| {
        g_persona = p;
        log.info("Loaded persona: {s}", .{p.name()});
    }
}

pub fn getPersona() Persona {
    return g_persona;
}

pub fn setPersona(p: Persona) void {
    g_persona = p;
}

pub fn setPersonaHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;

    const obj = switch (input) {
        .object => |o| o,
        else => {
            _ = std.fmt.bufPrint(result, "Error: invalid input", .{}) catch {};
            return false;
        },
    };

    const persona_val = obj.get("persona") orelse {
        _ = std.fmt.bufPrint(result, "Error: 'persona' required", .{}) catch {};
        return false;
    };

    const persona_str = switch (persona_val) {
        .string => |s| s,
        else => {
            _ = std.fmt.bufPrint(result, "Error: 'persona' must be a string", .{}) catch {};
            return false;
        },
    };

    const p = Persona.fromString(persona_str) orelse {
        _ = std.fmt.bufPrint(result, "Error: unknown persona '{s}'. Valid: neutral, friendly, technical, witty", .{persona_str}) catch {};
        return false;
    };

    g_persona = p;
    memory.memSet(config.MEMORY_NAMESPACE, config.KEY_PERSONA, p.name()) catch {};

    _ = std.fmt.bufPrint(result, "Persona set to: {s}", .{p.name()}) catch {};
    return true;
}

pub fn getPersonaHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    _ = std.fmt.bufPrint(result, "Current persona: {s}", .{g_persona.name()}) catch {};
    return true;
}

pub fn resetPersonaHandler(
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool {
    _ = allocator;
    _ = input;

    g_persona = .neutral;
    memory.memSet(config.MEMORY_NAMESPACE, config.KEY_PERSONA, "neutral") catch {};

    _ = std.fmt.bufPrint(result, "Persona reset to: neutral", .{}) catch {};
    return true;
}
