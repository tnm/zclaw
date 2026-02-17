#include "tools.h"
#include "tools_handlers.h"
#include "user_tools.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tools";

// -----------------------------------------------------------------------------
// Tool Registry
// -----------------------------------------------------------------------------

static const tool_def_t s_tools[] = {
    // GPIO
    {
        .name = "gpio_write",
        .description = "Set a GPIO pin HIGH or LOW. Controls LEDs, relays, outputs.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"Pin 2-10\"},\"state\":{\"type\":\"integer\",\"description\":\"0=LOW, 1=HIGH\"}},\"required\":[\"pin\",\"state\"]}",
        .execute = tools_gpio_write_handler
    },
    {
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"Pin 2-10\"}},\"required\":[\"pin\"]}",
        .execute = tools_gpio_read_handler
    },
    {
        .name = "delay",
        .description = "Wait for specified milliseconds (max 60000). Use between GPIO operations.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"milliseconds\":{\"type\":\"integer\",\"description\":\"Time to wait in ms (max 60000)\"}},\"required\":[\"milliseconds\"]}",
        .execute = tools_delay_handler
    },
    // Memory
    {
        .name = "memory_set",
        .description = "Store a value in persistent user memory. Key must start with u_.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"User key (max 15 chars, must start with u_)\"},\"value\":{\"type\":\"string\",\"description\":\"Value to store\"}},\"required\":[\"key\",\"value\"]}",
        .execute = tools_memory_set_handler
    },
    {
        .name = "memory_get",
        .description = "Retrieve a value from persistent user memory. Key must start with u_.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"User key to retrieve (must start with u_)\"}},\"required\":[\"key\"]}",
        .execute = tools_memory_get_handler
    },
    {
        .name = "memory_list",
        .description = "List all user memory keys (u_*).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tools_memory_list_handler
    },
    {
        .name = "memory_delete",
        .description = "Delete a key from persistent user memory. Key must start with u_.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"User key to delete (must start with u_)\"}},\"required\":[\"key\"]}",
        .execute = tools_memory_delete_handler
    },
    // Cron/Scheduler
    {
        .name = "cron_set",
        .description = "Create a scheduled task. Type 'periodic' runs every N minutes. Type 'daily' runs at a specific time.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"periodic\",\"daily\"]},\"interval_minutes\":{\"type\":\"integer\",\"description\":\"For periodic: minutes between runs\"},\"hour\":{\"type\":\"integer\",\"description\":\"For daily: hour 0-23\"},\"minute\":{\"type\":\"integer\",\"description\":\"For daily: minute 0-59\"},\"action\":{\"type\":\"string\",\"description\":\"What to do when triggered\"}},\"required\":[\"type\",\"action\"]}",
        .execute = tools_cron_set_handler
    },
    {
        .name = "cron_list",
        .description = "List all scheduled tasks.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tools_cron_list_handler
    },
    {
        .name = "cron_delete",
        .description = "Delete a scheduled task by ID.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Schedule ID to delete\"}},\"required\":[\"id\"]}",
        .execute = tools_cron_delete_handler
    },
    {
        .name = "get_time",
        .description = "Get current date and time.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tools_get_time_handler
    },
    // OTA
    {
        .name = "get_version",
        .description = "Get current firmware version.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tools_get_version_handler
    },
    {
        .name = "check_update",
        .description = "Check if firmware update is available at URL (manifest support not implemented yet).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to firmware binary\"}},\"required\":[\"url\"]}",
        .execute = tools_check_update_handler
    },
    {
        .name = "install_update",
        .description = "Download and install firmware update. Device will reboot on success.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to firmware binary\"}},\"required\":[\"url\"]}",
        .execute = tools_install_update_handler
    },
    // Health
    {
        .name = "get_health",
        .description = "Get device health status: heap memory, rate limits, time sync, version.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tools_get_health_handler
    },
    // User Tool Management
    {
        .name = "create_tool",
        .description = "Create a custom tool. Provide a short name (no spaces), brief description, and the action to perform when called.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Tool name (alphanumeric, no spaces)\"},\"description\":{\"type\":\"string\",\"description\":\"Short description for tool list\"},\"action\":{\"type\":\"string\",\"description\":\"What to do when tool is called\"}},\"required\":[\"name\",\"description\",\"action\"]}",
        .execute = tools_create_tool_handler
    },
    {
        .name = "list_user_tools",
        .description = "List all user-created custom tools.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tools_list_user_tools_handler
    },
    {
        .name = "delete_user_tool",
        .description = "Delete a user-created custom tool by name.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Tool name to delete\"}},\"required\":[\"name\"]}",
        .execute = tools_delete_user_tool_handler
    },
};

static const int s_tool_count = sizeof(s_tools) / sizeof(s_tools[0]);

void tools_init(void)
{
    // Initialize user-defined tools from NVS
    user_tools_init();

    ESP_LOGI(TAG, "Registered %d built-in tools, %d user tools",
             s_tool_count, user_tools_count());
    for (int i = 0; i < s_tool_count; i++) {
        ESP_LOGI(TAG, "  %s", s_tools[i].name);
    }
}

const tool_def_t *tools_get_all(int *count)
{
    *count = s_tool_count;
    return s_tools;
}

bool tools_execute(const char *name, const cJSON *input, char *result, size_t result_len)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Exec: %s", name);
            return s_tools[i].execute(input, result, result_len);
        }
    }
    snprintf(result, result_len, "Unknown tool: %s", name);
    return false;
}
