#include "mock_memory.h"
#include "memory.h"
#include "mock_esp.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MOCK_MEMORY_SLOTS 16
#define MOCK_MEMORY_KEY_LEN 31
#define MOCK_MEMORY_VALUE_LEN 511

typedef struct {
    bool in_use;
    char key[MOCK_MEMORY_KEY_LEN + 1];
    char value[MOCK_MEMORY_VALUE_LEN + 1];
} mock_memory_slot_t;

static mock_memory_slot_t s_slots[MOCK_MEMORY_SLOTS];
static esp_err_t s_next_set_err = ESP_OK;

static int find_slot_index(const char *key)
{
    int i;
    for (i = 0; i < MOCK_MEMORY_SLOTS; i++) {
        if (s_slots[i].in_use && strcmp(s_slots[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot_index(void)
{
    int i;
    for (i = 0; i < MOCK_MEMORY_SLOTS; i++) {
        if (!s_slots[i].in_use) {
            return i;
        }
    }
    return -1;
}

void mock_memory_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_next_set_err = ESP_OK;
}

void mock_memory_set_kv(const char *key, const char *value)
{
    int slot;
    if (!key || key[0] == '\0' || !value) {
        return;
    }

    slot = find_slot_index(key);
    if (slot < 0) {
        slot = find_free_slot_index();
    }
    if (slot < 0) {
        return;
    }

    s_slots[slot].in_use = true;
    strncpy(s_slots[slot].key, key, sizeof(s_slots[slot].key) - 1);
    s_slots[slot].key[sizeof(s_slots[slot].key) - 1] = '\0';
    strncpy(s_slots[slot].value, value, sizeof(s_slots[slot].value) - 1);
    s_slots[slot].value[sizeof(s_slots[slot].value) - 1] = '\0';
}

void mock_memory_fail_next_set(esp_err_t err)
{
    s_next_set_err = err;
}

esp_err_t memory_init(void)
{
    mock_memory_reset();
    return ESP_OK;
}

esp_err_t memory_set(const char *key, const char *value)
{
    if (s_next_set_err != ESP_OK) {
        esp_err_t err = s_next_set_err;
        s_next_set_err = ESP_OK;
        return err;
    }

    if (!key || !value || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    mock_memory_set_kv(key, value);
    return ESP_OK;
}

bool memory_get(const char *key, char *value, size_t max_len)
{
    int slot;
    if (!key || !value || max_len == 0 || key[0] == '\0') {
        return false;
    }

    slot = find_slot_index(key);
    if (slot < 0) {
        return false;
    }

    snprintf(value, max_len, "%s", s_slots[slot].value);
    return true;
}

esp_err_t memory_delete(const char *key)
{
    int slot;
    if (!key || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    slot = find_slot_index(key);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    return ESP_OK;
}
