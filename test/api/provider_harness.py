#!/usr/bin/env python3
"""Shared tool-calling harness for provider API integration tests."""

from __future__ import annotations

import copy
import json
import os
from dataclasses import dataclass
from typing import Any

import httpx


SYSTEM_PROMPT = """You are zclaw, an AI agent running on an ESP32 microcontroller. \
You have 400KB of RAM and run on bare metal with FreeRTOS. \
You can control GPIO pins, store persistent memories, and set schedules. \
Be concise - you're on a tiny chip. \
Use your tools to control hardware, remember things, and automate tasks. \
Users can create custom tools with create_tool. When you call a custom tool, \
you'll receive an action to execute - carry it out using your built-in tools."""

# Tool definitions matching zclaw's tools.c
TOOLS = [
    {
        "name": "gpio_write",
        "description": "Set a GPIO pin HIGH or LOW. Controls LEDs, relays, outputs.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pin": {"type": "integer", "description": "GPIO pin allowed by GPIO Tool Safety policy"},
                "state": {"type": "integer", "description": "0=LOW, 1=HIGH"},
            },
            "required": ["pin", "state"],
        },
    },
    {
        "name": "gpio_read",
        "description": "Read a GPIO pin state. Returns HIGH or LOW.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pin": {"type": "integer", "description": "GPIO pin allowed by GPIO Tool Safety policy"},
            },
            "required": ["pin"],
        },
    },
    {
        "name": "delay",
        "description": "Wait for specified milliseconds (max 60000). Use between GPIO operations.",
        "input_schema": {
            "type": "object",
            "properties": {
                "milliseconds": {"type": "integer", "description": "Time to wait in ms (max 60000)"},
            },
            "required": ["milliseconds"],
        },
    },
    {
        "name": "memory_set",
        "description": "Store a value in persistent memory. Survives reboots.",
        "input_schema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Key (max 15 chars)"},
                "value": {"type": "string", "description": "Value to store"},
            },
            "required": ["key", "value"],
        },
    },
    {
        "name": "memory_get",
        "description": "Retrieve a value from persistent memory.",
        "input_schema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Key to retrieve"},
            },
            "required": ["key"],
        },
    },
    {
        "name": "memory_list",
        "description": "List all keys stored in persistent memory.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "memory_delete",
        "description": "Delete a key from persistent memory.",
        "input_schema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Key to delete"},
            },
            "required": ["key"],
        },
    },
    {
        "name": "cron_set",
        "description": "Create a scheduled task. Type 'periodic' runs every N minutes. Type 'daily' runs at a specific time. Type 'once' runs one time after N minutes.",
        "input_schema": {
            "type": "object",
            "properties": {
                "type": {"type": "string", "enum": ["periodic", "daily", "once"]},
                "interval_minutes": {"type": "integer", "description": "For periodic: minutes between runs"},
                "delay_minutes": {"type": "integer", "description": "For once: minutes from now before one-time run"},
                "hour": {"type": "integer", "description": "For daily: hour 0-23"},
                "minute": {"type": "integer", "description": "For daily: minute 0-59"},
                "action": {"type": "string", "description": "What to do when triggered"},
            },
            "required": ["type", "action"],
        },
    },
    {
        "name": "cron_list",
        "description": "List all scheduled tasks.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "cron_delete",
        "description": "Delete a scheduled task by ID.",
        "input_schema": {
            "type": "object",
            "properties": {
                "id": {"type": "integer", "description": "Schedule ID to delete"},
            },
            "required": ["id"],
        },
    },
    {
        "name": "get_time",
        "description": "Get current date and time.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "get_version",
        "description": "Get current firmware version.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "get_health",
        "description": "Get device health status: heap memory, rate limits, time sync, version.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "create_tool",
        "description": "Create a custom tool. Provide a short name (no spaces), brief description, and the action to perform when called.",
        "input_schema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Tool name (alphanumeric, no spaces)"},
                "description": {"type": "string", "description": "Short description for tool list"},
                "action": {"type": "string", "description": "What to do when tool is called"},
            },
            "required": ["name", "description", "action"],
        },
    },
    {
        "name": "list_user_tools",
        "description": "List all user-created custom tools.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "delete_user_tool",
        "description": "Delete a user-created custom tool by name.",
        "input_schema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Tool name to delete"},
            },
            "required": ["name"],
        },
    },
]

