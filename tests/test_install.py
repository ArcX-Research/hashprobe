"""Check installed commands and MCP registration without touching user settings."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("HASHPROBE_TEST_BUILD", str(ROOT / "build"))).resolve()
OPENSSL = shutil.which("openssl")


class InstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory(prefix="hashprobe-install-")
        cls.addClassCleanup(directory.cleanup)
        cls.stage = Path(directory.name).resolve()
        cls.prefix = "/opt/hashprobe tools"
        cls.bin = cls.stage / cls.prefix.lstrip("/") / "bin"
        installed = subprocess.run(
            ["make", "install", f"BUILD={BUILD}", f"DESTDIR={cls.stage}", f"PREFIX={cls.prefix}"],
            cwd=ROOT, capture_output=True, text=True, timeout=60,
        )
        if installed.returncode:
            raise RuntimeError(installed.stdout + installed.stderr)

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hashprobe-setup-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name).resolve()
        self.user_dir = self.directory / "user"
        self.project = self.directory / "project"
        self.clients = self.directory / "clients"
        for path in (self.user_dir, self.project, self.clients):
            path.mkdir()
        self.config = self.user_dir / ".config/hashprobe/mcp.json"
        self.arguments = self.directory / "arguments"
        self.env = {
            **os.environ,
            "HOME": str(self.user_dir),
            "PATH": f"{self.bin}:{self.clients}",
            "HP_TEST_CLIENT_ARGUMENTS": str(self.arguments),
            "HP_TEST_CLIENT_STATUS": "0",
        }
        if OPENSSL:
            (self.clients / "openssl").symlink_to(OPENSSL)
        for client in ("codex", "claude"):
            stub = self.clients / client
            stub.write_text(
                '#!/bin/sh\nprintf "%s\\000" "$@" > "$HP_TEST_CLIENT_ARGUMENTS"\n'
                'exit "$HP_TEST_CLIENT_STATUS"\n'
            )
            stub.chmod(0o755)

    def invoke(self, *args, cwd=None, expected=0):
        result = subprocess.run(args, cwd=cwd or self.project, env=self.env,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def register(self, client, *args, **kwargs):
        return self.invoke("hashprobe-mcp", "setup", "--client", client, *args, **kwargs)

    def saved_arguments(self):
        return self.arguments.read_bytes().decode().split("\0")[:-1]

    def custom_config(self, path=None):
        path = path or self.config
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"targets": {"native": {
            "command": [str(BUILD / "sha256-target")], "cwd": str(self.project),
        }}}))
        return path

    def test_commands_work_from_another_project(self):
        result = self.invoke("hashprobe", "self-test")
        self.assertIn("4 known answers", result.stdout)
        self.invoke("hashprobe", "check", "--report", "check.json",
                    "--", str(BUILD / "sha256-target"))
        report = json.loads((self.project / "check.json").read_text())
        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["summary"]["complete"])

    @unittest.skipUnless(OPENSSL, "OpenSSL is needed for the default configuration")
    def test_setup_registers_absolute_paths_and_survives_removed_checkout(self):
        checkout = self.directory / "temporary checkout"
        checkout.mkdir()
        self.register("codex", cwd=checkout)
        self.assertEqual(self.saved_arguments(), [
            "mcp", "add", "hashprobe", "--", str(self.bin / "hashprobe-mcp"),
            "--config", str(self.config),
        ])
        self.assertEqual(self.config.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(self.config.read_text())["targets"]["openssl"]["cwd"], ".")
        checkout.rmdir()
        settings = self.invoke("hashprobe-mcp", "client-config")
        connection = json.loads(settings.stdout)["mcpServers"]["hashprobe"]
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "installed-client", "version": "1"},
            }},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                "name": "list_targets", "arguments": {},
            }},
        ]
        process = subprocess.run(
            [connection["command"], *connection["args"]], cwd=self.project,
            env={**self.env, "PATH": ""}, input="".join(json.dumps(r) + "\n" for r in requests),
            capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        responses = {item["id"]: item for item in map(json.loads, process.stdout.splitlines())}
        self.assertEqual({tool["name"] for tool in responses[2]["result"]["tools"]},
                         {"list_targets", "check", "get_failure", "replay"})
        self.assertEqual(responses[3]["result"]["structuredContent"]["targets"][0]["name"], "openssl")

    def test_claude_uses_user_scope_and_preserves_existing_config(self):
        self.custom_config()
        before = self.config.read_bytes()
        self.register("claude")
        self.assertEqual(self.saved_arguments(), [
            "mcp", "add", "--scope", "user", "--transport", "stdio", "hashprobe", "--",
            str(self.bin / "hashprobe-mcp"), "--config", str(self.config),
        ])
        self.assertEqual(self.config.read_bytes(), before)

    def test_custom_paths_are_not_interpreted_by_a_shell(self):
        config = self.custom_config(self.project / "settings ' $(touch bad) `touch bad` ;.json")
        self.register("codex", "--config", str(config))
        self.assertEqual(self.saved_arguments()[-1], str(config))
        self.assertFalse((self.project / "bad").exists())
        self.assertFalse(self.config.exists())

    def test_invalid_config_is_preserved_and_not_registered(self):
        self.config.parent.mkdir(parents=True)
        self.config.write_text('{"targets":{}}')
        before = self.config.read_bytes()
        self.register("codex", expected=2)
        self.assertEqual(self.config.read_bytes(), before)
        self.assertFalse(self.arguments.exists())

    def test_unknown_or_missing_client_does_not_create_config(self):
        result = self.register("unknown", expected=2)
        self.assertIn("unsupported client", result.stderr)
        (self.clients / "codex").unlink()
        result = self.register("codex", expected=2)
        self.assertIn("not on PATH", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertFalse(self.arguments.exists())

    def test_missing_openssl_does_not_create_config(self):
        (self.clients / "openssl").unlink(missing_ok=True)
        result = self.register("codex", expected=2)
        self.assertIn("not executable", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertFalse(self.arguments.exists())

    def test_client_failure_does_not_claim_success(self):
        self.custom_config()
        self.env["HP_TEST_CLIENT_STATUS"] = "1"
        result = self.register("claude", expected=2)
        self.assertIn("registration failed", result.stderr)
        self.assertNotIn("is registered", result.stdout)

    def test_setup_rejects_incompatible_options(self):
        for args in (("setup",), ("--client", "codex"),
                     ("init", "--client", "codex"),
                     ("setup", "--client", "codex", "--name", "other"),
                     ("setup", "--client", "codex", "--", "program")):
            with self.subTest(args=args):
                self.invoke("hashprobe-mcp", *args, expected=2)
        self.assertFalse(self.config.exists())
        self.assertFalse(self.arguments.exists())

    def test_uninstall_removes_only_installed_binaries(self):
        stage = self.directory / "uninstall"
        binaries = stage / "usr/local/bin"
        binaries.mkdir(parents=True)
        for name in ("hashprobe", "hashprobe-mcp", "unrelated"):
            (binaries / name).touch()
        self.custom_config()
        result = subprocess.run(
            ["make", "uninstall", f"DESTDIR={stage}", "PREFIX=/usr/local"],
            cwd=ROOT, capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([path.name for path in binaries.iterdir()], ["unrelated"])
        self.assertTrue(self.config.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
