#ifndef CONFIG_H
#define CONFIG_H

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
#define LLM_REQUEST_BUF_SIZE    12288   // 12KB for outgoing JSON
#define LLM_RESPONSE_BUF_SIZE   16384   // 16KB for incoming JSON
#define CHANNEL_RX_BUF_SIZE     512     // Input line buffer
#define CHANNEL_TX_BUF_SIZE     1024    // Output response buffer for serial/web relay
#define TOOL_RESULT_BUF_SIZE    512     // Tool execution result

// -----------------------------------------------------------------------------
// Conversation History
// -----------------------------------------------------------------------------
#define MAX_HISTORY_TURNS       12      // User/assistant pairs to keep
#define MAX_MESSAGE_LEN         1024    // Max length per message in history

// -----------------------------------------------------------------------------
// Agent Loop
// -----------------------------------------------------------------------------
#define MAX_TOOL_ROUNDS         5       // Max tool call iterations per request

// -----------------------------------------------------------------------------
// FreeRTOS Tasks
// -----------------------------------------------------------------------------
#define AGENT_TASK_STACK_SIZE   8192
#define CHANNEL_TASK_STACK_SIZE 4096
#define CRON_TASK_STACK_SIZE    4096
#define AGENT_TASK_PRIORITY     5
#define CHANNEL_TASK_PRIORITY   5
#define CRON_TASK_PRIORITY      4

// -----------------------------------------------------------------------------
// Queues
// -----------------------------------------------------------------------------
#define INPUT_QUEUE_LENGTH      8
#define OUTPUT_QUEUE_LENGTH     8
#define TELEGRAM_OUTPUT_QUEUE_LENGTH 4

// -----------------------------------------------------------------------------
// LLM Backend Configuration
// -----------------------------------------------------------------------------
typedef enum {
    LLM_BACKEND_ANTHROPIC = 0,
    LLM_BACKEND_OPENAI = 1,
    LLM_BACKEND_OPENROUTER = 2,
    LLM_BACKEND_OLLAMA = 3,
} llm_backend_t;

#define LLM_API_URL_ANTHROPIC   "https://api.anthropic.com/v1/messages"
#define LLM_API_URL_OPENAI      "https://api.openai.com/v1/chat/completions"
#define LLM_API_URL_OPENROUTER  "https://openrouter.ai/api/v1/chat/completions"
// Loopback default is mainly a placeholder for provisioning/runtime override.
#define LLM_API_URL_OLLAMA      "http://127.0.0.1:11434/v1/chat/completions"

#define LLM_DEFAULT_MODEL_ANTHROPIC   "claude-sonnet-4-5"
#define LLM_DEFAULT_MODEL_OPENAI      "gpt-5.2"
#define LLM_DEFAULT_MODEL_OPENROUTER  "minimax/minimax-m2.5"
#define LLM_DEFAULT_MODEL_OLLAMA      "qwen3:8b"

#define LLM_API_KEY_MAX_LEN       511
#define LLM_API_KEY_BUF_SIZE      (LLM_API_KEY_MAX_LEN + 1)
#define LLM_AUTH_HEADER_BUF_SIZE  (sizeof("Bearer ") - 1 + LLM_API_KEY_MAX_LEN + 1)

#define LLM_MAX_TOKENS          1024
#define HTTP_TIMEOUT_MS         30000   // 30 seconds for API calls
#define LLM_HTTP_TIMEOUT_MS     20000   // 20 seconds for LLM API calls
#define LLM_MAX_RETRIES         3       // Max LLM attempts per round (including first attempt)
#define LLM_RETRY_BASE_MS       2000    // Initial retry delay after a failed LLM call
#define LLM_RETRY_MAX_MS        10000   // Maximum exponential retry delay
#define LLM_RETRY_BUDGET_MS     45000   // Max wall-clock retry budget per LLM round

// -----------------------------------------------------------------------------
// System Prompt
// -----------------------------------------------------------------------------
#define SYSTEM_PROMPT \
    "You are zclaw, an AI agent running on an ESP32 microcontroller. " \
    "You have 400KB of RAM and run on bare metal with FreeRTOS. " \
    "You can create and run custom tools, control GPIO pins, store persistent memories, and set schedules. " \
    "You run on the device itself, not as a separate cloud session. " \
    "Be concise - you're on a tiny chip. " \
    "Return plain text only. Do not use markdown, code fences, bullet lists, backticks, " \
    "bold, italics, or headings. " \
    "Use your tools to control hardware, remember things, and automate tasks. " \
    "When summarizing capabilities, prioritize custom tools, schedules, memory, and GPIO before optional i2c_scan details. " \
    "When asked for all or multiple GPIO states, prefer one gpio_read_all call instead of repeated gpio_read calls. " \
    "If users explicitly ask to view or change persona/tone settings, use " \
    "set_persona/get_persona/reset_persona tools. " \
    "Persona is a persistent device setting on this ESP32 and survives reboot until changed or reset. " \
    "Do not change persona based on ambiguous wording or casual chat. " \
    "When asked what is currently saved/set on the device, use tools to verify instead of guessing. " \
    "Users can create custom tools with create_tool. When you call a custom tool, " \
    "you'll receive an action to execute - carry it out using your built-in tools."

