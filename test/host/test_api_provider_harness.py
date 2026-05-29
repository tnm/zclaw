#!/usr/bin/env python3
"""Offline unit tests for test/api/provider_harness.py."""

from __future__ import annotations

import importlib.util
import sys
from types import SimpleNamespace
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch


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
        field, value = provider_harness._openai_like_max_tokens_field("gpt-5.4")
        self.assertEqual(field, "max_completion_tokens")
        self.assertEqual(value, 1024)

    def test_openai_prefixed_gpt5_uses_max_completion_tokens(self) -> None:
        field, value = provider_harness._openai_like_max_tokens_field("openai/gpt-5.2")
        self.assertEqual(field, "max_completion_tokens")
        self.assertEqual(value, 1024)

    def test_openai_non_gpt5_uses_max_tokens(self) -> None:
        field, value = provider_harness._openai_like_max_tokens_field("gpt-4.1-mini")
        self.assertEqual(field, "max_tokens")
        self.assertEqual(value, 1024)

    def test_openai_prefixed_non_gpt5_uses_max_tokens(self) -> None:
        field, value = provider_harness._openai_like_max_tokens_field(
            "openai/gpt-4.1-mini"
        )
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
                                    "arguments": '{"pin":2,"state":1}',
                                },
                            }
                        ],
                    },
                }
            ]
        }
        text, tool_uses, done, assistant_msg = provider_harness._extract_openai_round(
            response
        )
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

    def test_call_api_openai_inserts_system_message_when_missing(self) -> None:
        provider = provider_harness.PROVIDERS["openai"]
        messages = [{"role": "user", "content": "Hello"}]
        payload: dict[str, Any] = {}

        def fake_post(
            url: str, headers: dict[str, str], json: dict[str, Any], timeout: int
        ) -> Mock:
            payload["url"] = url
            payload["headers"] = headers
            payload["json"] = json
            payload["timeout"] = timeout
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"ok": True}
            return response

        with patch.object(provider_harness, "httpx", SimpleNamespace(post=fake_post)):
            result = provider_harness.call_api(
                provider, messages, "test-key", "gpt-4.1-mini", user_tools=[]
            )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(payload["url"], provider.api_url)
        self.assertEqual(payload["timeout"], 30)
        request_json = payload["json"]
        self.assertEqual(
            request_json["messages"][0],
            {"role": "system", "content": provider_harness.SYSTEM_PROMPT},
        )
        self.assertEqual(
            request_json["messages"][1], {"role": "user", "content": "Hello"}
        )
        self.assertEqual(messages, [{"role": "user", "content": "Hello"}])

    def test_call_api_openai_keeps_existing_system_message(self) -> None:
        provider = provider_harness.PROVIDERS["openai"]
        messages = [
            {"role": "system", "content": "custom system prompt"},
            {"role": "user", "content": "Hello"},
        ]
        payload: dict[str, Any] = {}

        def fake_post(
            url: str, headers: dict[str, str], json: dict[str, Any], timeout: int
        ) -> Mock:
            payload["url"] = url
            payload["headers"] = headers
            payload["json"] = json
            payload["timeout"] = timeout
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"ok": True}
            return response

        with patch.object(provider_harness, "httpx", SimpleNamespace(post=fake_post)):
            provider_harness.call_api(
                provider, messages, "test-key", "gpt-4.1-mini", user_tools=[]
            )

        request_json = payload["json"]
        self.assertEqual(request_json["messages"], messages)

    def test_call_api_openrouter_prefixed_gpt5_uses_max_completion_tokens(self) -> None:
        provider = provider_harness.PROVIDERS["openrouter"]
        messages = [{"role": "user", "content": "Hello"}]
        payload: dict[str, Any] = {}

        def fake_post(
            url: str, headers: dict[str, str], json: dict[str, Any], timeout: int
        ) -> Mock:
            payload["url"] = url
            payload["headers"] = headers
            payload["json"] = json
            payload["timeout"] = timeout
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"ok": True}
            return response

        with patch.object(provider_harness, "httpx", SimpleNamespace(post=fake_post)):
            result = provider_harness.call_api(
                provider, messages, "test-key", "openai/gpt-5.2", user_tools=[]
            )

        self.assertEqual(result, {"ok": True})
        request_json = payload["json"]
        self.assertEqual(request_json["model"], "openai/gpt-5.2")
        self.assertEqual(request_json["max_completion_tokens"], 1024)
        self.assertNotIn("max_tokens", request_json)

    def test_call_api_azure_openai_uses_api_key_header_and_env_url(self) -> None:
        provider = provider_harness.PROVIDERS["azure-openai"]
        messages = [{"role": "user", "content": "Hello"}]
        payload: dict[str, Any] = {}

        def fake_post(
            url: str, headers: dict[str, str], json: dict[str, Any], timeout: int
        ) -> Mock:
            payload["url"] = url
            payload["headers"] = headers
            payload["json"] = json
            payload["timeout"] = timeout
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"ok": True}
            return response

        with patch.dict(
            provider_harness.os.environ,
            {
                "AZURE_OPENAI_API_URL": "https://demo.openai.azure.com/openai/responses?api-version=2025-04-01-preview"
            },
            clear=False,
        ):
            with patch.object(
                provider_harness, "httpx", SimpleNamespace(post=fake_post)
            ):
                result = provider_harness.call_api(
                    provider, messages, "test-key", "demo", user_tools=[]
                )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(
            payload["url"],
            "https://demo.openai.azure.com/openai/responses?api-version=2025-04-01-preview",
        )
        self.assertEqual(payload["headers"]["api-key"], "test-key")
        self.assertNotIn("Authorization", payload["headers"])
        self.assertEqual(
            payload["json"]["input"][0],
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "Hello"}],
            },
        )
        self.assertEqual(
            payload["json"]["instructions"], provider_harness.SYSTEM_PROMPT
        )
        self.assertEqual(payload["json"]["parallel_tool_calls"], False)
        self.assertEqual(payload["json"]["reasoning"], {"effort": "low"})
        self.assertEqual(payload["json"]["tools"][0]["type"], "function")
        self.assertIn("name", payload["json"]["tools"][0])

    def test_extract_responses_round_function_call(self) -> None:
        response = {
            "status": "completed",
            "output": [
                {
                    "type": "function_call",
                    "call_id": "call_123",
                    "name": "gpio_write",
                    "arguments": '{"pin":2,"state":1}',
                }
            ],
        }

        text, tool_uses, done, assistant_items = (
            provider_harness._extract_responses_round(response)
        )
        self.assertEqual(text, "")
        self.assertFalse(done)
        self.assertEqual(len(tool_uses), 1)
        self.assertEqual(tool_uses[0]["name"], "gpio_write")
        self.assertEqual(tool_uses[0]["input"]["pin"], 2)
        self.assertEqual(assistant_items[0]["type"], "function_call")

    def test_extract_responses_round_preserves_reasoning_items(self) -> None:
        response = {
            "status": "completed",
            "output": [
                {"id": "rs_123", "type": "reasoning", "summary": []},
                {
                    "type": "function_call",
                    "call_id": "call_123",
                    "name": "gpio_write",
                    "arguments": '{"pin":2,"state":1}',
                },
            ],
        }

        _, tool_uses, done, assistant_items = provider_harness._extract_responses_round(
            response
        )
        self.assertFalse(done)
        self.assertEqual(len(tool_uses), 1)
        self.assertEqual(len(assistant_items), 2)
        self.assertEqual(assistant_items[0]["type"], "reasoning")
        self.assertEqual(assistant_items[1]["type"], "function_call")

    def test_call_api_azure_openai_encodes_assistant_history_as_output_text(
        self,
    ) -> None:
        provider = provider_harness.PROVIDERS["azure-openai"]
        messages = [
            {"role": "assistant", "content": "Earlier answer"},
            {"role": "user", "content": "Next question"},
        ]
        payload: dict[str, Any] = {}

        def fake_post(
            url: str, headers: dict[str, str], json: dict[str, Any], timeout: int
        ) -> Mock:
            payload["json"] = json
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"ok": True}
            return response

        with patch.dict(
            provider_harness.os.environ,
            {
                "AZURE_OPENAI_API_URL": "https://demo.openai.azure.com/openai/responses?api-version=2025-04-01-preview"
            },
            clear=False,
        ):
            with patch.object(
                provider_harness, "httpx", SimpleNamespace(post=fake_post)
            ):
                provider_harness.call_api(
                    provider, messages, "test-key", "demo", user_tools=[]
                )

        self.assertEqual(
            payload["json"]["input"][0]["content"][0]["type"], "output_text"
        )
        self.assertEqual(
            payload["json"]["input"][1]["content"][0]["type"], "input_text"
        )

    def test_call_api_azure_openai_preserves_raw_response_items(self) -> None:
        provider = provider_harness.PROVIDERS["azure-openai"]
        messages = [
            {"id": "rs_123", "type": "reasoning", "summary": []},
            {
                "type": "function_call_output",
                "call_id": "call_123",
                "output": "ok",
            },
        ]
        payload: dict[str, Any] = {}

        def fake_post(
            url: str, headers: dict[str, str], json: dict[str, Any], timeout: int
        ) -> Mock:
            payload["json"] = json
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"ok": True}
            return response

        with patch.dict(
            provider_harness.os.environ,
            {
                "AZURE_OPENAI_API_URL": "https://demo.openai.azure.com/openai/responses?api-version=2025-04-01-preview"
            },
            clear=False,
        ):
            with patch.object(
                provider_harness, "httpx", SimpleNamespace(post=fake_post)
            ):
                provider_harness.call_api(
                    provider, messages, "test-key", "demo", user_tools=[]
                )

        self.assertEqual(payload["json"]["input"][0]["type"], "reasoning")
        self.assertEqual(payload["json"]["input"][1]["type"], "function_call_output")


if __name__ == "__main__":
    unittest.main()
