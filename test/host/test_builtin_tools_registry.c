/*
 * Host tests for built-in tool registry definitions.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct builtin_tool_entry {
    const char *name;
    const char *description;
    const char *schema;
} builtin_tool_entry_t;

#define TOOL_ENTRY(tool_name, tool_description, tool_schema, tool_execute) \
    {                                                                       \
        .name = tool_name,                                                  \
        .description = tool_description,                                    \
        .schema = tool_schema,                                              \
    },

static const builtin_tool_entry_t s_builtin_tools[] = {
#include "builtin_tools.def"
};

#undef TOOL_ENTRY

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

static bool has_tool(const char *name)
{
    size_t i;
    for (i = 0; i < (sizeof(s_builtin_tools) / sizeof(s_builtin_tools[0])); i++) {
        if (strcmp(s_builtin_tools[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool schema_has_object_properties(const char *schema)
{
    if (!schema || strstr(schema, "\"type\":\"object\"") == NULL) {
        return true;
    }
    return strstr(schema, "\"properties\":") != NULL;
}

TEST(registry_not_empty)
{
    ASSERT((sizeof(s_builtin_tools) / sizeof(s_builtin_tools[0])) > 0);
    return 0;
}

TEST(entries_have_valid_fields_and_unique_names)
{
    size_t i;
    size_t j;
    size_t count = sizeof(s_builtin_tools) / sizeof(s_builtin_tools[0]);

    for (i = 0; i < count; i++) {
        ASSERT(s_builtin_tools[i].name != NULL);
        ASSERT(s_builtin_tools[i].name[0] != '\0');
        ASSERT(s_builtin_tools[i].description != NULL);
        ASSERT(s_builtin_tools[i].description[0] != '\0');
        ASSERT(s_builtin_tools[i].schema != NULL);
        ASSERT(s_builtin_tools[i].schema[0] == '{');
        ASSERT(schema_has_object_properties(s_builtin_tools[i].schema));

        for (j = i + 1; j < count; j++) {
            ASSERT(strcmp(s_builtin_tools[i].name, s_builtin_tools[j].name) != 0);
        }
    }

    return 0;
}

TEST(required_core_tools_exist)
{
    static const char *required[] = {
        "gpio_write",
        "i2c_write",
        "i2c_read",
        "i2c_write_read",
        "dht_read",
        "memory_set",
        "cron_set",
        "get_diagnostics",
        "create_tool",
        "list_user_tools",
        "delete_user_tool",
        NULL
    };
    int i;

    for (i = 0; required[i] != NULL; i++) {
        ASSERT(has_tool(required[i]));
    }

    return 0;
}

int test_builtin_tools_registry_all(void)
{
    int failures = 0;

    printf("\nBuilt-in Tool Registry Tests:\n");

    printf("  registry_not_empty... ");
    if (test_registry_not_empty() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  entries_have_valid_fields_and_unique_names... ");
    if (test_entries_have_valid_fields_and_unique_names() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  required_core_tools_exist... ");
    if (test_required_core_tools_exist() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
