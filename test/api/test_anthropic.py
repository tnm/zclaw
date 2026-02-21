#!/usr/bin/env python3
"""
Test harness for zclaw tool calling against Anthropic API.

Usage:
    export ANTHROPIC_API_KEY=sk-ant-...
    python test_anthropic.py "Turn on GPIO 5"
    python test_anthropic.py "Create a tool to water plants by turning GPIO 5 on for 30 seconds"
    python test_anthropic.py --interactive
"""

import os
import sys
import json
import argparse
import httpx

API_URL = "https://api.anthropic.com/v1/messages"
DEFAULT_MODEL = "claude-sonnet-4-5"

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
                "state": {"type": "integer", "description": "0=LOW, 1=HIGH"}
            },
            "required": ["pin", "state"]
        }
    },
    {
        "name": "gpio_read",
        "description": "Read a GPIO pin state. Returns HIGH or LOW.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pin": {"type": "integer", "description": "GPIO pin allowed by GPIO Tool Safety policy"}
            },
            "required": ["pin"]
        }
    },
    {
        "name": "delay",
        "description": "Wait for specified milliseconds (max 60000). Use between GPIO operations.",
        "input_schema": {
            "type": "object",
            "properties": {
                "milliseconds": {"type": "integer", "description": "Time to wait in ms (max 60000)"}
            },
            "required": ["milliseconds"]
        }
    },
    {
        "name": "memory_set",
        "description": "Store a value in persistent memory. Survives reboots.",
        "input_schema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Key (max 15 chars)"},
                "value": {"type": "string", "description": "Value to store"}
            },
            "required": ["key", "value"]
        }
    },
    {
        "name": "memory_get",
        "description": "Retrieve a value from persistent memory.",
        "input_schema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Key to retrieve"}
            },
            "required": ["key"]
        }
    },
    {
        "name": "memory_list",
        "description": "List all keys stored in persistent memory.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "memory_delete",
        "description": "Delete a key from persistent memory.",
        "input_schema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Key to delete"}
            },
            "required": ["key"]
        }
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
                "action": {"type": "string", "description": "What to do when triggered"}
            },
            "required": ["type", "action"]
        }
    },
    {
        "name": "cron_list",
        "description": "List all scheduled tasks.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "cron_delete",
        "description": "Delete a scheduled task by ID.",
        "input_schema": {
            "type": "object",
            "properties": {
                "id": {"type": "integer", "description": "Schedule ID to delete"}
            },
            "required": ["id"]
        }
    },
    {
        "name": "get_time",
        "description": "Get current date and time.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "get_version",
        "description": "Get current firmware version.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "get_health",
        "description": "Get device health status: heap memory, rate limits, time sync, version.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "create_tool",
        "description": "Create a custom tool. Provide a short name (no spaces), brief description, and the action to perform when called.",
        "input_schema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Tool name (alphanumeric, no spaces)"},
                "description": {"type": "string", "description": "Short description for tool list"},
                "action": {"type": "string", "description": "What to do when tool is called"}
            },
            "required": ["name", "description", "action"]
        }
    },
    {
        "name": "list_user_tools",
        "description": "List all user-created custom tools.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "delete_user_tool",
        "description": "Delete a user-created custom tool by name.",
        "input_schema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Tool name to delete"}
            },
            "required": ["name"]
        }
    },
]

# Simulated tool results
MOCK_RESULTS = {
    "gpio_write": lambda inp: f"Pin {inp.get('pin')} → {'HIGH' if inp.get('state') else 'LOW'}",
    "gpio_read": lambda inp: f"Pin {inp.get('pin')} = HIGH",
    "delay": lambda inp: f"Waited {inp.get('milliseconds')} ms",
    "memory_set": lambda inp: f"Saved: {inp.get('key')} = {inp.get('value')}",
    "memory_get": lambda inp: f"{inp.get('key')} = example_value",
    "memory_list": lambda inp: "Stored keys: user_name, last_water",
    "memory_delete": lambda inp: f"Deleted: {inp.get('key')}",
    "cron_set": lambda inp: f"Created schedule #1: {inp.get('type')} → {inp.get('action')}",
    "cron_list": lambda inp: "No scheduled tasks",
    "cron_delete": lambda inp: f"Deleted schedule #{inp.get('id')}",
    "get_time": lambda inp: "2025-02-16 14:30:00 PST",
    "get_version": lambda inp: "zclaw v2.0.0",
    "get_health": lambda inp: "Health: OK | Heap: 180000 free | Requests: 5/hr, 20/day | Time: synced",
    "create_tool": lambda inp: f"Created tool '{inp.get('name')}': {inp.get('description')}",
    "list_user_tools": lambda inp: "No user tools defined",
    "delete_user_tool": lambda inp: f"Deleted tool '{inp.get('name')}'",
}

