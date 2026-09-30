"""Run real MCP requests through the official client and the native C server."""
import asyncio
import copy
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from mcp import Client
from mcp.client.stdio import StdioServerParameters

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("HASHPROBE_TEST_BUILD", str(ROOT / "build"))).resolve()
SERVER = str(BUILD / "hashprobe-mcp")
FIXTURE = ROOT / "tests" / "target_fixture.py"


class MCPTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hashprobe-mcp-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name).resolve()
        self.config = self.directory / "config.json"
        self.settings = {
            "report_dir": "reports",
            "targets": {
                "native": {"command": [str(BUILD / "sha256-target")], "version": "test-build"},
                "bug": {"command": [str(BUILD / "sha256-target"), "--demo-bug-nul"]},
                "slow": {"command": [sys.executable, str(FIXTURE), "timeout"]},
                "bad-output": {"command": [sys.executable, str(FIXTURE), "malformed"]},
            },
        }

    def client(self, *, mode="auto", settings=None):
        self.config.write_text(json.dumps(settings or self.settings))
        return Client(
            StdioServerParameters(command=SERVER,
                                  args=["--config", str(self.config)],
                                  cwd=self.directory),
            mode=mode, read_timeout_seconds=20,
        )

    async def call(self, client, tool, arguments, *, error=False):
        result = await client.call_tool(tool, arguments)
        self.assertEqual(result.is_error, error, result)
        return result if error else result.structured_content

    def report(self, report_id):
        return self.directory / "reports" / f"{report_id}.json"

    def waiting_target(self):
        marker = self.directory / "pid"
        script = self.directory / "wait.py"
        script.write_text("import os,pathlib,sys,time\npathlib.Path(sys.argv[1]).write_text(str(os.getpid()))\ntime.sleep(20)\n")
        self.settings["targets"]["waiting"] = {"command": [sys.executable, str(script), str(marker)]}
        return marker

    async def wait_for_pid(self, marker):
        for _ in range(200):
            if marker.exists():
                value = marker.read_text()
                if value.isdecimal():
                    return int(value)
            await asyncio.sleep(0.02)
        self.fail("test program did not start")

    async def assert_process_stopped(self, pid):
        for _ in range(200):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            await asyncio.sleep(0.02)
        self.fail("test program was left running")

    async def test_discovery_and_output_schemas(self):
        async with self.client() as client:
            listing = await client.list_tools()
            self.assertEqual({tool.name for tool in listing.tools},
                             {"list_targets", "check", "get_failure", "replay"})
            for tool in listing.tools:
                self.assertIsNotNone(tool.output_schema)
                if tool.name in ("check", "replay"):
                    self.assertFalse(tool.annotations.read_only_hint)
                    self.assertTrue(tool.annotations.destructive_hint)
            targets = await self.call(client, "list_targets", {})
            self.assertEqual({item["name"] for item in targets["targets"]}, set(self.settings["targets"]))
            self.assertNotIn("command", targets["targets"][0])

    async def test_full_run_uses_embedded_engine_from_another_directory(self):
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "native"})
            self.assertEqual(result["status"], "pass")
            self.assertTrue(result["counts"]["complete"])
            self.assertEqual(result["counts"]["passed"], 117)
            self.assertEqual(result["target_version"], "test-build")
            self.assertEqual(result["executable_sha256"], hashlib.sha256((BUILD / "sha256-target").read_bytes()).hexdigest())
            self.assertEqual(self.report(result["report_id"]).stat().st_mode & 0o777, 0o600)

    async def test_legacy_handshake_client(self):
        async with self.client(mode="legacy") as client:
            result = await self.call(client, "check", {"target": "native", "random_cases": 0})
            self.assertEqual(result["counts"]["passed"], 85)

    async def test_target_keeps_its_python_environment(self):
        # Resolving an interpreter symlink must not discard its virtual environment.
        self.settings["targets"]["python"] = {"command": [sys.executable, "-c",
            f"import hashlib, sys; assert sys.prefix == {sys.prefix!r}; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())"]}
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "python", "random_cases": 0})
            self.assertEqual(result["status"], "pass")
            self.assertEqual(result["counts"]["passed"], 85)

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL unavailable")
    async def test_independent_openssl_target(self):
        self.settings["targets"]["openssl"] = {
            "command": [shutil.which("openssl"), "dgst", "-sha256", "-binary"], "output": "binary"}
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "openssl"})
            self.assertEqual(result["counts"]["passed"], 117)

    async def test_find_inspect_and_replay_a_real_bug(self):
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "bug"})
            self.assertEqual(result["status"], "mismatch")
            self.assertGreater(result["failure_count"], 10)
            self.assertEqual(len(result["failures"]), 10)
            self.assertEqual(result["next_failure_index"], 10)
            report_id = result["report_id"]
            first = await self.call(client, "get_failure", {"report_id": report_id})
            self.assertEqual(first["input_hex"], "00")
            self.assertEqual(first["expected_hex"], hashlib.sha256(b"\0").hexdigest())
            self.assertEqual(first["actual_hex"], hashlib.sha256(b"").hexdigest())
            page = await self.call(client, "get_failure", {"report_id": report_id, "index": 2, "limit": 1})
            self.assertEqual(page["input_bytes"], 2)
            self.assertEqual(page["next_offset"], 1)
            second = await self.call(client, "get_failure", {"report_id": report_id, "index": 2, "offset": 1})
            self.assertIsNone(second["next_offset"])
            self.assertEqual(page["input_hex"] + second["input_hex"], "0000")
            fixed = await self.call(client, "replay", {"report_id": report_id, "target": "native"})
            self.assertEqual(fixed["status"], "pass")
            self.assertEqual(fixed["counts"]["passed"], result["failure_count"])
            self.assertNotEqual(fixed["report_id"], report_id)
            again = await self.call(client, "replay", {
                "report_id": report_id, "target": "bug", "case_id": first["case_id"]})
            self.assertEqual(again["counts"]["executed"], 1)
            self.assertEqual(again["status"], "mismatch")

    async def test_fail_fast_preserves_incomplete_status(self):
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "bug", "fail_fast": True})
            self.assertEqual(result["status"], "mismatch")
            self.assertFalse(result["counts"]["complete"])
            self.assertEqual(result["counts"]["mismatches"], 1)

    async def test_schema_errors_and_unknown_targets(self):
        async with self.client() as client:
            missing = await self.call(client, "check", {"target": "missing"}, error=True)
            self.assertIn("call list_targets", missing.content[0].text)
            for arguments in ({"target": "missing"}, {"target": "native", "random_cases": -1},
                              {"target": "native", "random_cases": True}, {"target": "native", "seed": 4294967296},
                              {"target": "native", "timeout_ms": 5001}):
                with self.subTest(arguments=arguments):
                    await self.call(client, "check", arguments, error=True)
            for report_id in ("../README.md", "/etc/passwd", "0" * 32):
                await self.call(client, "get_failure", {"report_id": report_id}, error=True)
            self.assertEqual(list((self.directory / "reports").glob("*.json")), [])

    async def test_target_errors_are_saved_results(self):
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "bad-output"})
            self.assertEqual(result["status"], "error")
            self.assertFalse(result["counts"]["complete"])
            failure = await self.call(client, "get_failure", {"report_id": result["report_id"]})
            self.assertEqual(failure["error"]["kind"], "invalid-output")

    async def test_per_test_timeout(self):
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "slow", "timeout_ms": 100})
            failure = await self.call(client, "get_failure", {"report_id": result["report_id"]})
            self.assertEqual(failure["error"]["kind"], "timeout")

    async def test_whole_run_timeout_saves_partial_report(self):
        self.settings["run_timeout_seconds"] = 1
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "slow"})
            self.assertEqual(result["status"], "error")
            self.assertEqual(result["stopped_reason"], "run_timeout")
            self.assertFalse(result["counts"]["complete"])
            self.assertTrue(self.report(result["report_id"]).exists())

    async def test_replay_uses_configured_program_not_report_command(self):
        marker = self.directory / "not-executed"
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "bug", "fail_fast": True})
            path = self.report(result["report_id"])
            report = json.loads(path.read_text())
            report["target"]["command"] = [sys.executable, str(FIXTURE), "touch", str(marker)]
            path.write_text(json.dumps(report))
            fixed = await self.call(client, "replay", {"report_id": result["report_id"], "target": "native"})
            self.assertEqual(fixed["status"], "pass")
            self.assertFalse(marker.exists())

    async def test_corrupt_report_symlink_and_bad_case_are_rejected(self):
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "bug", "fail_fast": True})
            report_id = result["report_id"]
            await self.call(client, "get_failure", {"report_id": report_id, "index": 999}, error=True)
            await self.call(client, "get_failure", {"report_id": report_id, "offset": 99}, error=True)
            await self.call(client, "get_failure", {"report_id": report_id, "limit": 4097}, error=True)
            await self.call(client, "replay", {"report_id": report_id, "target": "native", "case_id": "missing"}, error=True)
            path = self.report(report_id)
            original = path.read_text()
            for update in ({}, {"tool": None}, {"summary": {}}, {"target": None}, {"status": "pass"}):
                damaged = json.loads(original) if update else {}
                damaged.update(update)
                path.write_text(json.dumps(damaged))
                failure = await self.call(client, "get_failure", {"report_id": report_id}, error=True)
                self.assertIn("report is unavailable or invalid", failure.content[0].text)
            path.unlink()
            outside = self.directory / "outside.json"
            outside.write_text(original)
            path.symlink_to(outside)
            await self.call(client, "get_failure", {"report_id": report_id}, error=True)

    async def test_owner_storage_limits_preserve_existing_reports(self):
        self.settings["max_reports"] = 1
        async with self.client() as client:
            result = await self.call(client, "check", {"target": "native", "random_cases": 0})
            original = self.report(result["report_id"]).read_bytes()
            await self.call(client, "check", {"target": "native"}, error=True)
            self.assertEqual(self.report(result["report_id"]).read_bytes(), original)

    async def test_requests_remain_responsive_and_cancellation_stops_target(self):
        marker = self.waiting_target()
        async with self.client() as client:
            running = asyncio.create_task(client.call_tool("check", {"target": "waiting"}))
            try:
                pid = await self.wait_for_pid(marker)
                await self.call(client, "list_targets", {})
                await self.call(client, "check", {"target": "native"}, error=True)
                running.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await running
                await self.assert_process_stopped(pid)
                for _ in range(100):
                    result = await client.call_tool("check", {"target": "native", "random_cases": 0})
                    if not result.is_error:
                        break
                    await asyncio.sleep(0.02)
                self.assertFalse(result.is_error, result)
            finally:
                if not running.done():
                    running.cancel()
                    await asyncio.gather(running, return_exceptions=True)

    async def test_report_directory_lock_and_disk_budget(self):
        self.settings["max_storage_mb"] = 40
        async with self.client() as client:
            # Discovery waits until the native server has created its store.
            await self.call(client, "list_targets", {})
            with (self.directory / "reports" / ".lock").open("a") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = await self.call(client, "check", {"target": "native"}, error=True)
                self.assertIn("another test run", result.content[0].text)
            self.report("0" * 32).write_text("{}")
            result = await self.call(client, "check", {"target": "native"}, error=True)
            self.assertIn("storage limit", result.content[0].text)

    async def test_disconnect_stops_target_without_a_cancellation_message(self):
        marker = self.waiting_target()
        self.config.write_text(json.dumps(self.settings))
        process = await asyncio.create_subprocess_exec(
            SERVER, "--config", str(self.config), cwd=self.directory,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        try:
            initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "disconnect-test", "version": "1.0"}}}
            process.stdin.write((json.dumps(initialize) + "\n").encode())
            await process.stdin.drain()
            response = json.loads(await asyncio.wait_for(process.stdout.readline(), 5))
            self.assertIn("result", response)
            for request in ({"jsonrpc": "2.0", "method": "notifications/initialized"},
                            {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
                                "name": "check", "arguments": {"target": "waiting"}}}):
                process.stdin.write((json.dumps(request) + "\n").encode())
            await process.stdin.drain()
            pid = await self.wait_for_pid(marker)
            process.stdin.close()
            _, stderr = await asyncio.wait_for(process.communicate(), 5)
            self.assertEqual(process.returncode, 0, stderr.decode())
            await self.assert_process_stopped(pid)
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hashprobe-mcp-config-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name).resolve()
        self.config = self.directory / "config.json"

    def test_relative_paths_and_strict_configuration(self):
        program = self.directory / "program"
        program.symlink_to(BUILD / "sha256-target")
        data = {"targets": {"test": {"command": ["./program"]}}}
        self.config.write_text(json.dumps(data))
        loaded = subprocess.run([SERVER, "client-config", "--config", str(self.config)], capture_output=True)
        self.assertEqual(loaded.returncode, 0, loaded.stderr)
        for update in ({"timeout_ms": True}, {"max_reports": 0}, {"unknown_setting": 1}):
            changed = copy.deepcopy(data)
            changed.update(update)
            self.config.write_text(json.dumps(changed))
            invalid = subprocess.run([SERVER, "client-config", "--config", str(self.config)], capture_output=True)
            self.assertEqual(invalid.returncode, 2, invalid.stdout)
        self.config.write_text('{"targets":{},"targets":{}}')
        invalid = subprocess.run([SERVER, "client-config", "--config", str(self.config)], capture_output=True)
        self.assertEqual(invalid.returncode, 2, invalid.stdout)

    def test_init_prints_usable_client_settings_and_preserves_config(self):
        process = subprocess.run(
            [SERVER, "init", "--config", str(self.config),
             "--name", "native", "--", str(BUILD / "sha256-target")],
            capture_output=True, text=True, cwd=self.directory,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        connection = json.loads(process.stdout)["mcpServers"]["hashprobe"]
        self.assertEqual(connection["command"], SERVER)
        self.assertEqual(connection["args"], ["--config", str(self.config)])
        self.assertEqual(self.config.stat().st_mode & 0o777, 0o600)
        printed = subprocess.run([SERVER, "--config", str(self.config),
                                  "client-config"], capture_output=True, text=True)
        self.assertEqual(printed.returncode, 0, printed.stderr)
        self.assertEqual(json.loads(printed.stdout), json.loads(process.stdout))
        before = self.config.read_bytes()
        again = subprocess.run([SERVER, "init", "--config", str(self.config),
                                "--", str(BUILD / "sha256-target")], capture_output=True)
        self.assertEqual(again.returncode, 2)
        self.assertEqual(self.config.read_bytes(), before)

    def test_init_rejects_missing_program_without_saving_config(self):
        process = subprocess.run(
            [SERVER, "--config", str(self.config), "init", "--", "./missing"],
            capture_output=True, text=True, cwd=self.directory,
        )
        self.assertEqual(process.returncode, 2)
        self.assertIn("target program is not executable", process.stderr)
        self.assertFalse(self.config.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
