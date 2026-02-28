#include "voice.h"

#if CONFIG_ZCLAW_VOICE

#include "channel.h"
#include "config.h"
#include "messages.h"
#include "text_buffer.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "voice";
static i2s_chan_handle_t s_voice_rx_chan = NULL;
static QueueHandle_t s_input_queue = NULL;

#define VOICE_STT_REQ_PREFIX "__zclaw_voice_stt_req__:"
#define VOICE_CHUNK_PREFIX   VOICE_STT_REQ_PREFIX "chunk:"
#define VOICE_BEGIN_PREFIX   VOICE_STT_REQ_PREFIX "begin:"
#define VOICE_END_LINE       VOICE_STT_REQ_PREFIX "end"
#define VOICE_START_FRAMES_REQUIRED 2U
#define VOICE_STT_RAW_CHUNK_BYTES   192U
#define VOICE_BASE64_CHUNK_CAP      (((VOICE_STT_RAW_CHUNK_BYTES + 2U) / 3U) * 4U + 1U)

static const char *voice_capture_mode_name(void)
{
#if VOICE_CAPTURE_PDM_MODE
    return "pdm";
#else
    return "std-i2s";
#endif
}

static const char *voice_relay_transport_name(void)
{
#if VOICE_RELAY_TRANSPORT_HTTP
    return "http";
#else
    return "serial";
#endif
}

#if VOICE_CAPTURE_PDM_MODE
static bool voice_pdm_pins_configured(void)
{
    return VOICE_PDM_CLK_GPIO >= 0 &&
           VOICE_PDM_DIN_GPIO >= 0;
}
#else
static bool voice_std_i2s_pins_configured(void)
{
    return VOICE_I2S_BCLK_GPIO >= 0 &&
           VOICE_I2S_WS_GPIO >= 0 &&
           VOICE_I2S_DIN_GPIO >= 0;
}
#endif

static size_t voice_select_utterance_bytes(size_t requested_bytes, size_t min_bytes)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t available;

    if (largest <= VOICE_CAPTURE_ALLOC_RESERVE_BYTES) {
        return 0;
    }

    available = largest - VOICE_CAPTURE_ALLOC_RESERVE_BYTES;
    if (available > requested_bytes) {
        available = requested_bytes;
    }

    available &= ~(sizeof(int16_t) - 1U);
    if (available < min_bytes) {
        return 0;
    }

    return available;
}

static uint32_t voice_mean_abs_pcm(const int16_t *samples, size_t sample_count)
{
    uint64_t sum = 0;

    if (!samples || sample_count == 0) {
        return 0;
    }

    for (size_t i = 0; i < sample_count; i++) {
        int32_t v = samples[i];
        if (v < 0) {
            v = -v;
        }
        sum += (uint32_t)v;
    }

    return (uint32_t)(sum / sample_count);
}

#if VOICE_RELAY_TRANSPORT_SERIAL
static size_t voice_base64_encode(const uint8_t *input,
                                  size_t input_len,
                                  char *output,
                                  size_t output_size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((input_len + 2U) / 3U) * 4U;
    size_t in_idx = 0;
    size_t out_idx = 0;

    if (!output || output_size == 0) {
        return 0;
    }
    if (out_len + 1U > output_size) {
        return 0;
    }

    while (in_idx + 2U < input_len) {
        uint32_t chunk = ((uint32_t)input[in_idx] << 16) |
                         ((uint32_t)input[in_idx + 1U] << 8) |
                         ((uint32_t)input[in_idx + 2U]);
        output[out_idx++] = table[(chunk >> 18) & 0x3F];
        output[out_idx++] = table[(chunk >> 12) & 0x3F];
        output[out_idx++] = table[(chunk >> 6) & 0x3F];
        output[out_idx++] = table[chunk & 0x3F];
        in_idx += 3U;
    }

    if (in_idx < input_len) {
        uint32_t chunk = (uint32_t)input[in_idx] << 16;
        output[out_idx++] = table[(chunk >> 18) & 0x3F];
        if (in_idx + 1U < input_len) {
            chunk |= (uint32_t)input[in_idx + 1U] << 8;
            output[out_idx++] = table[(chunk >> 12) & 0x3F];
            output[out_idx++] = table[(chunk >> 6) & 0x3F];
            output[out_idx++] = '=';
        } else {
            output[out_idx++] = table[(chunk >> 12) & 0x3F];
            output[out_idx++] = '=';
            output[out_idx++] = '=';
        }
    }

    output[out_idx] = '\0';
    return out_idx;
}
#endif

