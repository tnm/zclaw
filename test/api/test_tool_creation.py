#!/usr/bin/env python3
"""
Test cases for zclaw tool creation via Anthropic API.

Verifies that Claude generates appropriate tool definitions when asked
to create user tools.

Usage:
    export ANTHROPIC_API_KEY=sk-ant-...
    python test_tool_creation.py
    python test_tool_creation.py -v  # verbose
"""

import os
import sys
import json
import argparse
import httpx

API_URL = "https://api.anthropic.com/v1/messages"
MODEL = os.environ.get("ANTHROPIC_MODEL", "claude-sonnet-4-5")

SYSTEM_PROMPT = """You are zclaw, an AI agent running on an ESP32 microcontroller. \
You have 400KB of RAM and run on bare metal with FreeRTOS. \
You can control GPIO pins, store persistent memories, and set schedules. \
Be concise - you're on a tiny chip. \
Use your tools to control hardware, remember things, and automate tasks. \
Users can create custom tools with create_tool. When you call a custom tool, \
you'll receive an action to execute - carry it out using your built-in tools."""

# Only include create_tool for these tests
TOOLS = [
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
        "name": "gpio_write",
        "description": "Set a GPIO pin HIGH or LOW.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pin": {"type": "integer"},
                "state": {"type": "integer"}
            },
            "required": ["pin", "state"]
        }
    },
    {
        "name": "delay",
        "description": "Wait for specified milliseconds (max 60000).",
        "input_schema": {
            "type": "object",
            "properties": {
                "milliseconds": {"type": "integer"}
            },
            "required": ["milliseconds"]
        }
    },
    {
        "name": "cron_set",
        "description": "Create a scheduled task.",
        "input_schema": {
            "type": "object",
            "properties": {
                "type": {"type": "string", "enum": ["periodic", "daily", "once"]},
                "interval_minutes": {"type": "integer"},
                "delay_minutes": {"type": "integer"},
                "hour": {"type": "integer"},
                "minute": {"type": "integer"},
                "action": {"type": "string"}
            },
            "required": ["type", "action"]
        }
    },
]


def call_api(message):
    """Make single-turn API request."""
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        raise RuntimeError("ANTHROPIC_API_KEY not set")

    headers = {
        "x-api-key": api_key,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
    }

    payload = {
        "model": MODEL,
        "max_tokens": 1024,
        "system": SYSTEM_PROMPT,
        "tools": TOOLS,
        "messages": [{"role": "user", "content": message}],
    }

    response = httpx.post(API_URL, headers=headers, json=payload, timeout=30)
    response.raise_for_status()
    return response.json()


def extract_tool_call(response):
    """Extract tool call from response."""
    for block in response.get("content", []):
        if block.get("type") == "tool_use" and block.get("name") == "create_tool":
            return block.get("input", {})
    return None


class TestResult:
    def __init__(self, name, passed, message, details=None):
        self.name = name
        self.passed = passed
        self.message = message
        self.details = details


def test_simple_gpio_tool(verbose=False):
    """Test: Create a tool to turn on an LED"""
    prompt = "Create a tool to turn on the LED on GPIO 2"

    try:
        response = call_api(prompt)
        tool_call = extract_tool_call(response)

        if verbose:
            print(f"  Response: {json.dumps(tool_call, indent=2)}")

        if not tool_call:
            return TestResult("simple_gpio_tool", False, "No create_tool call found")

        # Check required fields
        name = tool_call.get("name", "")
        desc = tool_call.get("description", "")
        action = tool_call.get("action", "")

        errors = []
        if not name or " " in name:
            errors.append(f"Invalid name: '{name}'")
        if not desc:
            errors.append("Missing description")
        if not action:
            errors.append("Missing action")
        if "gpio" not in action.lower() and "pin" not in action.lower() and "2" not in action:
            errors.append(f"Action doesn't mention GPIO/pin 2: '{action}'")

        if errors:
            return TestResult("simple_gpio_tool", False, "; ".join(errors), tool_call)

        return TestResult("simple_gpio_tool", True, f"name={name}", tool_call)

    except Exception as e:
        return TestResult("simple_gpio_tool", False, str(e))


def test_complex_sequence_tool(verbose=False):
    """Test: Create a tool with multiple steps"""
    prompt = "Create a tool called water_plants that turns GPIO 5 on, waits 30 seconds, then turns it off"

    try:
        response = call_api(prompt)
        tool_call = extract_tool_call(response)

        if verbose:
            print(f"  Response: {json.dumps(tool_call, indent=2)}")

        if not tool_call:
            return TestResult("complex_sequence_tool", False, "No create_tool call found")

        name = tool_call.get("name", "")
        action = tool_call.get("action", "")

        errors = []
        if name != "water_plants":
            errors.append(f"Expected name 'water_plants', got '{name}'")

        # Check action mentions key elements
        action_lower = action.lower()
        if "5" not in action and "gpio" not in action_lower:
            errors.append("Action missing GPIO 5 reference")
        if "30" not in action and "sec" not in action_lower:
            errors.append("Action missing 30 second wait")
        if "off" not in action_lower and "low" not in action_lower and "0" not in action:
            errors.append("Action missing turn off step")

        if errors:
            return TestResult("complex_sequence_tool", False, "; ".join(errors), tool_call)

        return TestResult("complex_sequence_tool", True, f"action length={len(action)}", tool_call)

    except Exception as e:
        return TestResult("complex_sequence_tool", False, str(e))


