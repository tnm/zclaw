//! Tool registry and dispatcher.
//! Replaces tools.c. No GPIO or I2C tools (ESP32-specific hardware removed).

const std = @import("std");
const config = @import("config.zig");
const tools_memory = @import("tools_memory.zig");
const tools_cron = @import("tools_cron.zig");
const tools_persona = @import("tools_persona.zig");
const tools_system = @import("tools_system.zig");
const tools_user = @import("tools_user.zig");

const log = std.log.scoped(.tools);

pub const ToolHandler = *const fn (
    allocator: std.mem.Allocator,
    input: std.json.Value,
    result: []u8,
) bool;

pub const ToolDef = struct {
    name: []const u8,
    description: []const u8,
    input_schema_json: []const u8,
    execute: ToolHandler,
};

// -----------------------------------------------------------------------------
// Built-in Tool Registry
// Note: GPIO (gpio_write, gpio_read, gpio_read_all, delay) and i2c_scan removed.
// These were ESP32-specific hardware tools with no desktop equivalent.
// -----------------------------------------------------------------------------
const TOOLS = [_]ToolDef{
    // Memory
    .{
        .name = "memory_set",
        .description = "Store a value in persistent memory. Key must start with u_.",
        .input_schema_json =
        \\{"type":"object","properties":{"key":{"type":"string","description":"User key (must start with u_)"},"value":{"type":"string","description":"Value to store"}},"required":["key","value"]}
        ,
        .execute = tools_memory.memorySetHandler,
    },
    .{
        .name = "memory_get",
        .description = "Retrieve a value from persistent memory. Key must start with u_.",
        .input_schema_json =
        \\{"type":"object","properties":{"key":{"type":"string","description":"User key (must start with u_)"}},"required":["key"]}
        ,
        .execute = tools_memory.memoryGetHandler,
    },
    .{
        .name = "memory_list",
        .description = "List all user memory keys (u_*).",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_memory.memoryListHandler,
    },
    .{
        .name = "memory_delete",
        .description = "Delete a key from persistent memory. Key must start with u_.",
        .input_schema_json =
        \\{"type":"object","properties":{"key":{"type":"string","description":"User key to delete (must start with u_)"}},"required":["key"]}
        ,
        .execute = tools_memory.memoryDeleteHandler,
    },
    // Persona
    .{
        .name = "set_persona",
        .description = "Set assistant tone persona. Call only when user explicitly asks to change persona/tone settings. Affects wording only.",
        .input_schema_json =
        \\{"type":"object","properties":{"persona":{"type":"string","enum":["neutral","friendly","technical","witty"],"description":"Persona name"}},"required":["persona"]}
        ,
        .execute = tools_persona.setPersonaHandler,
    },
    .{
        .name = "get_persona",
        .description = "Get current assistant tone persona.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_persona.getPersonaHandler,
    },
    .{
        .name = "reset_persona",
        .description = "Reset assistant tone persona back to neutral.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_persona.resetPersonaHandler,
    },
    // Cron/Scheduler
    .{
        .name = "cron_set",
        .description = "Create a scheduled task. Type 'periodic' runs every N minutes. Type 'daily' runs at a specific local time. Type 'once' runs one time after N minutes.",
        .input_schema_json =
        \\{"type":"object","properties":{"type":{"type":"string","enum":["periodic","daily","once"]},"interval_minutes":{"type":"integer","description":"For periodic: minutes between runs"},"delay_minutes":{"type":"integer","description":"For once: minutes from now"},"hour":{"type":"integer","description":"For daily: hour 0-23"},"minute":{"type":"integer","description":"For daily: minute 0-59"},"action":{"type":"string","description":"What to do when triggered"}},"required":["type","action"]}
        ,
        .execute = tools_cron.cronSetHandler,
    },
    .{
        .name = "cron_list",
        .description = "List all scheduled tasks.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_cron.cronListHandler,
    },
    .{
        .name = "cron_delete",
        .description = "Delete a scheduled task by ID.",
        .input_schema_json =
        \\{"type":"object","properties":{"id":{"type":"integer","description":"Schedule ID to delete"}},"required":["id"]}
        ,
        .execute = tools_cron.cronDeleteHandler,
    },
    .{
        .name = "get_time",
        .description = "Get current date and time.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_cron.getTimeHandler,
    },
    .{
        .name = "set_timezone",
        .description = "Set timezone label used by get_time. Accepts UTC, America/Los_Angeles, America/New_York, etc.",
        .input_schema_json =
        \\{"type":"object","properties":{"timezone":{"type":"string","description":"Timezone name"}},"required":["timezone"]}
        ,
        .execute = tools_cron.setTimezoneHandler,
    },
    .{
        .name = "get_timezone",
        .description = "Get current timezone setting.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_cron.getTimezoneHandler,
    },
    // System
    .{
        .name = "get_version",
        .description = "Get current zedclaw version.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_system.getVersionHandler,
    },
    .{
        .name = "get_health",
        .description = "Get agent health status: rate limits, time, version.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_system.getHealthHandler,
    },
    // User Tool Management
    .{
        .name = "create_tool",
        .description = "Create a custom tool with a name, description, and action.",
        .input_schema_json =
        \\{"type":"object","properties":{"name":{"type":"string","description":"Tool name (alphanumeric, no spaces)"},"description":{"type":"string","description":"Short description"},"action":{"type":"string","description":"What to do when tool is called"}},"required":["name","description","action"]}
        ,
        .execute = tools_system.createToolHandler,
    },
    .{
        .name = "list_user_tools",
        .description = "List all user-created custom tools.",
        .input_schema_json =
        \\{"type":"object","properties":{}}
        ,
        .execute = tools_system.listUserToolsHandler,
    },
    .{
        .name = "delete_user_tool",
        .description = "Delete a user-created custom tool by name.",
        .input_schema_json =
        \\{"type":"object","properties":{"name":{"type":"string","description":"Tool name to delete"}},"required":["name"]}
        ,
        .execute = tools_system.deleteUserToolHandler,
    },
};

pub fn init(allocator: std.mem.Allocator) void {
    tools_user.init(allocator);
    log.info("Registered {} built-in tools, {} user tools", .{
        TOOLS.len,
        tools_user.count(),
    });
}

pub fn getAll() []const ToolDef {
    return &TOOLS;
}

pub fn execute(
    allocator: std.mem.Allocator,
    name: []const u8,
    input: std.json.Value,
    result: []u8,
) bool {
    for (&TOOLS) |*tool| {
        if (std.mem.eql(u8, tool.name, name)) {
            log.info("Exec: {s}", .{name});
            return tool.execute(allocator, input, result);
        }
    }

    _ = std.fmt.bufPrint(result, "Unknown tool: {s}", .{name}) catch {};
    return false;
}
