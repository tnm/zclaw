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
PROVISION_DEV_SH = PROJECT_ROOT / "scripts" / "provision-dev.sh"
TELEGRAM_CLEAR_SH = PROJECT_ROOT / "scripts" / "telegram-clear-backlog.sh"
ERASE_SH = PROJECT_ROOT / "scripts" / "erase.sh"
BUILD_SH = PROJECT_ROOT / "scripts" / "build.sh"
FLASH_SH = PROJECT_ROOT / "scripts" / "flash.sh"


def _write_executable(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR)


class InstallProvisionScriptTests(unittest.TestCase):
    def _prepare_fake_idf_env(self, tmp: Path) -> tuple[dict[str, str], Path]:
        home = tmp / "home"
        export_dir = home / "esp" / "esp-idf"
        export_dir.mkdir(parents=True, exist_ok=True)
        (export_dir / "export.sh").write_text(
            "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
            encoding="utf-8",
        )

        bin_dir = tmp / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        env["HOME"] = str(home)
        env["PATH"] = f"{bin_dir}:/usr/bin:/bin:/usr/sbin:/sbin"
        env["TERM"] = "dumb"
        return env, bin_dir

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

    def test_build_box3_passes_expected_idf_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env, bin_dir = self._prepare_fake_idf_env(tmp)
            args_file = tmp / "idf-args.txt"
            env["IDF_ARGS_FILE"] = str(args_file)

            _write_executable(
                bin_dir / "idf.py",
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$IDF_ARGS_FILE\"\n",
            )

            proc = subprocess.run(
                [str(BUILD_SH), "--box-3"],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            args_text = args_file.read_text(encoding="utf-8")
            self.assertIn("IDF_TARGET=esp32s3", args_text)
            self.assertIn("SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32s3-box-3.defaults", args_text)
            self.assertIn("build", args_text)

    def test_flash_box3_passes_expected_idf_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env, bin_dir = self._prepare_fake_idf_env(tmp)
            fake_port = tmp / "ttyUSB0"
            fake_port.touch()
            args_file = tmp / "idf-args.txt"
            env["IDF_ARGS_FILE"] = str(args_file)

            _write_executable(
                bin_dir / "idf.py",
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$IDF_ARGS_FILE\"\n",
            )
            _write_executable(
                bin_dir / "lsof",
                "#!/bin/sh\n"
                "exit 1\n",
            )
            _write_executable(
                bin_dir / "esptool.py",
                "#!/bin/sh\n"
                "cat <<'EOF'\n"
                "Chip is ESP32-S3 (QFN56)\n"
                "MAC: AA:BB:CC:DD:EE:FF\n"
                "EOF\n",
            )
            _write_executable(
                bin_dir / "espefuse.py",
                "#!/bin/sh\n"
                "printf '%s\\n' 'FLASH_CRYPT_CNT = 0'\n",
            )

            proc = subprocess.run(
                [str(FLASH_SH), "--box-3", str(fake_port)],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            args_text = args_file.read_text(encoding="utf-8")
            self.assertIn("IDF_TARGET=esp32s3", args_text)
            self.assertIn("SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32s3-box-3.defaults", args_text)
            self.assertIn("-p", args_text)
            self.assertIn(str(fake_port), args_text)
            self.assertIn("flash", args_text)

    def test_flash_box3_rejects_non_s3_chip(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env, bin_dir = self._prepare_fake_idf_env(tmp)
            fake_port = tmp / "ttyUSB0"
            fake_port.touch()

            _write_executable(
                bin_dir / "lsof",
                "#!/bin/sh\n"
                "exit 1\n",
            )
            _write_executable(
                bin_dir / "esptool.py",
                "#!/bin/sh\n"
                "printf '%s\\n' 'Chip is ESP32-C3 (QFN32)'\n",
            )

            proc = subprocess.run(
                [str(FLASH_SH), "--box-3", str(fake_port)],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertNotEqual(proc.returncode, 0, msg=output)
            self.assertIn("requires target 'esp32s3'", output)
            self.assertIn("ESP32-C3", output)

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

    def _run_provision_api_check_capture_url(
        self,
        *,
        backend: str,
        api_url: str,
    ) -> tuple[subprocess.CompletedProcess[str], str]:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            curl_url_file = tmp / "curl-url.txt"

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
                "url=''\n"
                "while [ $# -gt 0 ]; do\n"
                "  case \"$1\" in\n"
                "    -o)\n"
                "      out=\"$2\"\n"
                "      shift 2\n"
                "      ;;\n"
                "    -w|-H|-d|--connect-timeout|--max-time)\n"
                "      shift 2\n"
                "      ;;\n"
                "    -s|-S|-sS)\n"
                "      shift\n"
                "      ;;\n"
                "    *)\n"
                "      url=\"$1\"\n"
                "      shift\n"
                "      ;;\n"
                "  esac\n"
                "done\n"
                "printf '%s' \"$url\" > \"$CURL_URL_FILE\"\n"
                "if [ -n \"$out\" ]; then\n"
                "  printf '%s' '{\"error\":{\"message\":\"invalid api key\"}}' > \"$out\"\n"
                "fi\n"
                "printf '%s' '401'\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin"
            env["TERM"] = "dumb"
            env["CURL_URL_FILE"] = str(curl_url_file)

            proc = subprocess.run(
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
                    "--api-url",
                    api_url,
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            called_url = curl_url_file.read_text(encoding="utf-8") if curl_url_file.exists() else ""
            return proc, called_url

    def _run_provision_ollama_missing_api_url(self) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            home = tmp / "home"
            export_dir = home / "esp" / "esp-idf"
            export_dir.mkdir(parents=True, exist_ok=True)
            (export_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
            env["TERM"] = "dumb"

            return subprocess.run(
                [
                    str(PROVISION_SH),
                    "--yes",
                    "--skip-api-check",
                    "--port",
                    "/dev/null",
                    "--ssid",
                    "TestNet",
                    "--pass",
                    "password123",
                    "--backend",
                    "ollama",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

    def _run_provision_length_validation(
        self,
        *,
        ssid: str,
        wifi_pass: str,
        tg_chat_ids: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            home = tmp / "home"
            export_dir = home / "esp" / "esp-idf"
            export_dir.mkdir(parents=True, exist_ok=True)
            (export_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
            env["TERM"] = "dumb"

            cmd = [
                str(PROVISION_SH),
                "--yes",
                "--skip-api-check",
                "--port",
                "/dev/null",
                "--ssid",
                ssid,
                "--pass",
                wifi_pass,
                "--backend",
                "openai",
                "--api-key",
                "sk-test",
            ]
            if tg_chat_ids is not None:
                cmd.extend(["--tg-chat-id", tg_chat_ids])

            return subprocess.run(
                cmd,
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

    def test_provision_openai_api_check_uses_models_endpoint_for_chat_override(self) -> None:
        proc, called_url = self._run_provision_api_check_capture_url(
            backend="openai",
            api_url="https://api.openai.com/v1/chat/completions",
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Verifying OpenAI API key", output)
        self.assertEqual(called_url, "https://api.openai.com/v1/models")

    def test_provision_openrouter_api_check_uses_models_endpoint_for_chat_override(self) -> None:
        proc, called_url = self._run_provision_api_check_capture_url(
            backend="openrouter",
            api_url="https://openrouter.ai/api/v1/chat/completions",
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Verifying OpenRouter API key", output)
        self.assertEqual(called_url, "https://openrouter.ai/api/v1/models")

    def test_provision_ollama_requires_api_url_in_yes_mode(self) -> None:
        proc = self._run_provision_ollama_missing_api_url()
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Error: --api-url is required with --backend ollama in --yes mode", output)

    def test_provision_rejects_ssid_longer_than_32_chars(self) -> None:
        proc = self._run_provision_length_validation(
            ssid="S" * 33,
            wifi_pass="password123",
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("SSID must be at most 32 bytes", output)

    def test_provision_rejects_password_longer_than_63_chars(self) -> None:
        proc = self._run_provision_length_validation(
            ssid="HomeNetwork",
            wifi_pass="p" * 64,
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("password exceeds 63 bytes", output)

    def test_provision_rejects_short_nonempty_password(self) -> None:
        proc = self._run_provision_length_validation(
            ssid="HomeNetwork",
            wifi_pass="short7!",
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("password must be 8-63 bytes", output)

    def test_provision_rejects_multibyte_ssid_above_32_bytes(self) -> None:
        proc = self._run_provision_length_validation(
            ssid="\u00e9" * 17,
            wifi_pass="password123",
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("SSID must be at most 32 bytes", output)

    def test_provision_rejects_multibyte_password_above_63_bytes(self) -> None:
        proc = self._run_provision_length_validation(
            ssid="HomeNetwork",
            wifi_pass="\u00e9" * 32,
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("password exceeds 63 bytes", output)

    def test_provision_rejects_more_than_4_telegram_chat_ids(self) -> None:
        proc = self._run_provision_length_validation(
            ssid="HomeNetwork",
            wifi_pass="password123",
            tg_chat_ids="1,2,3,4,5",
        )
        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Use 1-4 non-zero integers", output)

    def test_provision_writes_chat_id_allowlist_and_legacy_primary_key(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            home = tmp / "home"
            idf_dir = home / "esp" / "esp-idf"
            nvs_gen = idf_dir / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
            parttool = idf_dir / "components" / "partition_table" / "parttool.py"
            nvs_gen.parent.mkdir(parents=True, exist_ok=True)
            parttool.parent.mkdir(parents=True, exist_ok=True)
            nvs_gen.write_text("# nvs generator stub path\n", encoding="utf-8")
            parttool.write_text("# parttool stub path\n", encoding="utf-8")
            (idf_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            _write_executable(
                bin_dir / "python",
                "#!/bin/sh\n"
                "if [ \"$2\" = \"generate\" ]; then\n"
                "  cp \"$3\" \"$CSV_CAPTURE\"\n"
                "  : > \"$4\"\n"
                "  exit 0\n"
                "fi\n"
                "exit 0\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin:/usr/sbin:/sbin"
            env["TERM"] = "dumb"
            env["CSV_CAPTURE"] = str(tmp / "captured-nvs.csv")

            proc = subprocess.run(
                [
                    str(PROVISION_SH),
                    "--yes",
                    "--skip-api-check",
                    "--port",
                    "/dev/null",
                    "--ssid",
                    "HomeNetwork",
                    "--pass",
                    "password123",
                    "--backend",
                    "openai",
                    "--api-key",
                    "sk-test",
                    "--tg-token",
                    "123456789:abcdef",
                    "--tg-chat-id",
                    "7585013353,-100222333444",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)

            captured_csv = (tmp / "captured-nvs.csv").read_text(encoding="utf-8")
            self.assertIn('tg_chat_id,data,string,"7585013353"', captured_csv)
            self.assertIn('tg_chat_ids,data,string,"7585013353,-100222333444"', captured_csv)

    def test_provision_ollama_writes_normalized_api_url_without_api_key(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            home = tmp / "home"
            idf_dir = home / "esp" / "esp-idf"
            nvs_gen = idf_dir / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
            parttool = idf_dir / "components" / "partition_table" / "parttool.py"
            nvs_gen.parent.mkdir(parents=True, exist_ok=True)
            parttool.parent.mkdir(parents=True, exist_ok=True)
            nvs_gen.write_text("# nvs generator stub path\n", encoding="utf-8")
            parttool.write_text("# parttool stub path\n", encoding="utf-8")
            (idf_dir / "export.sh").write_text(
                "export IDF_PATH=\"$HOME/esp/esp-idf\"\n",
                encoding="utf-8",
            )

            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            _write_executable(
                bin_dir / "python",
                "#!/bin/sh\n"
                "if [ \"$2\" = \"generate\" ]; then\n"
                "  cp \"$3\" \"$CSV_CAPTURE\"\n"
                "  : > \"$4\"\n"
                "  exit 0\n"
                "fi\n"
                "exit 0\n",
            )

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin:/usr/sbin:/sbin"
            env["TERM"] = "dumb"
            env["CSV_CAPTURE"] = str(tmp / "captured-nvs.csv")

            proc = subprocess.run(
                [
                    str(PROVISION_SH),
                    "--yes",
                    "--skip-api-check",
                    "--port",
                    "/dev/null",
                    "--ssid",
                    "HomeNetwork",
                    "--pass",
                    "password123",
                    "--backend",
                    "ollama",
                    "--api-url",
                    "http://192.168.1.10:11434",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)

            captured_csv = (tmp / "captured-nvs.csv").read_text(encoding="utf-8")
            self.assertIn('llm_backend,data,string,"ollama"', captured_csv)
            self.assertIn('api_key,data,string,""', captured_csv)
            self.assertIn(
                'llm_api_url,data,string,"http://192.168.1.10:11434/v1/chat/completions"',
                captured_csv,
            )

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

    def test_provision_dev_write_template_creates_profile(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                    "--write-template",
                ],
                cwd=PROJECT_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            self.assertTrue(env_file.exists(), msg=output)
            content = env_file.read_text(encoding="utf-8")
            self.assertIn("ZCLAW_WIFI_SSID", content)
            self.assertIn("ZCLAW_API_KEY", content)
            self.assertIn("ZCLAW_API_URL", content)

    def test_provision_dev_forwards_profile_values(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            args_file = tmp / "args.txt"
            stub = tmp / "mock-provision.sh"

            _write_executable(
                stub,
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$ARGS_FILE\"\n",
            )
            env_file.write_text(
                "\n".join(
                    [
                        "ZCLAW_PORT=/dev/cu.usbmodem1101",
                        "ZCLAW_WIFI_SSID=Trident",
                        "ZCLAW_WIFI_PASS=Bolinas1001",
                        "ZCLAW_BACKEND=openai",
                        "ZCLAW_MODEL=gpt-5.2",
                        "ZCLAW_API_KEY=sk-test-1234567890",
                        "ZCLAW_TG_TOKEN=123456789:abcdef",
                        "ZCLAW_TG_CHAT_ID=7585013353",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["ARGS_FILE"] = str(args_file)
            env["ZCLAW_PROVISION_SCRIPT"] = str(stub)

            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                    "--show-config",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            self.assertTrue(args_file.exists(), msg=output)
            args_text = args_file.read_text(encoding="utf-8")
            self.assertIn("--yes", args_text)
            self.assertIn("--ssid", args_text)
            self.assertIn("Trident", args_text)
            self.assertIn("--api-key", args_text)
            self.assertIn("sk-test-1234567890", args_text)
            self.assertIn("--tg-token", args_text)
            self.assertIn("123456789:abcdef", args_text)
            self.assertIn("Telegram bot ID: 123456789", output)
            self.assertNotIn("123456789:abcdef", output)
            self.assertNotIn("sk-test-1234567890", output)

    def test_provision_dev_uses_provider_specific_env_key(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            args_file = tmp / "args.txt"
            stub = tmp / "mock-provision.sh"

            _write_executable(
                stub,
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$ARGS_FILE\"\n",
            )
            env_file.write_text(
                "\n".join(
                    [
                        "ZCLAW_WIFI_SSID=Trident",
                        "ZCLAW_BACKEND=openrouter",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["ARGS_FILE"] = str(args_file)
            env["ZCLAW_PROVISION_SCRIPT"] = str(stub)
            env["OPENROUTER_API_KEY"] = "or-sk-test-xyz"

            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            args_text = args_file.read_text(encoding="utf-8")
            self.assertIn("--backend", args_text)
            self.assertIn("openrouter", args_text)
            self.assertIn("--api-key", args_text)
            self.assertIn("or-sk-test-xyz", args_text)

    def test_provision_dev_forwards_multi_telegram_chat_ids(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            args_file = tmp / "args.txt"
            stub = tmp / "mock-provision.sh"

            _write_executable(
                stub,
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$ARGS_FILE\"\n",
            )
            env_file.write_text(
                "\n".join(
                    [
                        "ZCLAW_WIFI_SSID=Trident",
                        "ZCLAW_BACKEND=openai",
                        "ZCLAW_API_KEY=sk-test-1234567890",
                        "ZCLAW_TG_TOKEN=123456789:abcdef",
                        "ZCLAW_TG_CHAT_IDS=7585013353,-100222333444",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["ARGS_FILE"] = str(args_file)
            env["ZCLAW_PROVISION_SCRIPT"] = str(stub)

            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                    "--show-config",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            args_text = args_file.read_text(encoding="utf-8")
            self.assertIn("--tg-chat-id", args_text)
            self.assertIn("7585013353,-100222333444", args_text)
            self.assertIn("Telegram chat ID(s):", output)

    def test_provision_dev_errors_when_key_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            env_file.write_text(
                "\n".join(
                    [
                        "ZCLAW_WIFI_SSID=Trident",
                        "ZCLAW_BACKEND=openai",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()
            env.pop("OPENAI_API_KEY", None)
            env.pop("ANTHROPIC_API_KEY", None)
            env.pop("OPENROUTER_API_KEY", None)
            env.pop("ZCLAW_API_KEY", None)

            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Error: API key not set.", output)

    def test_provision_dev_ollama_requires_api_url(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            env_file.write_text(
                "\n".join(
                    [
                        "ZCLAW_WIFI_SSID=Trident",
                        "ZCLAW_BACKEND=ollama",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()

            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Error: API URL not set for Ollama backend.", output)

    def test_provision_dev_ollama_forwards_api_url_without_api_key(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            args_file = tmp / "args.txt"
            stub = tmp / "mock-provision.sh"

            _write_executable(
                stub,
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$ARGS_FILE\"\n",
            )
            env_file.write_text(
                "\n".join(
                    [
                        "ZCLAW_WIFI_SSID=Trident",
                        "ZCLAW_BACKEND=ollama",
                        "ZCLAW_API_URL=http://192.168.1.10:11434/v1/chat/completions",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["ARGS_FILE"] = str(args_file)
            env["ZCLAW_PROVISION_SCRIPT"] = str(stub)

            proc = subprocess.run(
                [
                    str(PROVISION_DEV_SH),
                    "--env-file",
                    str(env_file),
                    "--show-config",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            output = f"{proc.stdout}\n{proc.stderr}"
            self.assertEqual(proc.returncode, 0, msg=output)
            self.assertTrue(args_file.exists(), msg=output)
            args_text = args_file.read_text(encoding="utf-8")
            self.assertIn("--backend", args_text)
            self.assertIn("ollama", args_text)
            self.assertIn("--api-url", args_text)
            self.assertIn("http://192.168.1.10:11434/v1/chat/completions", args_text)
            self.assertNotIn("--api-key", args_text)
            self.assertIn("API URL: http://192.168.1.10:11434/v1/chat/completions", output)

    def test_telegram_clear_backlog_errors_without_token(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            env_file.write_text("# no token\n", encoding="utf-8")

            env = os.environ.copy()
            env.pop("ZCLAW_TG_TOKEN", None)

            proc = subprocess.run(
                [
                    str(TELEGRAM_CLEAR_SH),
                    "--env-file",
                    str(env_file),
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertNotEqual(proc.returncode, 0, msg=output)
        self.assertIn("Error: Telegram token not set.", output)

    def test_telegram_clear_backlog_uses_profile_token_and_advances_offset(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            env_file = tmp / "dev.env"
            env_file.write_text(
                "ZCLAW_TG_TOKEN=123456789:abcdef\n",
                encoding="utf-8",
            )

            bin_dir = tmp / "bin"
            bin_dir.mkdir(parents=True, exist_ok=True)
            _write_executable(
                bin_dir / "curl",
                "#!/bin/sh\n"
                "out=''\n"
                "fmt=''\n"
                "url=''\n"
                "while [ $# -gt 0 ]; do\n"
                "  case \"$1\" in\n"
                "    -o) out=\"$2\"; shift 2 ;;\n"
                "    -w) fmt=\"$2\"; shift 2 ;;\n"
                "    --connect-timeout|--max-time) shift 2 ;;\n"
                "    -s|-S|-sS) shift ;;\n"
                "    *) url=\"$1\"; shift ;;\n"
                "  esac\n"
                "done\n"
                "printf '%s\\n' \"$url\" >> \"$CURL_URLS_FILE\"\n"
                "code='200'\n"
                "if echo \"$url\" | grep -q 'offset=-1'; then\n"
                "  body='{\"ok\":true,\"result\":[{\"update_id\":4242}]}'\n"
                "elif echo \"$url\" | grep -q 'offset=4243'; then\n"
                "  body='{\"ok\":true,\"result\":[]}'\n"
                "else\n"
                "  code='400'\n"
                "  body='{\"ok\":false,\"error_code\":400,\"description\":\"bad offset\"}'\n"
                "fi\n"
                "if [ -n \"$out\" ]; then\n"
                "  printf '%s' \"$body\" > \"$out\"\n"
                "fi\n"
                "if [ -n \"$fmt\" ]; then\n"
                "  printf '%s' \"$code\"\n"
                "else\n"
                "  printf '%s' \"$body\"\n"
                "fi\n",
            )

            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:/usr/bin:/bin"
            env["CURL_URLS_FILE"] = str(tmp / "urls.txt")
            env.pop("ZCLAW_TG_TOKEN", None)

            proc = subprocess.run(
                [
                    str(TELEGRAM_CLEAR_SH),
                    "--env-file",
                    str(env_file),
                    "--show-config",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )

            urls = (tmp / "urls.txt").read_text(encoding="utf-8")

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertIn("Cleared Telegram backlog up to update_id=4242", output)
        self.assertIn("Bot ID: 123456789", output)
        self.assertNotIn("123456789:abcdef", output)
        self.assertIn("offset=-1", urls)
        self.assertIn("offset=4243", urls)

    def test_telegram_clear_backlog_dry_run(self) -> None:
        proc = subprocess.run(
            [
                str(TELEGRAM_CLEAR_SH),
                "--token",
                "123456789:abcdef",
                "--dry-run",
            ],
            cwd=PROJECT_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

        output = f"{proc.stdout}\n{proc.stderr}"
        self.assertEqual(proc.returncode, 0, msg=output)
        self.assertIn("Dry run requests:", output)
        self.assertIn("offset=-1", output)
        self.assertIn("offset=<latest+1>", output)


if __name__ == "__main__":
    unittest.main()