static esp_err_t voice_enqueue_transcript(const char *transcript)
{
    channel_msg_t msg;

    if (!s_input_queue || !transcript || transcript[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&msg, 0, sizeof(msg));
    strncpy(msg.text, transcript, sizeof(msg.text) - 1U);
    msg.text[sizeof(msg.text) - 1U] = '\0';
    msg.source = MSG_SOURCE_VOICE;
    msg.chat_id = 0;

    if (xQueueSend(s_input_queue, &msg, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "Input queue full, dropping voice transcript");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

#if VOICE_RELAY_TRANSPORT_HTTP
typedef struct {
    char *buf;
    size_t len;
    size_t max;
    bool truncated;
} voice_http_response_ctx_t;

static void voice_trim_whitespace(char *text)
{
    size_t len = 0;
    size_t start = 0;
    size_t end;

    if (!text) {
        return;
    }

    len = strlen(text);
    while (start < len) {
        char ch = text[start];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        start++;
    }

    end = len;
    while (end > start) {
        char ch = text[end - 1U];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        end--;
    }

    if (start > 0 && end > start) {
        memmove(text, text + start, end - start);
    }
    text[end - start] = '\0';
}

static esp_err_t voice_http_event_handler(esp_http_client_event_t *evt)
{
    voice_http_response_ctx_t *ctx = (voice_http_response_ctx_t *)evt->user_data;

    if (!ctx || !ctx->buf) {
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (evt->data_len <= 0) {
        return ESP_OK;
    }

    if (!text_buffer_append(ctx->buf, &ctx->len, ctx->max, (const char *)evt->data, evt->data_len)) {
        if (!ctx->truncated) {
            ctx->truncated = true;
            ESP_LOGW(TAG, "Voice HTTP response truncated at %u bytes", (unsigned)(ctx->max - 1U));
        }
    }
    return ESP_OK;
}
#endif

#if VOICE_RELAY_TRANSPORT_SERIAL
static esp_err_t voice_send_audio_to_serial_relay(const int16_t *samples,
                                                  size_t sample_count,
                                                  char *transcript,
                                                  size_t transcript_size)
{
    char line_buf[384];
    char encoded[VOICE_BASE64_CHUNK_CAP];
    bool ok = false;
    const uint8_t *pcm = (const uint8_t *)samples;
    size_t pcm_len = sample_count * sizeof(int16_t);
    size_t offset = 0;
    esp_err_t err;

    err = channel_voice_stt_prepare();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Relay STT prepare failed: %s", esp_err_to_name(err));
        return err;
    }

    int written = snprintf(line_buf, sizeof(line_buf), "%s%d", VOICE_BEGIN_PREFIX, VOICE_SAMPLE_RATE_HZ);
    if (written <= 0 || (size_t)written >= sizeof(line_buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = channel_voice_stt_send_line(line_buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Relay STT begin send failed: %s", esp_err_to_name(err));
        return err;
    }

    while (offset < pcm_len) {
        size_t chunk_len = pcm_len - offset;
        if (chunk_len > VOICE_STT_RAW_CHUNK_BYTES) {
            chunk_len = VOICE_STT_RAW_CHUNK_BYTES;
        }

        size_t encoded_len = voice_base64_encode(pcm + offset, chunk_len, encoded, sizeof(encoded));
        if (encoded_len == 0) {
            ESP_LOGE(TAG, "Failed to base64-encode voice chunk");
            return ESP_ERR_INVALID_SIZE;
        }

        size_t prefix_len = strlen(VOICE_CHUNK_PREFIX);
        if (prefix_len + encoded_len + 1U >= sizeof(line_buf)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(line_buf, VOICE_CHUNK_PREFIX, prefix_len);
        memcpy(line_buf + prefix_len, encoded, encoded_len);
        line_buf[prefix_len + encoded_len] = '\0';

        err = channel_voice_stt_send_line(line_buf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Relay STT chunk send failed: %s", esp_err_to_name(err));
            return err;
        }

        offset += chunk_len;
    }

    err = channel_voice_stt_send_line(VOICE_END_LINE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Relay STT end send failed: %s", esp_err_to_name(err));
        return err;
    }

    err = channel_voice_stt_receive(transcript, transcript_size, &ok, VOICE_RELAY_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Relay STT response timeout/error: %s", esp_err_to_name(err));
        return err;
    }

    if (!ok) {
        ESP_LOGW(TAG, "Relay STT rejected utterance: %s", transcript);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
#endif

#if VOICE_RELAY_TRANSPORT_HTTP
static esp_err_t voice_send_audio_to_http_relay(const int16_t *samples,
                                                size_t sample_count,
                                                char *transcript,
                                                size_t transcript_size)
{
    voice_http_response_ctx_t ctx = {
        .buf = transcript,
        .len = 0,
        .max = transcript_size,
        .truncated = false,
    };
    esp_http_client_config_t config;
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status = -1;
    char sample_rate_buf[12];
    size_t pcm_len = sample_count * sizeof(int16_t);

    if (VOICE_HTTP_STT_URL[0] == '\0') {
        ESP_LOGW(TAG, "Voice relay HTTP URL is not configured; set CONFIG_ZCLAW_VOICE_HTTP_STT_URL");
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm_len > (size_t)INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&config, 0, sizeof(config));
    config.url = VOICE_HTTP_STT_URL;
    config.event_handler = voice_http_event_handler;
    config.user_data = &ctx;
    config.timeout_ms = VOICE_RELAY_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    transcript[0] = '\0';
    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init voice HTTP client");
        return ESP_FAIL;
    }

    snprintf(sample_rate_buf, sizeof(sample_rate_buf), "%d", VOICE_SAMPLE_RATE_HZ);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Zclaw-Sample-Rate", sample_rate_buf);
    esp_http_client_set_header(client, "X-Zclaw-PCM-Format", "s16le");
    if (VOICE_HTTP_API_KEY[0] != '\0') {
        esp_http_client_set_header(client, "X-Zclaw-Key", VOICE_HTTP_API_KEY);
    }
    esp_http_client_set_post_field(client, (const char *)samples, (int)pcm_len);

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice relay HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ctx.truncated) {
        return ESP_ERR_NO_MEM;
    }

    voice_trim_whitespace(transcript);
    if (status != 200) {
        if (transcript[0] != '\0') {
            ESP_LOGW(TAG, "Voice relay HTTP rejected utterance (status=%d): %s", status, transcript);
        } else {
            ESP_LOGW(TAG, "Voice relay HTTP rejected utterance (status=%d)", status);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (transcript[0] == '\0') {
        ESP_LOGW(TAG, "Voice relay HTTP returned empty transcript");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
#endif

static esp_err_t voice_send_audio_to_relay(const int16_t *samples,
                                           size_t sample_count,
                                           char *transcript,
                                           size_t transcript_size)
{
#if !VOICE_RELAY_STT_ENABLED
    (void)samples;
    (void)sample_count;
    (void)transcript;
    (void)transcript_size;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!samples || sample_count == 0 || !transcript || transcript_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

#if VOICE_RELAY_TRANSPORT_HTTP
    return voice_send_audio_to_http_relay(samples, sample_count, transcript, transcript_size);
#else
    return voice_send_audio_to_serial_relay(samples, sample_count, transcript, transcript_size);
#endif
#endif
}

static void voice_process_utterance(const int16_t *samples, size_t sample_count)
{
    char transcript[VOICE_TRANSCRIPT_MAX_LEN + 1U];
    esp_err_t err;

    if (!samples || sample_count == 0) {
        return;
    }

    transcript[0] = '\0';
    err = voice_send_audio_to_relay(samples, sample_count, transcript, sizeof(transcript));
    if (err != ESP_OK) {
        return;
    }
    if (transcript[0] == '\0') {
        ESP_LOGD(TAG, "Relay STT returned empty transcript");
        return;
    }

    ESP_LOGI(TAG, "Voice transcript: %s", transcript);
    voice_enqueue_transcript(transcript);
}

static void voice_capture_task(void *arg)
{
    (void)arg;

    const size_t frame_samples = ((size_t)VOICE_SAMPLE_RATE_HZ * (size_t)VOICE_FRAME_MS) / 1000U;
    const size_t frame_bytes = frame_samples * sizeof(int16_t);
    const size_t min_utterance_samples = ((size_t)VOICE_SAMPLE_RATE_HZ *
                                          (size_t)VOICE_MIN_UTTERANCE_MS) / 1000U;
    size_t max_utterance_samples = ((size_t)VOICE_SAMPLE_RATE_HZ *
                                    (size_t)VOICE_MAX_UTTERANCE_MS) / 1000U;
    const size_t min_utterance_bytes = min_utterance_samples * sizeof(int16_t);
    size_t utterance_bytes = max_utterance_samples * sizeof(int16_t);
    size_t silence_frames_to_end = (size_t)VOICE_SILENCE_END_MS / (size_t)VOICE_FRAME_MS;
    int16_t *frame_buf = NULL;
    int16_t *utterance_buf = NULL;
    bool capturing = false;
    uint32_t start_frames = 0;
    size_t utterance_samples = 0;
    size_t silence_frames = 0;

    if (frame_samples == 0 || max_utterance_samples == 0) {
        ESP_LOGW(TAG, "Invalid voice frame/utterance config; stopping capture task");
        vTaskDelete(NULL);
        return;
    }
    if (silence_frames_to_end == 0) {
        silence_frames_to_end = 1;
    }

    frame_buf = (int16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_8BIT);

#if CONFIG_SPIRAM
    utterance_buf = (int16_t *)heap_caps_malloc(utterance_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    utterance_buf = (int16_t *)heap_caps_malloc(utterance_bytes, MALLOC_CAP_8BIT);
#endif

    if (!utterance_buf) {
        size_t fallback_bytes = voice_select_utterance_bytes(utterance_bytes, min_utterance_bytes);
        if (fallback_bytes >= min_utterance_bytes && fallback_bytes < utterance_bytes) {
            utterance_buf = (int16_t *)heap_caps_malloc(fallback_bytes, MALLOC_CAP_8BIT);
            if (utterance_buf) {
                utterance_bytes = fallback_bytes;
                max_utterance_samples = utterance_bytes / sizeof(int16_t);
                ESP_LOGW(TAG,
                         "Voice capture buffer reduced to %uB (%ums max utterance) due to heap limits",
                         (unsigned)utterance_bytes,
                         (unsigned)((max_utterance_samples * 1000U) / (size_t)VOICE_SAMPLE_RATE_HZ));
            }
        }
    }

    if (!frame_buf || !utterance_buf) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ESP_LOGE(TAG,
                 "Voice capture allocation failed (frame=%uB utterance=%uB min=%uB largest_free=%uB)",
                 (unsigned)frame_bytes,
                 (unsigned)utterance_bytes,
                 (unsigned)min_utterance_bytes,
                 (unsigned)largest);
        free(frame_buf);
        free(utterance_buf);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_voice_rx_chan,
                                         frame_buf,
                                         frame_bytes,
                                         &bytes_read,
                                         1000);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t read_samples = bytes_read / sizeof(int16_t);
        if (read_samples == 0) {
            continue;
        }

        uint32_t level = voice_mean_abs_pcm(frame_buf, read_samples);
        if (!capturing) {
            if (level >= (uint32_t)VOICE_VAD_START_THRESHOLD) {
                start_frames++;
            } else {
                start_frames = 0;
            }

            if (start_frames >= VOICE_START_FRAMES_REQUIRED) {
                capturing = true;
                utterance_samples = 0;
                silence_frames = 0;
                ESP_LOGI(TAG, "Voice activity start (level=%" PRIu32 ")", level);
            } else {
                continue;
            }
        }

        size_t remaining = max_utterance_samples - utterance_samples;
        size_t copy_samples = read_samples;
        if (copy_samples > remaining) {
            copy_samples = remaining;
        }
        if (copy_samples > 0) {
            memcpy(utterance_buf + utterance_samples, frame_buf, copy_samples * sizeof(int16_t));
            utterance_samples += copy_samples;
        }

        if (level < (uint32_t)VOICE_VAD_END_THRESHOLD) {
            silence_frames++;
        } else {
            silence_frames = 0;
        }

        bool reached_max = utterance_samples >= max_utterance_samples;
        bool reached_silence = silence_frames >= silence_frames_to_end;
        if (!reached_max && !reached_silence) {
            continue;
        }

        ESP_LOGI(TAG, "Voice activity end (samples=%u, silence_frames=%u, max=%d)",
                 (unsigned)utterance_samples,
                 (unsigned)silence_frames,
                 reached_max ? 1 : 0);

        if (utterance_samples >= min_utterance_samples) {
            voice_process_utterance(utterance_buf, utterance_samples);
        } else {
            ESP_LOGD(TAG, "Ignoring short utterance (%u samples)", (unsigned)utterance_samples);
        }

        capturing = false;
        start_frames = 0;
        utterance_samples = 0;
        silence_frames = 0;
    }
}

esp_err_t voice_start(QueueHandle_t input_queue)
{
    s_input_queue = input_queue;

    if (!input_queue) {
        return ESP_ERR_INVALID_ARG;
    }
#if VOICE_CAPTURE_PDM_MODE
    if (!voice_pdm_pins_configured()) {
        ESP_LOGW(TAG,
                 "Voice enabled but PDM pins are not configured (CLK=%d, DIN=%d). "
                 "Set CONFIG_ZCLAW_VOICE_PDM_* in menuconfig; skipping voice startup.",
                 VOICE_PDM_CLK_GPIO,
                 VOICE_PDM_DIN_GPIO);
        return ESP_OK;
    }
#else
    if (!voice_std_i2s_pins_configured()) {
        ESP_LOGW(TAG,
                 "Voice enabled but I2S pins are not configured (BCLK=%d, WS=%d, DIN=%d). "
                 "Set CONFIG_ZCLAW_VOICE_I2S_* in menuconfig; skipping voice startup.",
                 VOICE_I2S_BCLK_GPIO,
                 VOICE_I2S_WS_GPIO,
                 VOICE_I2S_DIN_GPIO);
        return ESP_OK;
    }
#endif
#if !VOICE_RELAY_STT_ENABLED
    ESP_LOGW(TAG, "Voice relay STT bridge disabled; skipping voice startup");
    return ESP_OK;
#endif
#if VOICE_RELAY_TRANSPORT_HTTP
    if (VOICE_HTTP_STT_URL[0] == '\0') {
        ESP_LOGW(TAG,
                 "Voice relay transport is HTTP but endpoint URL is empty. "
                 "Set CONFIG_ZCLAW_VOICE_HTTP_STT_URL; skipping voice startup.");
        return ESP_OK;
    }
#endif

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)VOICE_I2S_PORT,
                                                             I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_voice_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(err));
        return err;
    }

#if VOICE_CAPTURE_PDM_MODE
#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = VOICE_PDM_CLK_GPIO,
            .din = VOICE_PDM_DIN_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(s_voice_rx_chan, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S PDM RX mode: %s", esp_err_to_name(err));
        return err;
    }
#else
    ESP_LOGE(TAG, "PDM capture mode selected, but target does not support I2S PDM RX");
    return ESP_ERR_NOT_SUPPORTED;
#endif
#else
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = VOICE_I2S_BCLK_GPIO,
            .ws = VOICE_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = VOICE_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_voice_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD mode: %s", esp_err_to_name(err));
        return err;
    }
#endif

    err = i2s_channel_enable(s_voice_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(voice_capture_task,
                    "voice_capture",
                    VOICE_TASK_STACK_SIZE,
                    NULL,
                    VOICE_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start voice capture task");
        return ESP_FAIL;
    }

#if VOICE_CAPTURE_PDM_MODE
    ESP_LOGI(TAG,
             "Voice relay pipeline started (sample_rate=%dHz frame=%dms mode=%s transport=%s i2s_port=%d pdm_clk=%d pdm_din=%d vad_start=%d vad_end=%d)",
             VOICE_SAMPLE_RATE_HZ,
             VOICE_FRAME_MS,
             voice_capture_mode_name(),
             voice_relay_transport_name(),
             VOICE_I2S_PORT,
             VOICE_PDM_CLK_GPIO,
             VOICE_PDM_DIN_GPIO,
             VOICE_VAD_START_THRESHOLD,
             VOICE_VAD_END_THRESHOLD);
#else
    ESP_LOGI(TAG,
             "Voice relay pipeline started (sample_rate=%dHz frame=%dms mode=%s transport=%s i2s_port=%d bclk=%d ws=%d din=%d vad_start=%d vad_end=%d)",
             VOICE_SAMPLE_RATE_HZ,
             VOICE_FRAME_MS,
             voice_capture_mode_name(),
             voice_relay_transport_name(),
             VOICE_I2S_PORT,
             VOICE_I2S_BCLK_GPIO,
             VOICE_I2S_WS_GPIO,
             VOICE_I2S_DIN_GPIO,
             VOICE_VAD_START_THRESHOLD,
             VOICE_VAD_END_THRESHOLD);
#endif
    return ESP_OK;
}

#endif // CONFIG_ZCLAW_VOICE
