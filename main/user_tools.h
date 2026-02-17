#ifndef USER_TOOLS_H
#define USER_TOOLS_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>

// User-defined tool (stored in NVS)
typedef struct {
    char name[TOOL_NAME_MAX_LEN];
    char description[TOOL_DESC_MAX_LEN];
    char action[CRON_MAX_ACTION_LEN];  // Natural language action to execute
} user_tool_t;

// Initialize user tools (load from NVS)
void user_tools_init(void);

// Create a new user tool (persists to NVS)
// Returns true on success
bool user_tools_create(const char *name, const char *description, const char *action);

// Delete a user tool by name
bool user_tools_delete(const char *name);

// Get all user tools
// Returns count, fills array up to max_count
int user_tools_get_all(user_tool_t *tools, int max_count);

// Find a user tool by name
// Returns NULL if not found
const user_tool_t *user_tools_find(const char *name);

// Get count of user tools
int user_tools_count(void);

// List user tools into buffer (for display)
void user_tools_list(char *buf, size_t buf_len);

#endif // USER_TOOLS_H
