#include "tools_handlers.h"
#include "memory.h"
#include "nvs_keys.h"
#include "esp_err.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

static bool canonicalize_persona_name(const char *input, char *output, size_t output_len)
{
    char lowered[24] = {0};
    size_t i = 0;

    if (!input || !output || output_len == 0) {
        return false;
    }

    for (; input[i] != '\0' && i < sizeof(lowered) - 1; i++) {
        lowered[i] = (char)tolower((unsigned char)input[i]);
    }
    lowered[i] = '\0';

    if (strcmp(lowered, "neutral") == 0 ||
        strcmp(lowered, "friendly") == 0 ||
        strcmp(lowered, "technical") == 0 ||
        strcmp(lowered, "witty") == 0) {
        strncpy(output, lowered, output_len - 1);
        output[output_len - 1] = '\0';
        return true;
    }

    return false;
}

static const char *load_current_persona(char *buf, size_t buf_len)
{
    char canonical[24] = {0};

    if (!buf || buf_len == 0) {
        return "neutral";
    }

    if (!memory_get(NVS_KEY_PERSONA, buf, buf_len)) {
        return "neutral";
    }

    if (!canonicalize_persona_name(buf, canonical, sizeof(canonical))) {
        return "neutral";
    }

    strncpy(buf, canonical, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return buf;
}

bool tools_set_persona_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *persona_json = cJSON_GetObjectItem(input, "persona");
    char canonical[24] = {0};
    esp_err_t err;

    if (!persona_json || !cJSON_IsString(persona_json)) {
        snprintf(result, result_len, "Error: 'persona' required (string)");
        return false;
    }

    if (!canonicalize_persona_name(persona_json->valuestring, canonical, sizeof(canonical))) {
        snprintf(result, result_len,
                 "Error: unknown persona '%s' (use neutral, friendly, technical, witty)",
                 persona_json->valuestring ? persona_json->valuestring : "");
        return false;
    }

    err = memory_set(NVS_KEY_PERSONA, canonical);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: %s", esp_err_to_name(err));
        return false;
    }

    snprintf(result, result_len, "Persona set to %s.", canonical);
    return true;
}

bool tools_get_persona_handler(const cJSON *input, char *result, size_t result_len)
{
    char stored[24] = {0};
    const char *current;

    (void)input;

    current = load_current_persona(stored, sizeof(stored));
    snprintf(result, result_len,
             "Current persona: %s. Available: neutral, friendly, technical, witty.",
             current);
    return true;
}

bool tools_reset_persona_handler(const cJSON *input, char *result, size_t result_len)
{
    esp_err_t err;

    (void)input;

    err = memory_set(NVS_KEY_PERSONA, "neutral");
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: %s", esp_err_to_name(err));
        return false;
    }

    snprintf(result, result_len, "Persona reset to neutral.");
    return true;
}
