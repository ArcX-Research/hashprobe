"""Exercise the native stdio protocol directly; only Python's standard library is used."""
import asyncio
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("HASHPROBE_TEST_BUILD", str(ROOT / "build"))).resolve()
MODERN = "2026-07-28"


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hashprobe-protocol-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name).resolve()
        # A copied server has no sibling engine, Python package, or schema file.
        self.server = self.directory / "hashprobe-mcp"
        shutil.copy2(BUILD / "hashprobe-mcp", self.server)
        self.config = self.directory / "config.json"
        self.settings = {"targets": {"native": {"command": [str(BUILD / "sha256-target")]}}}
        self.process = None
        await self.start()

    async def start(self, *, pass_fds=()):
        self.config.write_text(json.dumps(self.settings))
        self.process = await asyncio.create_subprocess_exec(
            str(self.server), "--config", str(self.config), cwd=self.directory,
            env={**os.environ, "PATH": ""}, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
            pass_fds=pass_fds,
        )

    async def stop(self):
        if self.process is None:
            return
        process, self.process = self.process, None
        process.stdin.close()
        try:
            _, stderr = await asyncio.wait_for(process.communicate(), 5)
        except BaseException:
            if process.returncode is None:
                process.kill()
                await process.communicate()
            raise
        self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
        self.assertNotIn(b"runtime error:", stderr)
        self.assertNotIn(b"Sanitizer", stderr)

    async def asyncTearDown(self):
        await self.stop()

    async def send(self, packet):
        data = packet if isinstance(packet, bytes) else json.dumps(packet, ensure_ascii=False).encode()
        self.process.stdin.write(data + b"\n")
        await self.process.stdin.drain()

    async def receive(self):
        line = await asyncio.wait_for(self.process.stdout.readline(), 20)
        self.assertTrue(line, "server stopped before replying")
        return json.loads(line)

    def request(self, method, params=None, *, request_id=1, modern=True):
        params = dict(params or {})
        if modern:
            params["_meta"] = {"io.modelcontextprotocol/protocolVersion": MODERN,
                               "io.modelcontextprotocol/clientCapabilities": {}}
        return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}

    async def call(self, method, params=None, **kwargs):
        await self.send(self.request(method, params, **kwargs))
        return await self.receive()

    async def test_standalone_server_with_no_program_search_path(self):
        discovered = (await self.call("server/discover"))["result"]
        self.assertEqual(discovered["supportedVersions"], [MODERN])
        self.assertEqual(discovered["resultType"], "complete")
        self.assertEqual(discovered["ttlMs"], 0)
        self.assertEqual(discovered["cacheScope"], "private")
        listing = (await self.call("tools/list"))["result"]
        self.assertEqual(len(listing["tools"]), 4)
        self.assertEqual(listing["ttlMs"], 0)
        result = (await self.call("tools/call", {"name": "check", "arguments": {"target": "native"}}))["result"]
        self.assertFalse(result["isError"])
        self.assertEqual(result["structuredContent"]["counts"]["passed"], 117)

    async def test_discovery_response_is_flushed_when_input_closes(self):
        for request_id in range(8):
            await self.send(self.request("tools/list", request_id=request_id))
        self.process.stdin.close()
        stdout, stderr = await asyncio.wait_for(self.process.communicate(), 5)
        self.assertEqual(self.process.returncode, 0, stderr.decode(errors="replace"))
        responses = [json.loads(line) for line in stdout.splitlines()]
        self.assertEqual([response["id"] for response in responses], list(range(8)))
        for response in responses:
            self.assertEqual(len(response["result"]["tools"]), 4)

    async def test_shutdown_is_bounded_when_client_stops_reading(self):
        await self.stop()
        read_fd, write_fd = os.pipe()
        process = None
        try:
            # Keep the read end open without consuming output or buffering it in asyncio.
            process = await asyncio.create_subprocess_exec(
                str(self.server), "--config", str(self.config), cwd=self.directory,
                stdin=asyncio.subprocess.PIPE, stdout=write_fd, stderr=asyncio.subprocess.PIPE,
            )
            for request_id in range(32):
                packet = self.request("tools/list", request_id=request_id)
                process.stdin.write(json.dumps(packet).encode() + b"\n")
            await process.stdin.drain()
            process.stdin.close()
            await asyncio.wait_for(process.wait(), 3)
            self.assertEqual(process.returncode, 2)
            self.assertNotIn(b"Sanitizer", await process.stderr.read())
        finally:
            if process is not None and process.returncode is None:
                process.kill()
                await process.wait()
            os.close(read_fd)
            os.close(write_fd)

    async def test_target_cannot_use_client_file_descriptors(self):
        await self.stop()
        with tempfile.TemporaryFile() as private:
            descriptor = fcntl.fcntl(private.fileno(), fcntl.F_DUPFD, 3)
            try:
                self.settings["targets"]["probe"] = {
                    "command": [sys.executable, str(ROOT / "tests/target_fixture.py"),
                                "private-fd", str(descriptor)]}
                await self.start(pass_fds=(descriptor,))
                result = (await self.call("tools/call", {
                    "name": "check", "arguments": {"target": "probe", "random_cases": 0}
                }))["result"]
                self.assertFalse(result["isError"])
                self.assertEqual(result["structuredContent"]["status"], "pass")
                self.assertEqual(result["structuredContent"]["counts"]["passed"], 85)
                os.fstat(descriptor)
            finally:
                await self.stop()
                os.close(descriptor)

    async def test_legacy_versions_and_initialization_order(self):
        for version in ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"):
            with self.subTest(version=version):
                response = await self.call("tools/list", modern=False)
                self.assertIn("error", response)
                response = await self.call("initialize", {
                    "protocolVersion": version, "capabilities": {},
                    "clientInfo": {"name": "raw-test", "version": "1"}}, modern=False)
                self.assertEqual(response["result"]["protocolVersion"], version)
                self.assertNotIn("resultType", response["result"])
                self.assertIn("error", await self.call("tools/list", modern=False))
                await self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
                self.assertEqual((await self.call("ping", modern=False))["result"], {})
                self.assertEqual(len((await self.call("tools/list", modern=False))["result"]["tools"]), 4)
                result = (await self.call("tools/call", {"name": "list_targets"}, modern=False))["result"]
                self.assertEqual(json.loads(result["content"][0]["text"])["targets"][0]["name"], "native")
                await self.stop()
                await self.start()

    async def test_version_and_capability_validation(self):
        request = self.request("server/discover")
        request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01"
        await self.send(request)
        error = (await self.receive())["error"]
        self.assertEqual(error["code"], -32022)
        self.assertEqual(error["data"]["supported"], [MODERN])
        request["params"]["_meta"].pop("io.modelcontextprotocol/clientCapabilities")
        await self.send(request)
        self.assertEqual((await self.receive())["error"]["code"], -32602)
        self.assertIn("result", await self.call("ping"))

    async def test_request_ids_are_exact_and_utf8_is_preserved(self):
        for request_id in (0, -1, 9007199254740993, -9007199254740993, "", "café-🧪"):
            with self.subTest(request_id=request_id):
                response = await self.call("ping", request_id=request_id)
                self.assertEqual(response["id"], request_id)
                self.assertIn("result", response)
        for request_id in (None, True, [], {}, 1.5):
            response = await self.call("ping", request_id=request_id)
            self.assertIsNone(response["id"])
            self.assertEqual(response["error"]["code"], -32600)

    async def test_invalid_json_is_rejected_and_the_next_request_still_works(self):
        invalid = [b'{', b'{"id":1,"id":2}', b'{"n":01}', b'{"n":1.}', b'{"n":-.5}',
                   b'{"n":1e}', b'{"n":NaN}', b'{"n":Infinity}', b'{} {}',
                   b'{"x":"\\u0000"}', b'{"x":"\\ud800"}', b'{"x":"\\udc00"}',
                   b'{"x":"\\u123"}', b'{"x":"\\q"}', b'{"x":"\x00"}',
                   b'{"x":"\xff"}', b'{"x":"\xc0\x80"}', b'{"x":"\xed\xa0\x80"}',
                   b'{"x":"\xf4\x90\x80\x80"}', b'\x0b{}', b'[' * 40 + b']' * 40,
                   b'[' + b'0,' * 65536 + b'0]']
        for data in invalid:
            with self.subTest(data=data):
                await self.send(data)
                self.assertEqual((await self.receive())["error"]["code"], -32700)
                self.assertIn("result", await self.call("ping"))
        for data in (b'[]', b'null', b'true', b'{}', b'{"jsonrpc":"1.0","method":"ping","id":1}'):
            await self.send(data)
            self.assertEqual((await self.receive())["error"]["code"], -32600)

    async def test_message_limit_resynchronizes_at_the_next_line(self):
        await self.send(b' ' * (256 * 1024 + 1))
        self.assertEqual((await self.receive())["error"]["code"], -32600)
        self.assertIn("result", await self.call("ping"))

    async def test_long_unicode_argument_name_keeps_error_reply_valid(self):
        for name in ("x" * 99 + "🧪", "x" * 99 + "é", "🧪" * 100):
            response = await self.call("tools/call", {"name": "list_targets", "arguments": {name: True}})
            self.assertTrue(response["result"]["isError"])
        self.assertIn("result", await self.call("ping"))

    async def test_notifications_never_run_tools_or_produce_responses(self):
        for method, params in (("notifications/unknown", {}),
                               ("tools/call", {"name": "check", "arguments": {"target": "native"}})):
            packet = self.request(method, params)
            packet.pop("id")
            await self.send(packet)
        response = await self.call("ping", request_id="after-notifications")
        self.assertEqual(response["id"], "after-notifications")
        self.assertEqual(list((self.directory / "reports").glob("*.json")), [])

    async def test_invalid_tool_arguments_do_not_start_a_run(self):
        for arguments in ({}, {"target": "native", "random_cases": 1.5},
                          {"target": "native", "random_cases": True},
                          {"target": "native", "random_cases": 1.0},
                          {"target": "native", "seed": -1},
                          {"target": "native", "command": ["/bin/sh"]}):
            with self.subTest(arguments=arguments):
                result = await self.call("tools/call", {"name": "check", "arguments": arguments})
                self.assertTrue(result["result"]["isError"])
        self.assertEqual(list((self.directory / "reports").glob("*.json")), [])

    async def test_protocol_errors_preserve_the_connection(self):
        for modern in (True, False):
            if not modern:
                await self.call("initialize", {
                    "protocolVersion": "2025-11-25", "capabilities": {},
                    "clientInfo": {"name": "preflight-test", "version": "1"},
                }, modern=False)
                await self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            cases = [
                ("unknown-method", {}, -32601),
                ("tools/call", {"name": "unknown-tool"}, -32602),
                ("tools/call", {"name": "check", "arguments": []}, -32602),
                ("tools/call", {"name": "check", "arguments": None}, -32602),
                ("tools/call", {"name": "check", "arguments": "native"}, -32602),
            ]
            for method, params, code in cases:
                with self.subTest(modern=modern, method=method, params=params):
                    response = await self.call(method, params, modern=modern)
                    self.assertEqual(response["error"]["code"], code)
                    self.assertNotIn("result", response)
                    self.assertIn("result", await self.call("ping", modern=modern))
        self.assertEqual(list((self.directory / "reports").glob("*.json")), [])

    async def test_sigterm_stops_the_target_and_its_descendant(self):
        await self.stop()
        marker, escaped = self.directory / "started", self.directory / "escaped"
        script = self.directory / "wait.py"
        script.write_text(
            "import os,pathlib,subprocess,sys,time\n"
            "subprocess.Popen([sys.executable, '-c', "
            "\"import pathlib,sys,time; time.sleep(0.8); pathlib.Path(sys.argv[1]).touch()\", sys.argv[2]])\n"
            "pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))\ntime.sleep(20)\n")
        self.settings["targets"]["waiting"] = {"command": [sys.executable, str(script), str(marker), str(escaped)]}
        await self.start()
        await self.send(self.request("tools/call", {"name": "check", "arguments": {"target": "waiting"}}))
        for _ in range(200):
            if marker.exists() and marker.read_text().isdecimal():
                break
            await asyncio.sleep(0.01)
        self.assertTrue(marker.exists(), "target did not start")
        pid = int(marker.read_text())
        self.process.send_signal(signal.SIGTERM)
        await asyncio.wait_for(self.process.wait(), 5)
        await self.stop()
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        await asyncio.sleep(1)
        self.assertFalse(escaped.exists(), "target descendant survived server shutdown")


if __name__ == "__main__":
    unittest.main(verbosity=2)
