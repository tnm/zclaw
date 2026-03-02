#ifndef NVS_KEYS_H
#define NVS_KEYS_H

// System/configuration keys stored in NVS namespace "zclaw".
#define NVS_KEY_BOOT_COUNT   "boot_count"
#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_LLM_BACKEND  "llm_backend"
#define NVS_KEY_API_KEY      "api_key"
#define NVS_KEY_LLM_MODEL    "llm_model"
#define NVS_KEY_LLM_API_URL  "llm_api_url"
#define NVS_KEY_TG_TOKEN     "tg_token"
#define NVS_KEY_TG_CHAT_ID   "tg_chat_id"
#define NVS_KEY_TG_CHAT_IDS  "tg_chat_ids"
#define NVS_KEY_EMAIL_BRIDGE_URL "email_br_url"
#define NVS_KEY_EMAIL_BRIDGE_KEY "email_br_key"
#define NVS_KEY_TIMEZONE     "timezone"
#define NVS_KEY_PERSONA      "persona"

// Rate-limit bookkeeping keys.
#define NVS_KEY_RL_DAILY     "rl_daily"
#define NVS_KEY_RL_DAY       "rl_day"
#define NVS_KEY_RL_YEAR      "rl_year"

#endif // NVS_KEYS_H
