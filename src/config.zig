//! Configuration constants and runtime config for zedclaw desktop CLI.
//! Replaces config.h and ESP-IDF Kconfig. All ESP32/FreeRTOS constants removed.

const std = @import("std");

// -----------------------------------------------------------------------------
// Version
// -----------------------------------------------------------------------------
pub const VERSION = "3.0.0";

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
pub const LLM_REQUEST_BUF_SIZE: usize = 65536; // 64KB - desktop has plenty
pub const LLM_RESPONSE_BUF_SIZE: usize = 131072; // 128KB
pub const CHANNEL_RX_BUF_SIZE: usize = 4096;
pub const CHANNEL_TX_BUF_SIZE: usize = 16384;
pub const TOOL_RESULT_BUF_SIZE: usize = 4096;

// -----------------------------------------------------------------------------
// Conversation History
// -----------------------------------------------------------------------------
pub const MAX_HISTORY_TURNS: usize = 12;
pub const MAX_MESSAGE_LEN: usize = 4096;

// -----------------------------------------------------------------------------
// Agent Loop
// -----------------------------------------------------------------------------
pub const MAX_TOOL_ROUNDS: usize = 5;

// -----------------------------------------------------------------------------
// LLM Backend Configuration
// -----------------------------------------------------------------------------
pub const LlmBackend = enum {
    anthropic,
    openai,
    openrouter,
    ollama,

    pub fn fromString(s: []const u8) ?LlmBackend {
        if (std.mem.eql(u8, s, "anthropic")) return .anthropic;
        if (std.mem.eql(u8, s, "openai")) return .openai;
        if (std.mem.eql(u8, s, "openrouter")) return .openrouter;
        if (std.mem.eql(u8, s, "ollama")) return .ollama;
        return null;
    }

    pub fn name(self: LlmBackend) []const u8 {
        return switch (self) {
            .anthropic => "Anthropic",
            .openai => "OpenAI",
            .openrouter => "OpenRouter",
            .ollama => "Ollama",
        };
    }

    pub fn defaultApiUrl(self: LlmBackend) []const u8 {
        return switch (self) {
            .anthropic => "https://api.anthropic.com/v1/messages",
            .openai => "https://api.openai.com/v1/chat/completions",
            .openrouter => "https://openrouter.ai/api/v1/chat/completions",
            .ollama => "http://127.0.0.1:11434/v1/chat/completions",
        };
    }

    pub fn defaultModel(self: LlmBackend) []const u8 {
        return switch (self) {
            .anthropic => "claude-opus-4-5",
            .openai => "gpt-4o",
            .openrouter => "anthropic/claude-opus-4-5",
            .ollama => "qwen3:8b",
        };
    }

    pub fn requiresApiKey(self: LlmBackend) bool {
        return self != .ollama;
    }

    pub fn isOpenAiFormat(self: LlmBackend) bool {
        return self == .openai or self == .openrouter or self == .ollama;
    }
};

// -----------------------------------------------------------------------------
// LLM Request Settings
// -----------------------------------------------------------------------------
pub const LLM_MAX_TOKENS: u32 = 1024;
pub const HTTP_TIMEOUT_MS: u64 = 30_000;
pub const LLM_HTTP_TIMEOUT_MS: u64 = 60_000;
pub const LLM_MAX_RETRIES: usize = 3;
pub const LLM_RETRY_BASE_MS: u64 = 2_000;
pub const LLM_RETRY_MAX_MS: u64 = 10_000;
pub const LLM_RETRY_BUDGET_MS: u64 = 90_000;

// -----------------------------------------------------------------------------
// System Prompt
// -----------------------------------------------------------------------------
pub const SYSTEM_PROMPT =
    "You are zedclaw, an AI agent running as a desktop command-line tool. " ++
    "You can create and run custom tools, store persistent memories, and set schedules. " ++
    "Be helpful and concise. " ++
    "Return plain text only. Do not use markdown, code fences, bullet lists, backticks, " ++
    "bold, italics, or headings. " ++
    "Use your tools to remember things and automate tasks. " ++
    "When asked what is currently saved, use tools to verify instead of guessing. " ++
    "Users can create custom tools with create_tool. When you call a custom tool, " ++
    "you'll receive an action to execute - carry it out using your built-in tools.";

// -----------------------------------------------------------------------------
// Storage
// -----------------------------------------------------------------------------
pub const MEMORY_NAMESPACE = "zclaw";
pub const MEMORY_NAMESPACE_CRON = "zc_cron";
pub const MEMORY_NAMESPACE_TOOLS = "zc_tools";
pub const MEMORY_NAMESPACE_CONFIG = "zc_config";
pub const MEMORY_KEY_MAX_LEN: usize = 64;
pub const MEMORY_VALUE_MAX_LEN: usize = 4096;

