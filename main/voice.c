#include "voice.h"

#if CONFIG_ZCLAW_VOICE

#include "channel.h"
#include "config.h"
#include "messages.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/task.h"
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

static bool voice_i2s_pins_configured(void)
{
    return VOICE_I2S_BCLK_GPIO >= 0 &&
           VOICE_I2S_WS_GPIO >= 0 &&
           VOICE_I2S_DIN_GPIO >= 0;
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
    char line_buf[384];
    char encoded[VOICE_BASE64_CHUNK_CAP];
    bool ok = false;
    const uint8_t *pcm = (const uint8_t *)samples;
    size_t pcm_len = sample_count * sizeof(int16_t);
    size_t offset = 0;
    esp_err_t err;

    if (!samples || sample_count == 0 || !transcript || transcript_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

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
    const size_t max_utterance_samples = ((size_t)VOICE_SAMPLE_RATE_HZ *
                                          (size_t)VOICE_MAX_UTTERANCE_MS) / 1000U;
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

    frame_buf = (int16_t *)malloc(frame_bytes);
    utterance_buf = (int16_t *)malloc(max_utterance_samples * sizeof(int16_t));
    if (!frame_buf || !utterance_buf) {
        ESP_LOGE(TAG, "Voice capture allocation failed (frame=%uB utterance=%uB)",
                 (unsigned)frame_bytes,
                 (unsigned)(max_utterance_samples * sizeof(int16_t)));
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
    if (!voice_i2s_pins_configured()) {
        ESP_LOGW(TAG,
                 "Voice enabled but I2S pins are not configured (BCLK=%d, WS=%d, DIN=%d). "
                 "Set CONFIG_ZCLAW_VOICE_I2S_* in menuconfig; skipping voice startup.",
                 VOICE_I2S_BCLK_GPIO,
                 VOICE_I2S_WS_GPIO,
                 VOICE_I2S_DIN_GPIO);
        return ESP_OK;
    }
#if !VOICE_RELAY_STT_ENABLED
    ESP_LOGW(TAG, "Voice relay STT bridge disabled; skipping voice startup");
    return ESP_OK;
#endif

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)VOICE_I2S_PORT,
                                                             I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_voice_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(err));
        return err;
    }

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

    ESP_LOGI(TAG,
             "Voice relay pipeline started (sample_rate=%dHz frame=%dms i2s_port=%d bclk=%d ws=%d din=%d vad_start=%d vad_end=%d)",
             VOICE_SAMPLE_RATE_HZ,
             VOICE_FRAME_MS,
             VOICE_I2S_PORT,
             VOICE_I2S_BCLK_GPIO,
             VOICE_I2S_WS_GPIO,
             VOICE_I2S_DIN_GPIO,
             VOICE_VAD_START_THRESHOLD,
             VOICE_VAD_END_THRESHOLD);
    return ESP_OK;
}

#endif // CONFIG_ZCLAW_VOICE
