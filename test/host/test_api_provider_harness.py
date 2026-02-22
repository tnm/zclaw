#!/usr/bin/env python3
"""Offline unit tests for test/api/provider_harness.py."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
HARNESS_PATH = PROJECT_ROOT / "test" / "api" / "provider_harness.py"

SPEC = importlib.util.spec_from_file_location("provider_harness", HARNESS_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Unable to load provider harness from {HARNESS_PATH}")
provider_harness = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = provider_harness
SPEC.loader.exec_module(provider_harness)


class ProviderHarnessTests(unittest.TestCase):
    def test_openai_gpt5_uses_max_completion_tokens(self) -> None:
        field, value = provider_harness._openai_like_max_tokens_field("gpt-5.2")
        self.assertEqual(field, "max_completion_tokens")
        self.assertEqual(value, 1024)

    def test_openai_non_gpt5_uses_max_tokens(self) -> None:
        field, value = provider_harness._openai_like_max_tokens_field("gpt-4o-mini")
        self.assertEqual(field, "max_tokens")
        self.assertEqual(value, 1024)

    def test_extract_anthropic_round_tool_call(self) -> None:
        response = {
            "stop_reason": "tool_use",
            "content": [
                {"type": "text", "text": "Calling tool"},
                {
                    "type": "tool_use",
                    "id": "toolu_123",
                    "name": "gpio_write",
                    "input": {"pin": 2, "state": 1},
                },
            ],
        }
        text, tool_uses, done = provider_harness._extract_anthropic_round(response)
        self.assertEqual(text, "Calling tool")
        self.assertFalse(done)
        self.assertEqual(len(tool_uses), 1)
        self.assertEqual(tool_uses[0]["name"], "gpio_write")
        self.assertEqual(tool_uses[0]["input"]["pin"], 2)

    def test_extract_openai_round_tool_call(self) -> None:
        response = {
            "choices": [
                {
                    "finish_reason": "tool_calls",
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_123",
                                "type": "function",
                                "function": {
                                    "name": "gpio_write",
                                    "arguments": "{\"pin\":2,\"state\":1}",
                                },
                            }
                        ],
                    },
                }
            ]
        }
        text, tool_uses, done, assistant_msg = provider_harness._extract_openai_round(response)
        self.assertEqual(text, "")
        self.assertFalse(done)
        self.assertEqual(len(tool_uses), 1)
        self.assertEqual(tool_uses[0]["name"], "gpio_write")
        self.assertEqual(tool_uses[0]["input"]["pin"], 2)
        self.assertEqual(assistant_msg["role"], "assistant")
        self.assertIn("tool_calls", assistant_msg)

    def test_extract_openai_round_bad_arguments_fallbacks_to_empty_object(self) -> None:
        response = {
            "choices": [
                {
                    "finish_reason": "tool_calls",
                    "message": {
                        "role": "assistant",
                        "content": "",
                        "tool_calls": [
                            {
                                "id": "call_456",
                                "type": "function",
                                "function": {
                                    "name": "gpio_write",
                                    "arguments": "{bad_json",
                                },
                            }
                        ],
                    },
                }
            ]
        }
        _, tool_uses, done, _ = provider_harness._extract_openai_round(response)
        self.assertFalse(done)
        self.assertEqual(tool_uses[0]["input"], {})


if __name__ == "__main__":
    unittest.main()