# Simulated tool results
MOCK_RESULTS = {
    "gpio_write": lambda inp: f"Pin {inp.get('pin')} -> {'HIGH' if inp.get('state') else 'LOW'}",
    "gpio_read": lambda inp: f"Pin {inp.get('pin')} = HIGH",
    "delay": lambda inp: f"Waited {inp.get('milliseconds')} ms",
    "memory_set": lambda inp: f"Saved: {inp.get('key')} = {inp.get('value')}",
    "memory_get": lambda inp: f"{inp.get('key')} = example_value",
    "memory_list": lambda inp: "Stored keys: user_name, last_water",
    "memory_delete": lambda inp: f"Deleted: {inp.get('key')}",
    "cron_set": lambda inp: f"Created schedule #1: {inp.get('type')} -> {inp.get('action')}",
    "cron_list": lambda inp: "No scheduled tasks",
    "cron_delete": lambda inp: f"Deleted schedule #{inp.get('id')}",
    "get_time": lambda inp: "2026-02-21 14:30:00 UTC",
    "get_version": lambda inp: "zclaw v2.0.4",
    "get_health": lambda inp: "Health: OK | Heap: 180000 free | Requests: 5/hr, 20/day | Time: synced",
    "create_tool": lambda inp: f"Created tool '{inp.get('name')}': {inp.get('description')}",
    "list_user_tools": lambda inp: "No user tools defined",
    "delete_user_tool": lambda inp: f"Deleted tool '{inp.get('name')}'",
}


@dataclass(frozen=True)
class ProviderConfig:
    name: str
    api_url: str
    default_model: str
    model_env: str
    api_key_env: str
    wire_format: str


PROVIDERS = {
    "anthropic": ProviderConfig(
        name="anthropic",
        api_url="https://api.anthropic.com/v1/messages",
        default_model="claude-sonnet-4-5",
        model_env="ANTHROPIC_MODEL",
        api_key_env="ANTHROPIC_API_KEY",
        wire_format="anthropic",
    ),
    "openai": ProviderConfig(
        name="openai",
        api_url="https://api.openai.com/v1/chat/completions",
        default_model="gpt-5.2",
        model_env="OPENAI_MODEL",
        api_key_env="OPENAI_API_KEY",
        wire_format="openai",
    ),
    "openrouter": ProviderConfig(
        name="openrouter",
        api_url="https://openrouter.ai/api/v1/chat/completions",
        default_model="minimax/minimax-m2.5",
        model_env="OPENROUTER_MODEL",
        api_key_env="OPENROUTER_API_KEY",
        wire_format="openai",
    ),
}


def _tool_defs_for_provider(provider: ProviderConfig, user_tools: list[dict[str, str]]) -> list[dict[str, Any]]:
    base = copy.deepcopy(TOOLS)
    for ut in user_tools:
        base.append(
            {
                "name": ut["name"],
                "description": ut["description"],
                "input_schema": {"type": "object", "properties": {}},
            }
        )

    if provider.wire_format == "anthropic":
        return base

    return [
        {
            "type": "function",
            "function": {
                "name": tool["name"],
                "description": tool["description"],
                "parameters": tool["input_schema"],
            },
        }
        for tool in base
    ]


def _openai_like_max_tokens_field(model: str) -> tuple[str, int]:
    # Mirror firmware behavior: GPT-5 chat-completions expects max_completion_tokens.
    if model.lower().startswith("gpt-5"):
        return ("max_completion_tokens", 1024)
    return ("max_tokens", 1024)


