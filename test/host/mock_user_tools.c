/*
 * Mock user_tools for host tests
 */

#include "user_tools.h"
#include <string.h>
#include <stdio.h>

static user_tool_t s_mock_tools[MAX_DYNAMIC_TOOLS];
static int s_mock_count = 0;

void user_tools_init(void) {
    s_mock_count = 0;
}

bool user_tools_create(const char *name, const char *description, const char *action) {
    if (s_mock_count >= MAX_DYNAMIC_TOOLS) return false;
    strncpy(s_mock_tools[s_mock_count].name, name, TOOL_NAME_MAX_LEN - 1);
    strncpy(s_mock_tools[s_mock_count].description, description, TOOL_DESC_MAX_LEN - 1);
    strncpy(s_mock_tools[s_mock_count].action, action, CRON_MAX_ACTION_LEN - 1);
    s_mock_count++;
    return true;
}

bool user_tools_delete(const char *name) {
    (void)name;
    return false;
}

int user_tools_get_all(user_tool_t *tools, int max_count) {
    int count = s_mock_count < max_count ? s_mock_count : max_count;
    if (tools && count > 0) {
        memcpy(tools, s_mock_tools, (size_t)count * sizeof(user_tool_t));
    }
    return count;
}

const user_tool_t *user_tools_find(const char *name) {
    (void)name;
    return NULL;
}

int user_tools_count(void) {
    return s_mock_count;
}

void user_tools_list(char *buf, size_t buf_len) {
    if (buf && buf_len > 0) {
        snprintf(buf, buf_len, "Mock: %d user tools", s_mock_count);
    }
}