// -----------------------------------------------------------------------------
// GPIO tool safety range (configurable via Kconfig)
// -----------------------------------------------------------------------------
#ifdef CONFIG_ZCLAW_GPIO_MIN_PIN
#define GPIO_MIN_PIN            CONFIG_ZCLAW_GPIO_MIN_PIN
#else
#define GPIO_MIN_PIN            2
#endif

#ifdef CONFIG_ZCLAW_GPIO_MAX_PIN
#define GPIO_MAX_PIN            CONFIG_ZCLAW_GPIO_MAX_PIN
#else
#define GPIO_MAX_PIN            10
#endif

#ifdef CONFIG_ZCLAW_GPIO_ALLOWED_PINS
#define GPIO_ALLOWED_PINS_CSV   CONFIG_ZCLAW_GPIO_ALLOWED_PINS
#else
#define GPIO_ALLOWED_PINS_CSV   ""
#endif

#if GPIO_MIN_PIN > GPIO_MAX_PIN
#error "GPIO_MIN_PIN must be <= GPIO_MAX_PIN"
#endif

// -----------------------------------------------------------------------------
// NVS (persistent storage)
// -----------------------------------------------------------------------------
#define NVS_NAMESPACE           "zclaw"
#define NVS_NAMESPACE_CRON      "zc_cron"
#define NVS_NAMESPACE_TOOLS     "zc_tools"
#define NVS_NAMESPACE_CONFIG    "zc_config"
#define NVS_MAX_KEY_LEN         15      // NVS limit
#define NVS_MAX_VALUE_LEN       512     // Increased for tool/cron definitions

// -----------------------------------------------------------------------------
// WiFi
// -----------------------------------------------------------------------------
#define WIFI_MAX_RETRY          10
#define WIFI_RETRY_DELAY_MS     1000

// -----------------------------------------------------------------------------
// Telegram
// -----------------------------------------------------------------------------
#define TELEGRAM_API_URL        "https://api.telegram.org/bot"
#define TELEGRAM_POLL_TIMEOUT   30      // Long polling timeout (seconds)
// OpenRouter can require tighter heap headroom during TLS setup on small targets.
// Use a shorter Telegram long-poll window only for that backend to reduce overlap.
#define TELEGRAM_POLL_TIMEOUT_OPENROUTER 8
#define TELEGRAM_POLL_INTERVAL  100     // ms between poll attempts on error
#define TELEGRAM_MAX_MSG_LEN    4096    // Max message length
#define TELEGRAM_FLUSH_ON_START 1       // Drop stale pending updates at startup
#define TELEGRAM_STALE_POLL_LOG_INTERVAL 4          // Log every N stale-only polls
#define TELEGRAM_STALE_POLL_RESYNC_STREAK 8         // Trigger auto-resync after this streak
#define TELEGRAM_STALE_POLL_RESYNC_COOLDOWN_MS 60000 // Min gap between auto-resync attempts
#define START_COMMAND_COOLDOWN_MS 30000 // Debounce repeated Telegram /start bursts
#define MESSAGE_REPLAY_COOLDOWN_MS 20000 // Suppress repeated identical non-command bursts

// -----------------------------------------------------------------------------
// Cron / Scheduler
// -----------------------------------------------------------------------------
#define CRON_CHECK_INTERVAL_MS  10000   // Check schedules every 10 seconds
#define CRON_MAX_ENTRIES        16      // Max scheduled tasks
#define CRON_MAX_ACTION_LEN     256     // Max action string length

// -----------------------------------------------------------------------------
// Factory Reset
// -----------------------------------------------------------------------------
#ifdef CONFIG_ZCLAW_FACTORY_RESET_PIN
#define FACTORY_RESET_PIN       CONFIG_ZCLAW_FACTORY_RESET_PIN
#else
#define FACTORY_RESET_PIN       9       // Hold low for 5 seconds to reset
#endif

#ifdef CONFIG_ZCLAW_FACTORY_RESET_HOLD_MS
#define FACTORY_RESET_HOLD_MS   CONFIG_ZCLAW_FACTORY_RESET_HOLD_MS
#else
#define FACTORY_RESET_HOLD_MS   5000
#endif

// -----------------------------------------------------------------------------
// NTP (time sync)
// -----------------------------------------------------------------------------
#define NTP_SERVER              "pool.ntp.org"
#define NTP_SYNC_TIMEOUT_MS     10000
#define DEFAULT_TIMEZONE_POSIX  "UTC0"
#define TIMEZONE_MAX_LEN        64

// -----------------------------------------------------------------------------
// Dynamic Tools
// -----------------------------------------------------------------------------
#define MAX_DYNAMIC_TOOLS       8       // Max user-registered tools
#define TOOL_NAME_MAX_LEN       24
#define TOOL_DESC_MAX_LEN       128

// -----------------------------------------------------------------------------
// Boot Loop Protection
// -----------------------------------------------------------------------------
#define MAX_BOOT_FAILURES       4       // Enter safe mode after N consecutive failures
#define BOOT_SUCCESS_DELAY_MS   30000   // Clear boot counter after this time connected

// -----------------------------------------------------------------------------
// Rate Limiting
// -----------------------------------------------------------------------------
#define RATELIMIT_MAX_PER_HOUR      100     // Max LLM requests per hour
#define RATELIMIT_MAX_PER_DAY       1000    // Max LLM requests per day
#define RATELIMIT_ENABLED           1       // Set to 0 to disable

#endif // CONFIG_H
