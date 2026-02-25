set(ZCLAW_SRCS
    "main.c"
    "agent.c"
    "channel.c"
    "llm.c"
    "llm_auth.c"
    "tools.c"
    "tools_common.c"
    "tools_gpio.c"
    "tools_i2c.c"
    "tools_memory.c"
    "tools_persona.c"
    "tools_cron.c"
    "tools_system.c"
    "memory.c"
    "json_util.c"
    "telegram.c"
    "telegram_poll_policy.c"
    "telegram_chat_ids.c"
    "telegram_token.c"
    "cron.c"
    "memory_keys.c"
    "telegram_update.c"
    "text_buffer.c"
    "security.c"
    "cron_utils.c"
    "ratelimit.c"
    "ota.c"
    "boot_guard.c"
    "user_tools.c"
    "wifi_credentials.c"
)

if(CONFIG_ZCLAW_VOICE)
    list(APPEND ZCLAW_SRCS "voice.c")
endif()

idf_component_register(
    SRCS ${ZCLAW_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_http_client
        esp_netif
        esp-tls
        esp_timer
        json
        driver
        lwip
        esp_driver_usb_serial_jtag
        app_update
)

set(ZCLAW_VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION")
set(ZCLAW_RELEASE_VERSION "dev")
if(EXISTS "${ZCLAW_VERSION_FILE}")
    file(READ "${ZCLAW_VERSION_FILE}" ZCLAW_RELEASE_VERSION)
    string(STRIP "${ZCLAW_RELEASE_VERSION}" ZCLAW_RELEASE_VERSION)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ZCLAW_VERSION="${ZCLAW_RELEASE_VERSION}")
