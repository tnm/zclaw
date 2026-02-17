#ifndef CONFIG_H
#define CONFIG_H

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
#define LLM_REQUEST_BUF_SIZE    12288   // 12KB for outgoing JSON
#define LLM_RESPONSE_BUF_SIZE   16384   // 16KB for incoming JSON
#define CHANNEL_RX_BUF_SIZE     512     // Input line buffer
#define TOOL_RESULT_BUF_SIZE    512     // Tool execution result

// -----------------------------------------------------------------------------
// Conversation History
// -----------------------------------------------------------------------------
#define MAX_HISTORY_TURNS       8       // User/assistant pairs to keep
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
#define WEBSETUP_TASK_STACK_SIZE 4096
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
} llm_backend_t;

#define LLM_API_URL_ANTHROPIC   "https://api.anthropic.com/v1/messages"
#define LLM_API_URL_OPENAI      "https://api.openai.com/v1/chat/completions"
#define LLM_API_URL_OPENROUTER  "https://openrouter.ai/api/v1/chat/completions"

#define LLM_DEFAULT_MODEL_ANTHROPIC   "claude-sonnet-4-5"
#define LLM_DEFAULT_MODEL_OPENAI      "gpt-5.2"
#define LLM_DEFAULT_MODEL_OPENROUTER  "minimax/minimax-m2.5"

#define LLM_MAX_TOKENS          1024
#define HTTP_TIMEOUT_MS         30000   // 30 seconds for API calls

// -----------------------------------------------------------------------------
// System Prompt
// -----------------------------------------------------------------------------
#define SYSTEM_PROMPT \
    "You are zclaw, an AI agent running on an ESP32 microcontroller. " \
    "You have 400KB of RAM and run on bare metal with FreeRTOS. " \
    "You can control GPIO pins, store persistent memories, and set schedules. " \
    "Be concise - you're on a tiny chip. " \
    "Use your tools to control hardware, remember things, and automate tasks. " \
    "Users can create custom tools with create_tool. When you call a custom tool, " \
    "you'll receive an action to execute - carry it out using your built-in tools."

// -----------------------------------------------------------------------------
// GPIO (safe pins on XIAO ESP32C3)
// -----------------------------------------------------------------------------
#define GPIO_MIN_PIN            2
#define GPIO_MAX_PIN            10

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
#define WIFI_AP_SSID            "zclaw-setup"
#define WIFI_AP_PASS            ""      // Open network for setup
#define WIFI_AP_CHANNEL         1
#define WIFI_AP_MAX_CONN        2

// -----------------------------------------------------------------------------
// Telegram
// -----------------------------------------------------------------------------
#define TELEGRAM_API_URL        "https://api.telegram.org/bot"
#define TELEGRAM_POLL_TIMEOUT   30      // Long polling timeout (seconds)
#define TELEGRAM_POLL_INTERVAL  100     // ms between poll attempts on error
#define TELEGRAM_MAX_MSG_LEN    4096    // Max message length

// -----------------------------------------------------------------------------
// Cron / Scheduler
// -----------------------------------------------------------------------------
#define CRON_CHECK_INTERVAL_MS  60000   // Check schedules every minute
#define CRON_MAX_ENTRIES        16      // Max scheduled tasks
#define CRON_MAX_ACTION_LEN     256     // Max action string length

// -----------------------------------------------------------------------------
// Web Setup Server
// -----------------------------------------------------------------------------
#define WEBSETUP_PORT           80
#define WEBSETUP_MAX_URI_LEN    512
#define WEBSETUP_ENABLE_STA_SERVER 0  // Security: keep setup UI AP-only by default

// -----------------------------------------------------------------------------
// Factory Reset
// -----------------------------------------------------------------------------
#define FACTORY_RESET_PIN       9       // Hold low for 5 seconds to reset
#define FACTORY_RESET_HOLD_MS   5000

// -----------------------------------------------------------------------------
// NTP (time sync)
// -----------------------------------------------------------------------------
#define NTP_SERVER              "pool.ntp.org"
#define NTP_SYNC_TIMEOUT_MS     10000

// -----------------------------------------------------------------------------
// Dynamic Tools
// -----------------------------------------------------------------------------
#define MAX_DYNAMIC_TOOLS       8       // Max user-registered tools
#define TOOL_NAME_MAX_LEN       24
#define TOOL_DESC_MAX_LEN       128

// -----------------------------------------------------------------------------
// Boot Loop Protection
// -----------------------------------------------------------------------------
#define MAX_BOOT_FAILURES       3       // Enter safe mode after N consecutive failures
#define BOOT_SUCCESS_DELAY_MS   30000   // Clear boot counter after this time connected

// -----------------------------------------------------------------------------
// Rate Limiting
// -----------------------------------------------------------------------------
#define RATELIMIT_MAX_PER_HOUR      30      // Max LLM requests per hour
#define RATELIMIT_MAX_PER_DAY       200     // Max LLM requests per day
#define RATELIMIT_ENABLED           1       // Set to 0 to disable

#endif // CONFIG_H