def call_api(
    provider: ProviderConfig,
    messages: list[dict[str, Any]],
    api_key: str,
    model: str,
    user_tools: list[dict[str, str]],
) -> dict[str, Any]:
    """Make API request to provider."""
    tools = _tool_defs_for_provider(provider, user_tools)

    if provider.wire_format == "anthropic":
        headers = {
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        }
        payload = {
            "model": model,
            "max_tokens": 1024,
            "system": SYSTEM_PROMPT,
            "tools": tools,
            "messages": messages,
        }
    else:
        headers = {
            "Authorization": f"Bearer {api_key}",
            "content-type": "application/json",
        }
        if provider.name == "openrouter":
            headers["HTTP-Referer"] = os.environ.get("OPENROUTER_HTTP_REFERER", "https://github.com/tnm/zclaw")
            headers["X-Title"] = os.environ.get("OPENROUTER_X_TITLE", "zclaw api tests")

        token_field, token_value = _openai_like_max_tokens_field(model)
        payload = {
            "model": model,
            token_field: token_value,
            "messages": messages,
            "tools": tools,
        }

    response = httpx.post(provider.api_url, headers=headers, json=payload, timeout=30)
    response.raise_for_status()
    return response.json()


def execute_tool(name: str, input_data: dict[str, Any], user_tools: list[dict[str, str]]) -> str:
    """Simulate tool execution."""
    for ut in user_tools:
        if ut["name"] == name:
            return f"Execute this action now: {ut['action']}"

    if name in MOCK_RESULTS:
        return str(MOCK_RESULTS[name](input_data))
    return f"Unknown tool: {name}"


def handle_create_tool(input_data: dict[str, Any], user_tools: list[dict[str, str]]) -> str:
    """Track user-created tools in-memory for the current session."""
    user_tools.append(
        {
            "name": str(input_data.get("name", "")),
            "description": str(input_data.get("description", "")),
            "action": str(input_data.get("action", "")),
        }
    )
    return str(MOCK_RESULTS["create_tool"](input_data))


def _extract_anthropic_round(response: dict[str, Any]) -> tuple[str, list[dict[str, Any]], bool]:
    stop_reason = response.get("stop_reason")
    content = response.get("content", [])
    text_response = ""
    tool_uses: list[dict[str, Any]] = []

    for block in content:
        if block.get("type") == "text":
            text_response = str(block.get("text", ""))
        elif block.get("type") == "tool_use":
            tool_uses.append(
                {
                    "id": str(block.get("id", "")),
                    "name": str(block.get("name", "")),
                    "input": block.get("input", {}),
                }
            )

    done = stop_reason == "end_turn" or not tool_uses
    return text_response, tool_uses, done


def _parse_tool_args(arguments_raw: Any) -> dict[str, Any]:
    if isinstance(arguments_raw, dict):
        return arguments_raw
    if not isinstance(arguments_raw, str):
        return {}
    try:
        parsed = json.loads(arguments_raw)
    except json.JSONDecodeError:
        return {}
    if isinstance(parsed, dict):
        return parsed
    return {}


def _extract_openai_round(response: dict[str, Any]) -> tuple[str, list[dict[str, Any]], bool, dict[str, Any]]:
    choices = response.get("choices", [])
    if not choices:
        return "", [], True, {"role": "assistant", "content": ""}

    choice0 = choices[0]
    message = choice0.get("message", {})
    text_response = message.get("content")
    if text_response is None:
        text_response = ""
    text_response = str(text_response)

    raw_tool_calls = message.get("tool_calls", [])
    tool_uses: list[dict[str, Any]] = []
    for call in raw_tool_calls:
        function_block = call.get("function", {})
        tool_uses.append(
            {
                "id": str(call.get("id", "")),
                "name": str(function_block.get("name", "")),
                "input": _parse_tool_args(function_block.get("arguments", "{}")),
            }
        )

    assistant_msg: dict[str, Any] = {"role": "assistant", "content": message.get("content")}
    if raw_tool_calls:
        assistant_msg["tool_calls"] = raw_tool_calls

    done = not tool_uses
    return text_response, tool_uses, done, assistant_msg


