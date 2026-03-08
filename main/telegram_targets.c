#include "telegram_targets.h"

#include "config.h"
#include "telegram_chat_ids.h"

#include <string.h>

static int64_t s_primary_chat_id = 0;
static int64_t s_allowed_chat_ids[TELEGRAM_MAX_ALLOWED_CHAT_IDS] = {0};
static size_t s_allowed_chat_count = 0;

void telegram_targets_clear(void)
{
    memset(s_allowed_chat_ids, 0, sizeof(s_allowed_chat_ids));
    s_allowed_chat_count = 0;
    s_primary_chat_id = 0;
}

bool telegram_targets_set_from_string(const char *input)
{
    int64_t parsed_ids[TELEGRAM_MAX_ALLOWED_CHAT_IDS] = {0};
    size_t parsed_count = 0;

    if (!telegram_chat_ids_parse(input, parsed_ids, TELEGRAM_MAX_ALLOWED_CHAT_IDS, &parsed_count)) {
        return false;
    }

    telegram_targets_clear();
    for (size_t i = 0; i < parsed_count; i++) {
        s_allowed_chat_ids[i] = parsed_ids[i];
    }
    s_allowed_chat_count = parsed_count;
    s_primary_chat_id = s_allowed_chat_ids[0];
    return true;
}

bool telegram_targets_has_any(void)
{
    return s_allowed_chat_count > 0;
}

size_t telegram_targets_count(void)
{
    return s_allowed_chat_count;
}

int64_t telegram_targets_primary_chat_id(void)
{
    return s_primary_chat_id;
}

bool telegram_targets_is_authorized(int64_t incoming_chat_id)
{
    return telegram_chat_ids_contains(s_allowed_chat_ids, s_allowed_chat_count, incoming_chat_id);
}

int64_t telegram_targets_resolve_target_chat_id(int64_t requested_chat_id)
{
    return telegram_chat_ids_resolve_target(
        s_allowed_chat_ids, s_allowed_chat_count, s_primary_chat_id, requested_chat_id);
}
