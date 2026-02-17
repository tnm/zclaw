#!/usr/bin/env python3
"""Host tests for the local web preview server helpers."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import sys


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

from web_preview_server import compute_reload_version, inject_live_reload_script


class WebPreviewServerTests(unittest.TestCase):
    def test_inject_live_reload_before_closing_body(self) -> None:
        html = b"<html><body><h1>Test</h1></body></html>"
        output = inject_live_reload_script(html, "abc123", 750)

        self.assertIn(b"/__reload", output)
        self.assertLess(output.find(b"/__reload"), output.lower().rfind(b"</body>"))

    def test_inject_live_reload_without_body_tag_appends(self) -> None:
        html = b"<html><h1>Test</h1></html>"
        output = inject_live_reload_script(html, "abc123", 750)

        self.assertTrue(output.endswith(b"</script>\n"))
        self.assertIn(b"setInterval(checkForChanges, 750)", output)

    def test_reload_version_changes_when_file_is_modified(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "setup.html"
            path.write_text("A", encoding="utf-8")

            v1 = compute_reload_version((path,))

            path.write_text("AB", encoding="utf-8")
            v2 = compute_reload_version((path,))

        self.assertNotEqual(v1, v2)
        self.assertEqual(len(v1), 12)
        self.assertEqual(len(v2), 12)


if __name__ == "__main__":
    unittest.main()