def run_conversation(
    provider: ProviderConfig,
    user_message: str,
    api_key: str,
    model: str,
    user_tools: list[dict[str, str]],
    verbose: bool = True,
) -> str:
    """Run a full conversation with tool calling."""
    messages: list[dict[str, Any]] = [{"role": "user", "content": user_message}]

    if verbose:
        print(f"\n{'='*60}")
        print(f"PROVIDER: {provider.name}")
        print(f"MODEL: {model}")
        print(f"USER: {user_message}")
        print("=" * 60)

    max_rounds = 5
    for round_num in range(max_rounds):
        response = call_api(provider, messages, api_key, model, user_tools)

        if provider.wire_format == "anthropic":
            text_response, tool_uses, done = _extract_anthropic_round(response)
            assistant_msg = {"role": "assistant", "content": response.get("content", [])}
            stop_reason = response.get("stop_reason")
        else:
            text_response, tool_uses, done, assistant_msg = _extract_openai_round(response)
            stop_reason = response.get("choices", [{}])[0].get("finish_reason")

        if verbose:
            print(f"\n--- Round {round_num + 1} (stop_reason: {stop_reason}) ---")
            if text_response:
                print(f"TEXT: {text_response}")

        if done:
            if verbose:
                print(f"\n{'='*60}")
                print(f"FINAL: {text_response}")
                print("=" * 60)
            return text_response

        messages.append(assistant_msg)

        if provider.wire_format == "anthropic":
            tool_results = []
            for tool_use in tool_uses:
                tool_name = tool_use["name"]
                tool_id = tool_use["id"]
                tool_input = tool_use["input"]

                if verbose:
                    print(f"TOOL CALL: {tool_name}({json.dumps(tool_input)})")

                if tool_name == "create_tool":
                    result = handle_create_tool(tool_input, user_tools)
                else:
                    result = execute_tool(tool_name, tool_input, user_tools)

                if verbose:
                    print(f"TOOL RESULT: {result}")

                tool_results.append({"type": "tool_result", "tool_use_id": tool_id, "content": result})
            messages.append({"role": "user", "content": tool_results})
        else:
            for tool_use in tool_uses:
                tool_name = tool_use["name"]
                tool_id = tool_use["id"]
                tool_input = tool_use["input"]

                if verbose:
                    print(f"TOOL CALL: {tool_name}({json.dumps(tool_input)})")

                if tool_name == "create_tool":
                    result = handle_create_tool(tool_input, user_tools)
                else:
                    result = execute_tool(tool_name, tool_input, user_tools)

                if verbose:
                    print(f"TOOL RESULT: {result}")

                messages.append({"role": "tool", "tool_call_id": tool_id, "content": result})

    return "(Max rounds reached)"


def interactive_mode(provider: ProviderConfig, api_key: str, model: str) -> None:
    """Interactive REPL mode."""
    user_tools: list[dict[str, str]] = []

    print("\nzclaw API Test Harness")
    print(f"Provider: {provider.name}")
    print(f"Model: {model}")
    print("Type messages to send to the model. Type 'quit' to exit.")
    print("Type 'tools' to see available tools.")
    print("Type 'user_tools' to see created user tools.")
    print()

    while True:
        try:
            user_input = input("You: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nGoodbye!")
            break

        if not user_input:
            continue
        if user_input.lower() == "quit":
            break
        if user_input.lower() == "tools":
            print("\nBuilt-in tools:")
            for tool in TOOLS:
                print(f"  {tool['name']}: {tool['description']}")
            print()
            continue
        if user_input.lower() == "user_tools":
            if user_tools:
                print("\nUser tools:")
                for ut in user_tools:
                    print(f"  {ut['name']}: {ut['description']}")
                    print(f"    Action: {ut['action']}")
            else:
                print("\nNo user tools created yet.")
            print()
            continue

        try:
            run_conversation(provider, user_input, api_key, model, user_tools)
        except httpx.HTTPStatusError as err:
            print(f"API Error: {err.response.status_code} - {err.response.text}")
        except Exception as err:
            print(f"Error: {err}")