def test_scheduled_action_tool(verbose=False):
    """Test: Create a tool involving scheduling"""
    prompt = "Create a tool to remind me every hour to drink water"

    try:
        response = call_api(prompt)
        tool_call = extract_tool_call(response)

        if verbose:
            print(f"  Response: {json.dumps(tool_call, indent=2)}")

        if not tool_call:
            return TestResult("scheduled_action_tool", False, "No create_tool call found")

        name = tool_call.get("name", "")
        action = tool_call.get("action", "")

        errors = []
        if not name:
            errors.append("Missing name")
        if " " in name:
            errors.append(f"Name has spaces: '{name}'")

        # Action should mention scheduling or hourly
        action_lower = action.lower()
        if "hour" not in action_lower and "cron" not in action_lower and "schedule" not in action_lower and "60" not in action:
            errors.append(f"Action doesn't mention scheduling: '{action}'")

        if errors:
            return TestResult("scheduled_action_tool", False, "; ".join(errors), tool_call)

        return TestResult("scheduled_action_tool", True, f"name={name}", tool_call)

    except Exception as e:
        return TestResult("scheduled_action_tool", False, str(e))


def test_descriptive_name_generation(verbose=False):
    """Test: Claude generates good tool names from description"""
    prompt = "Create a tool that checks the temperature sensor and stores it in memory"

    try:
        response = call_api(prompt)
        tool_call = extract_tool_call(response)

        if verbose:
            print(f"  Response: {json.dumps(tool_call, indent=2)}")

        if not tool_call:
            return TestResult("descriptive_name", False, "No create_tool call found")

        name = tool_call.get("name", "")
        desc = tool_call.get("description", "")

        errors = []
        if not name:
            errors.append("Missing name")
        if " " in name:
            errors.append(f"Name has spaces: '{name}'")
        if len(name) > 24:
            errors.append(f"Name too long ({len(name)} > 24)")

        # Name should be descriptive (not just "tool1")
        if name.lower() in ["tool", "tool1", "my_tool", "custom_tool"]:
            errors.append(f"Name not descriptive: '{name}'")

        if errors:
            return TestResult("descriptive_name", False, "; ".join(errors), tool_call)

        return TestResult("descriptive_name", True, f"name={name}", tool_call)

    except Exception as e:
        return TestResult("descriptive_name", False, str(e))


def test_action_is_executable(verbose=False):
    """Test: Action should describe steps Claude can execute"""
    prompt = "Create a tool to blink GPIO 3 three times"

    try:
        response = call_api(prompt)
        tool_call = extract_tool_call(response)

        if verbose:
            print(f"  Response: {json.dumps(tool_call, indent=2)}")

        if not tool_call:
            return TestResult("executable_action", False, "No create_tool call found")

        action = tool_call.get("action", "")

        errors = []
        if not action:
            errors.append("Missing action")
        if len(action) < 10:
            errors.append(f"Action too short: '{action}'")
        if len(action) > 256:
            errors.append(f"Action too long ({len(action)} > 256)")

        # Should mention the key elements
        if "3" not in action:
            errors.append("Action doesn't mention GPIO 3 or three times")

        if errors:
            return TestResult("executable_action", False, "; ".join(errors), tool_call)

        return TestResult("executable_action", True, f"action='{action[:50]}...'", tool_call)

    except Exception as e:
        return TestResult("executable_action", False, str(e))


def run_tests(verbose=False):
    """Run all tests."""
    tests = [
        ("Simple GPIO tool", test_simple_gpio_tool),
        ("Complex sequence tool", test_complex_sequence_tool),
        ("Scheduled action tool", test_scheduled_action_tool),
        ("Descriptive name generation", test_descriptive_name_generation),
        ("Executable action format", test_action_is_executable),
    ]

    print(f"\nzclaw Tool Creation Tests (model: {MODEL})")
    print("=" * 60)

    passed = 0
    failed = 0

    for test_name, test_func in tests:
        print(f"\n{test_name}...")
        result = test_func(verbose)

        if result.passed:
            print(f"  ✓ PASS: {result.message}")
            passed += 1
        else:
            print(f"  ✗ FAIL: {result.message}")
            failed += 1

        if verbose and result.details:
            print(f"  Details: {json.dumps(result.details, indent=4)}")

    print("\n" + "=" * 60)
    print(f"Results: {passed} passed, {failed} failed")

    return failed == 0


def main():
    parser = argparse.ArgumentParser(description="Test zclaw tool creation")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show detailed output")
    args = parser.parse_args()

    if not os.environ.get("ANTHROPIC_API_KEY"):
        print("Error: ANTHROPIC_API_KEY environment variable not set")
        sys.exit(1)

    success = run_tests(args.verbose)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
