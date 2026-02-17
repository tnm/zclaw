#ifndef TOOLS_H
#define TOOLS_H

#include "cJSON.h"
#include <stdbool.h>

// Tool execution function signature
typedef bool (*tool_execute_fn)(const cJSON *input, char *result, size_t result_len);

// Tool definition
typedef struct tool_def {
    const char *name;
    const char *description;
    const char *input_schema_json;
    tool_execute_fn execute;
} tool_def_t;

// Initialize the tool registry
void tools_init(void);

// Get all registered tools
const tool_def_t *tools_get_all(int *count);

// Find and execute a tool by name
// Returns true if tool was found and executed
bool tools_execute(const char *name, const cJSON *input, char *result, size_t result_len);

#endif // TOOLS_H
