#!/usr/bin/env python3
"""Unit tests for qemu_live_llm_bridge helpers."""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

from qemu_live_llm_bridge import (
    build_error_payload,
    call_provider,
    compact_json_or_error,
    detect_provider_from_request,
    resolve_provider,
)


class QemuLiveLlmBridgeTests(unittest.TestCase):
    def test_build_error_payload_shape(self) -> None:
        payload = json.loads(build_error_payload("boom"))
        self.assertIn("error", payload)
        self.assertIn("message", payload["error"])
        self.assertIn("boom", payload["error"]["message"])

    def test_compact_json_minifies(self) -> None:
        compacted = compact_json_or_error('{ "ok": true, "n": 1 }')
        self.assertEqual(compacted, '{"ok":true,"n":1}')

    def test_compact_json_handles_non_json(self) -> None:
        payload = json.loads(compact_json_or_error("not json"))
        self.assertIn("error", payload)
        self.assertIn("message", payload["error"])

    def test_detect_provider_from_anthropic_shape(self) -> None:
        request = json.dumps(
            {
                "model": "claude-sonnet-4-6",
                "system": "You are helpful.",
                "messages": [{"role": "user", "content": "hi"}],
                "tools": [
                    {
                        "name": "gpio_write",
                        "description": "write pin",
                        "input_schema": {"type": "object"},
                    }
                ],
            }
        )
        self.assertEqual(detect_provider_from_request(request), "anthropic")

    def test_detect_provider_from_openai_shape(self) -> None:
        request = json.dumps(
            {
                "model": "gpt-5.2",
                "messages": [
                    {"role": "system", "content": "You are helpful."},
                    {"role": "user", "content": "hi"},
                ],
                "tools": [
                    {
                        "type": "function",
                        "function": {
                            "name": "gpio_write",
                            "description": "write pin",
                            "parameters": {"type": "object"},
                        },
                    }
                ],
            }
        )
        self.assertEqual(detect_provider_from_request(request), "openai")

    def test_detect_provider_from_responses_shape(self) -> None:
        request = json.dumps(
            {
                "model": "gpt-5.4",
                "instructions": "You are helpful.",
                "input": [
                    {
                        "type": "message",
                        "role": "user",
                        "content": [{"type": "input_text", "text": "hi"}],
                    }
                ],
                "tools": [
                    {
                        "type": "function",
                        "name": "gpio_write",
                        "description": "write pin",
                        "parameters": {"type": "object"},
                    }
                ],
            }
        )
        self.assertEqual(detect_provider_from_request(request), "azure-openai")

    def test_detect_provider_defaults_to_openai_for_invalid_json(self) -> None:
        self.assertEqual(detect_provider_from_request("not-json"), "openai")

    def test_detect_provider_defaults_to_openai_for_ambiguous_shape(self) -> None:
        request = json.dumps(
            {
                "model": "any-model",
                "messages": [{"role": "user", "content": "hi"}],
            }
        )
        self.assertEqual(detect_provider_from_request(request), "openai")

    def test_resolve_provider_auto_uses_detection(self) -> None:
        request = json.dumps(
            {
                "model": "gpt-5.2",
                "messages": [{"role": "system", "content": "x"}],
                "tools": [{"type": "function", "function": {"name": "test"}}],
            }
        )
        self.assertEqual(resolve_provider("auto", request), "openai")
        self.assertEqual(resolve_provider("anthropic", request), "anthropic")

    def test_resolve_provider_auto_detects_responses_shape(self) -> None:
        request = json.dumps(
            {
                "model": "gpt-5.4",
                "instructions": "x",
                "input": [
                    {
                        "type": "message",
                        "role": "user",
                        "content": [{"type": "input_text", "text": "hi"}],
                    }
                ],
            }
        )
        self.assertEqual(resolve_provider("auto", request), "azure-openai")

    def test_call_provider_azure_openai_requires_env_vars(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            payload = json.loads(call_provider("azure-openai", "{}", 1))
        self.assertIn("error", payload)
        self.assertIn("AZURE_OPENAI_API_KEY", payload["error"]["message"])


if __name__ == "__main__":
    unittest.main()