// -----------------------------------------------------------------------------
// Memory Keys
// -----------------------------------------------------------------------------
pub const KEY_LLM_BACKEND = "llm_backend";
pub const KEY_LLM_MODEL = "llm_model";
pub const KEY_LLM_API_KEY = "llm_api_key";
pub const KEY_LLM_API_URL = "llm_api_url";
pub const KEY_TELEGRAM_TOKEN = "telegram_token";
pub const KEY_TELEGRAM_CHAT_IDS = "telegram_chat_ids";
pub const KEY_TIMEZONE = "timezone";
pub const KEY_PERSONA = "persona";
pub const KEY_RL_DAILY = "rl_daily";
pub const KEY_RL_DAY = "rl_day";
pub const KEY_RL_YEAR = "rl_year";

// -----------------------------------------------------------------------------
// Telegram
// -----------------------------------------------------------------------------
pub const TELEGRAM_API_BASE = "https://api.telegram.org/bot";
pub const TELEGRAM_POLL_TIMEOUT: u32 = 30;
pub const TELEGRAM_POLL_INTERVAL_MS: u64 = 100;
pub const TELEGRAM_MAX_MSG_LEN: usize = 4096;
pub const TELEGRAM_FLUSH_ON_START: bool = true;
pub const START_COMMAND_COOLDOWN_MS: i64 = 30_000;
pub const MESSAGE_REPLAY_COOLDOWN_MS: i64 = 20_000;

// -----------------------------------------------------------------------------
// Cron / Scheduler
// -----------------------------------------------------------------------------
pub const CRON_CHECK_INTERVAL_MS: u64 = 10_000;
pub const CRON_MAX_ENTRIES: usize = 16;
pub const CRON_MAX_ACTION_LEN: usize = 512;

// -----------------------------------------------------------------------------
// Rate Limiting
// -----------------------------------------------------------------------------
pub const RATELIMIT_MAX_PER_HOUR: u32 = 100;
pub const RATELIMIT_MAX_PER_DAY: u32 = 1000;
pub const RATELIMIT_ENABLED: bool = true;

// -----------------------------------------------------------------------------
// Dynamic Tools
// -----------------------------------------------------------------------------
pub const MAX_DYNAMIC_TOOLS: usize = 16;
pub const TOOL_NAME_MAX_LEN: usize = 32;
pub const TOOL_DESC_MAX_LEN: usize = 256;

// -----------------------------------------------------------------------------
// Timezone
// -----------------------------------------------------------------------------
pub const DEFAULT_TIMEZONE = "UTC";
pub const TIMEZONE_MAX_LEN: usize = 64;

// -----------------------------------------------------------------------------
// Input Queue
// -----------------------------------------------------------------------------
pub const INPUT_QUEUE_MAX_LEN: usize = 32;

// -----------------------------------------------------------------------------
// Runtime Config (loaded from env vars or config file)
// -----------------------------------------------------------------------------
pub const RuntimeConfig = struct {
    llm_backend: LlmBackend = .anthropic,
    llm_api_key: []const u8 = "",
    llm_model: []const u8 = "",
    llm_api_url: []const u8 = "",
    telegram_token: []const u8 = "",
    data_dir: []const u8 = "",
    timezone: []const u8 = DEFAULT_TIMEZONE,

    pub fn load(allocator: std.mem.Allocator) !RuntimeConfig {
        var cfg = RuntimeConfig{};

        // Backend
        if (std.posix.getenv("ZEDCLAW_LLM_BACKEND")) |v| {
            cfg.llm_backend = LlmBackend.fromString(v) orelse .anthropic;
        }

        // API key
        cfg.llm_api_key = std.posix.getenv("ZEDCLAW_LLM_API_KEY") orelse "";

        // Model
        cfg.llm_model = std.posix.getenv("ZEDCLAW_LLM_MODEL") orelse cfg.llm_backend.defaultModel();

        // API URL override
        cfg.llm_api_url = std.posix.getenv("ZEDCLAW_LLM_API_URL") orelse "";

        // Telegram
        cfg.telegram_token = std.posix.getenv("ZEDCLAW_TELEGRAM_TOKEN") orelse "";

        // Data directory
        if (std.posix.getenv("ZEDCLAW_DATA_DIR")) |v| {
            cfg.data_dir = v;
        } else {
            // Default: ~/.config/zedclaw
            const home = std.posix.getenv("HOME") orelse "/tmp";
            cfg.data_dir = try std.fs.path.join(allocator, &.{ home, ".config", "zedclaw" });
        }

        // Timezone
        cfg.timezone = std.posix.getenv("ZEDCLAW_TIMEZONE") orelse DEFAULT_TIMEZONE;

        return cfg;
    }

    pub fn apiUrl(self: *const RuntimeConfig) []const u8 {
        if (self.llm_api_url.len > 0) return self.llm_api_url;
        return self.llm_backend.defaultApiUrl();
    }

    pub fn model(self: *const RuntimeConfig) []const u8 {
        if (self.llm_model.len > 0) return self.llm_model;
        return self.llm_backend.defaultModel();
    }
};