# Track user tools created in this session
user_tools = []


def call_api(messages, api_key, model):
    """Make API request to Anthropic."""
    headers = {
        "x-api-key": api_key,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
    }

    # Include user tools in the tool list
    tools = TOOLS.copy()
    for ut in user_tools:
        tools.append({
            "name": ut["name"],
            "description": ut["description"],
            "input_schema": {"type": "object", "properties": {}}
        })

    payload = {
        "model": model,
        "max_tokens": 1024,
        "system": SYSTEM_PROMPT,
        "tools": tools,
        "messages": messages,
    }

    response = httpx.post(API_URL, headers=headers, json=payload, timeout=30)
    response.raise_for_status()
    return response.json()


def execute_tool(name, input_data):
    """Simulate tool execution."""
    # Check for user tool first
    for ut in user_tools:
        if ut["name"] == name:
            return f"Execute this action now: {ut['action']}"

    if name in MOCK_RESULTS:
        return MOCK_RESULTS[name](input_data)
    return f"Unknown tool: {name}"


def handle_create_tool(input_data):
    """Handle create_tool specially to track user tools."""
    user_tools.append({
        "name": input_data.get("name"),
        "description": input_data.get("description"),
        "action": input_data.get("action"),
    })
    return MOCK_RESULTS["create_tool"](input_data)


def run_conversation(user_message, api_key, model, verbose=True):
    """Run a full conversation with tool calling."""
    messages = [{"role": "user", "content": user_message}]

    if verbose:
        print(f"\n{'='*60}")
        print(f"USER: {user_message}")
        print(f"MODEL: {model}")
        print('='*60)

    max_rounds = 5
    for round_num in range(max_rounds):
        response = call_api(messages, api_key, model)

        stop_reason = response.get("stop_reason")
        content = response.get("content", [])

        if verbose:
            print(f"\n--- Round {round_num + 1} (stop_reason: {stop_reason}) ---")

        # Process content blocks
        text_response = ""
        tool_uses = []

        for block in content:
            if block.get("type") == "text":
                text_response = block.get("text", "")
                if verbose and text_response:
                    print(f"TEXT: {text_response}")
            elif block.get("type") == "tool_use":
                tool_uses.append(block)

        if stop_reason == "end_turn" or not tool_uses:
            # Done - return final text
            if verbose:
                print(f"\n{'='*60}")
                print(f"FINAL: {text_response}")
                print('='*60)
            return text_response

        # Handle tool calls
        # Add assistant message with tool_use blocks
        messages.append({"role": "assistant", "content": content})

        # Execute tools and add results
        tool_results = []
        for tool_use in tool_uses:
            tool_name = tool_use.get("name")
            tool_id = tool_use.get("id")
            tool_input = tool_use.get("input", {})

            if verbose:
                print(f"TOOL CALL: {tool_name}({json.dumps(tool_input)})")

            # Special handling for create_tool
            if tool_name == "create_tool":
                result = handle_create_tool(tool_input)
            else:
                result = execute_tool(tool_name, tool_input)

            if verbose:
                print(f"TOOL RESULT: {result}")

            tool_results.append({
                "type": "tool_result",
                "tool_use_id": tool_id,
                "content": result,
            })

        messages.append({"role": "user", "content": tool_results})

    return "(Max rounds reached)"


def interactive_mode(api_key, model):
    """Interactive REPL mode."""
    print("\nzclaw API Test Harness")
    print(f"Model: {model}")
    print("Type messages to send to Claude. Type 'quit' to exit.")
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
            for t in TOOLS:
                print(f"  {t['name']}: {t['description']}")
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
            run_conversation(user_input, api_key, model)
        except httpx.HTTPStatusError as e:
            print(f"API Error: {e.response.status_code} - {e.response.text}")
        except Exception as e:
            print(f"Error: {e}")


def main():
    parser = argparse.ArgumentParser(description="Test zclaw tool calling with Anthropic API")
    parser.add_argument("message", nargs="?", help="Message to send")
    parser.add_argument("--interactive", "-i", action="store_true", help="Interactive mode")
    parser.add_argument("--quiet", "-q", action="store_true", help="Only show final response")
    parser.add_argument("--model", "-m", help="Model to use (default: from ANTHROPIC_MODEL env or claude-sonnet-4-5-20250514)")
    args = parser.parse_args()

    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        print("Error: ANTHROPIC_API_KEY environment variable not set")
        sys.exit(1)

    model = args.model or os.environ.get("ANTHROPIC_MODEL", DEFAULT_MODEL)

    if args.interactive:
        interactive_mode(api_key, model)
    elif args.message:
        run_conversation(args.message, api_key, model, verbose=not args.quiet)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
