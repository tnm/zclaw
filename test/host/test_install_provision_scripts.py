#!/usr/bin/env python3
"""Host tests for install/provision shell-script behavior."""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
INSTALL_SH = PROJECT_ROOT / "install.sh"
PROVISION_SH = PROJECT_ROOT / "scripts" / "provision.sh"
ERASE_SH = PROJECT_ROOT / "scripts" / "erase.sh"


def _write_executable(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR)


class InstallProvisionScriptTests(unittest.TestCase):
    def _run_install_with_prefs(self, prefs_text: str, extra_args: list[str]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            home = tmp / "home"
            config_dir = home / ".config" / "zclaw"
            config_dir.mkdir(parents=True, exist_ok=True)
            (config_dir / "install.env").write_text(prefs_text, encoding="utf-8")

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["XDG_CONFIG_HOME"] = str(home / ".config")
            # Keep PATH narrow so QEMU is treated as missing in CI/macOS hosts.
            env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
            env["TERM"] = "dumb"

            cmd = [
                str(INSTALL_SH),
                "--no-build",
                "--no-install-idf",
                "--no-repair-idf",
                "--no-flash",
                "--no-provision",
                "--no-monitor",
                *extra_args,
            ]
            return subprocess.run(
                cmd,
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

    def test_install_auto_applies_saved_qemu_choice(self) -> None:
        prefs = """# zclaw install.sh preferences
INSTALL_IDF=n
REPAIR_IDF=
INSTALL_QEMU=n
INSTALL_CJSON=
BUILD_NOW=n
REPAIR_BUILD_IDF=
FLASH_NOW=
FLASH_MODE=1
PROVISION_NOW=
MONITOR_AFTER_FLASH=
LAST_PORT=
"""
        proc = self._run_install_with_prefs(prefs, [])
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertIn("Install QEMU for ESP32 emulation?: no (saved)", output)

    def test_install_cli_override_beats_saved_qemu_choice(self) -> None:
        prefs = """# zclaw install.sh preferences
INSTALL_IDF=n
REPAIR_IDF=
INSTALL_QEMU=y
INSTALL_CJSON=
BUILD_NOW=n
REPAIR_BUILD_IDF=
FLASH_NOW=
FLASH_MODE=1
PROVISION_NOW=
MONITOR_AFTER_FLASH=
LAST_PORT=
"""
        proc = self._run_install_with_prefs(prefs, ["--no-qemu"])
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertIn("Install QEMU for ESP32 emulation?: no", output)
        self.assertNotIn("Install QEMU for ESP32 emulation?: no (saved)", output)

    def _run_provision_detect(self, env_ssid: str, nmcli_output: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)

            _write_executable(
                bin_dir / "uname",
                "#!/bin/sh\n"
                "echo Linux\n",
            )
            _write_executable(
                bin_dir / "nmcli",
                "#!/bin/sh\n"
                f"printf '%s\\n' '{nmcli_output}'\n",
            )

            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin"
            env["ZCLAW_WIFI_SSID"] = env_ssid
            env["TERM"] = "dumb"

            return subprocess.run(
                [str(PROVISION_SH), "--print-detected-ssid"],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

    def test_provision_detect_ignores_placeholder_env_ssid(self) -> None:
        proc = self._run_provision_detect("<redacted>", "yes:RealNetwork")
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertEqual(proc.stdout.strip(), "RealNetwork")

    def test_provision_detect_uses_non_placeholder_env_ssid(self) -> None:
        proc = self._run_provision_detect(":smiley:", "yes:RealNetwork")
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertEqual(proc.stdout.strip(), ":smiley:")

    def _run_provision_api_check_fail(self, backend: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)

            home = tmp / "home"
            export_dir = home / "esp" / "esp-idf"
            export_dir.mkdir(parents=True, exist_ok=True)
            (export_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            _write_executable(
                bin_dir / "curl",
                "#!/bin/sh\n"
                "out=''\n"
                "while [ $# -gt 0 ]; do\n"
                "  if [ \"$1\" = \"-o\" ]; then out=\"$2\"; shift 2; continue; fi\n"
                "  shift\n"
                "done\n"
                "if [ -n \"$out\" ]; then\n"
                "  printf '%s' '{\"error\":{\"message\":\"invalid api key\"}}' > \"$out\"\n"
                "fi\n"
                "printf '%s' '401'\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin"
            env["TERM"] = "dumb"

            return subprocess.run(
                [
                    str(PROVISION_SH),
                    "--yes",
                    "--port",
                    "/dev/null",
                    "--ssid",
                    "TestNet",
                    "--pass",
                    "password123",
                    "--backend",
                    backend,
                    "--api-key",
                    "sk-test",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

    def test_provision_openai_api_check_runs_in_yes_mode(self) -> None:
        proc = self._run_provision_api_check_fail("openai")
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Verifying OpenAI API key", output)
        self.assertIn("Error: API check failed in --yes mode.", output)

    def test_provision_openrouter_api_check_runs_in_yes_mode(self) -> None:
        proc = self._run_provision_api_check_fail("openrouter")
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Verifying OpenRouter API key", output)
        self.assertIn("Error: API check failed in --yes mode.", output)

    def test_erase_requires_explicit_mode(self) -> None:
        proc = subprocess.run(
            [
                str(ERASE_SH),
                "--yes",
                "--port",
                "/tmp/not-a-real-serial-port",
            ],
            cwd=PROJECT_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("choose one of --nvs or --all", output)

    def test_erase_all_requires_yes_in_non_interactive_shell(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            fake_port = tmp / "ttyUSB0"
            fake_port.touch()

            home = tmp / "home"
            export_dir = home / "esp" / "esp-idf"
            export_dir.mkdir(parents=True, exist_ok=True)
            (export_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            _write_executable(
                bin_dir / "lsof",
                "#!/bin/sh\n"
                "exit 1\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin:/usr/sbin:/sbin"
            env["TERM"] = "dumb"

            proc = subprocess.run(
                [
                    str(ERASE_SH),
                    "--all",
                    "--port",
                    str(fake_port),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("interactive confirmation required in non-interactive mode", output)

    def test_erase_nvs_yes_executes_parttool_erase_partition(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            fake_port = tmp / "ttyUSB0"
            fake_port.touch()

            home = tmp / "home"
            idf_dir = home / "esp" / "esp-idf"
            parttool = idf_dir / "components" / "partition_table" / "parttool.py"
            parttool.parent.mkdir(parents=True, exist_ok=True)
            parttool.write_text("# parttool stub path\n", encoding="utf-8")
            (idf_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            _write_executable(
                bin_dir / "python3",
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$ERASE_ARGS_FILE\"\n",
            )
            _write_executable(
                bin_dir / "lsof",
                "#!/bin/sh\n"
                "exit 1\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin"
            env["TERM"] = "dumb"
            env["ERASE_ARGS_FILE"] = str(tmp / "erase-args.txt")

            proc = subprocess.run(
                [
                    str(ERASE_SH),
                    "--nvs",
                    "--yes",
                    "--port",
                    str(fake_port),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            args_text = (tmp / "erase-args.txt").read_text(encoding="utf-8")

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertIn("erase_partition", args_text)
        self.assertIn("--partition-name", args_text)
        self.assertIn("nvs", args_text)

    def test_erase_all_yes_executes_idf_erase_flash(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            fake_port = tmp / "ttyUSB0"
            fake_port.touch()

            home = tmp / "home"
            export_dir = home / "esp" / "esp-idf"
            export_dir.mkdir(parents=True, exist_ok=True)
            (export_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            _write_executable(
                bin_dir / "idf.py",
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$ERASE_ARGS_FILE\"\n",
            )
            _write_executable(
                bin_dir / "lsof",
                "#!/bin/sh\n"
                "exit 1\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin"
            env["TERM"] = "dumb"
            env["ERASE_ARGS_FILE"] = str(tmp / "erase-args.txt")

            proc = subprocess.run(
                [
                    str(ERASE_SH),
                    "--all",
                    "--yes",
                    "--port",
                    str(fake_port),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            args_text = (tmp / "erase-args.txt").read_text(encoding="utf-8")

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertIn("-p", args_text)
        self.assertIn(str(fake_port), args_text)
        self.assertIn("erase-flash", args_text)


if __name__ == "__main__":
    unittest.main()
