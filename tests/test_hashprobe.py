"""Exercise the compiled C CLI, real targets, and saved reports end to end."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
FIXTURE = ROOT / "tests" / "target_fixture.py"
MASK64 = (1 << 64) - 1


def random_inputs(seed, count, maximum):
    state = seed

    def word():
        nonlocal state
        state = (state + 0x9E3779B97F4A7C15) & MASK64
        value = state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
        return value ^ (value >> 31)

    for _ in range(count):
        length = word() % (maximum + 1)
        data = b"".join(word().to_bytes(8, "little") for _ in range((length + 7) // 8))
        yield data[:length]


class HashprobeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="hashprobe-test-")
        self.addCleanup(self.directory.cleanup)
        self.cwd = Path(self.directory.name)
        self.counter = 0

    def path(self, prefix="report", suffix=".json"):
        self.counter += 1
        return self.cwd / f"{prefix}-{self.counter}{suffix}"

    def cli(self, *args, expected=0):
        process = subprocess.run(
            [str(BUILD / "hashprobe"), *map(str, args)],
            cwd=self.cwd, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        self.assertNotIn("runtime error:", process.stderr)
        self.assertNotIn("AddressSanitizer", process.stderr)
        return process

    def target(self, mode, *args):
        return [sys.executable, str(FIXTURE), mode, *map(str, args)]

    def check(self, target=None, options=(), expected=0):
        report = self.path()
        self.cli("check", "--report", report, *options, "--",
                 *(target or [BUILD / "sha256-target"]), expected=expected)
        return report, json.loads(report.read_text())

    def source(self, data=b"a\x00\xffb"):
        report = {
            "schema_version": 1,
            "tool": {"name": "hashprobe", "version": "0.1.0"},
            "algorithm": "sha256",
            "results": [{
                "id": "saved-case", "category": "random", "status": "mismatch",
                "input_bytes": len(data), "input_hex": data.hex(),
                "expected_hex": hashlib.sha256(data).hexdigest(),
            }],
        }
        path = self.path("source")
        path.write_text(json.dumps(report))
        return path, report

    def replay(self, source, target, options=(), expected=0):
        report = self.path("replay")
        self.cli("replay", source, "--report", report, *options, "--", *target, expected=expected)
        return json.loads(report.read_text())

    def test_help_version_and_missing_command(self):
        self.assertIn("Hashprobe", self.cli("--help").stdout)
        self.assertIn("0.1.0", self.cli("--version").stdout)
        self.cli(expected=2)

    def test_reference_known_answers(self):
        self.assertIn("4 known answers", self.cli("self-test").stdout)

    def test_native_suite_and_independent_expected_digests(self):
        _, report = self.check(options=("--target-version", "test-build"))
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["summary"], {
            "planned": 117, "executed": 117, "passed": 117,
            "mismatches": 0, "errors": 0, "skipped": 0, "complete": True,
        })
        self.assertEqual(report["target"]["version_tag"], "test-build")
        self.assertEqual(report["target"]["executable"]["sha256"],
                         hashlib.sha256((BUILD / "sha256-target").read_bytes()).hexdigest())
        random_cases = iter(random_inputs(0, 32, 4096))
        known = {
            "kat-empty": b"", "kat-abc": b"abc",
            "kat-multiblock": b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
            "kat-million-a": b"a" * 1_000_000,
        }
        for result in report["results"]:
            if result["category"] == "known-answer":
                data = known[result["id"]]
            elif result["category"] == "boundary":
                pattern = result["id"].split("-")[-1]
                size = result["input_bytes"]
                data = {"zero": lambda: bytes(size), "ff": lambda: b"\xff" * size,
                        "ramp": lambda: bytes(i % 256 for i in range(size))}[pattern]()
            else:
                data = next(random_cases)
            self.assertEqual(result["input_bytes"], len(data))
            self.assertEqual(result["expected_hex"], hashlib.sha256(data).hexdigest())
            self.assertEqual(result["actual_hex"], result["expected_hex"])
            self.assertNotIn("input_hex", result)
        self.assertEqual(len({r["id"] for r in report["results"]}), 117)

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL executable unavailable")
    def test_openssl_binary_target(self):
        _, report = self.check([shutil.which("openssl"), "dgst", "-sha256", "-binary"],
                               options=("--output", "binary"))
        self.assertEqual(report["summary"]["passed"], 117)

    def test_python_hashlib_target(self):
        _, report = self.check(self.target("correct"))
        self.assertEqual(report["summary"]["passed"], 117)

    def test_c_nul_bug_is_detected_and_replayed(self):
        faulty = [BUILD / "sha256-target", "--demo-bug-nul"]
        source, report = self.check(faulty, expected=1)
        failed = [r for r in report["results"] if r["status"] == "mismatch"]
        self.assertTrue(failed)
        self.assertTrue(report["summary"]["complete"])
        self.assertEqual(failed[0]["id"], "boundary-0001-zero")
        for item in failed:
            data = bytes.fromhex(item["input_hex"])
            self.assertEqual(item["expected_hex"], hashlib.sha256(data).hexdigest())
            self.assertEqual(item["actual_hex"], hashlib.sha256(data.split(b"\0")[0]).hexdigest())
        fixed = self.replay(source, [BUILD / "sha256-target"])
        self.assertEqual(fixed["summary"]["passed"], len(failed))
        self.assertEqual(fixed["suite"]["source_sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
        again = self.replay(source, faulty, options=("--case", failed[0]["id"]), expected=1)
        self.assertEqual(again["summary"]["executed"], 1)
        self.assertEqual(again["results"][0]["input_hex"], failed[0]["input_hex"])

    def test_fail_fast_is_incomplete(self):
        _, report = self.check(self.target("zero"), options=("--fail-fast",), expected=1)
        self.assertFalse(report["summary"]["complete"])
        self.assertEqual(report["summary"]["executed"], 1)
        self.assertEqual(report["summary"]["skipped"], 116)
        self.assertEqual(report["status"], "mismatch")

    def test_adapter_errors_are_distinct_and_bounded(self):
        for mode, kind in (("malformed", "invalid-output"), ("nul-output", "invalid-output"),
                           ("exit", "exit-status"), ("signal", "signal"),
                           ("timeout", "timeout"), ("flood", "output-limit"),
                           ("stderr-flood", "output-limit")):
            with self.subTest(mode=mode):
                _, report = self.check(self.target(mode), options=("--timeout-ms", "300"), expected=2)
                self.assertEqual(report["status"], "error")
                self.assertEqual(report["summary"]["executed"], 1)
                self.assertFalse(report["summary"]["complete"])
                error = report["results"][0]["error"]
                self.assertEqual(error["kind"], kind)
                self.assertLessEqual(len(error["stdout_hex"]), 1024)
                self.assertLessEqual(len(error["stderr_hex"]), 1024)
                if mode == "exit":
                    self.assertEqual(error["exit_status"], 7)
                    self.assertEqual(bytes.fromhex(error["stderr_hex"]), b"diagnostic\0\xff\n")

    def test_missing_executable_is_error(self):
        _, report = self.check([self.cwd / "missing-executable"], expected=2)
        self.assertEqual(report["results"][0]["error"]["kind"], "spawn")

    def test_uppercase_hex_and_binary_whitespace(self):
        source, _ = self.source()
        report = self.replay(source, self.target("uppercase"))
        self.assertEqual(report["summary"]["passed"], 1)
        report = self.replay(source, self.target("binary-whitespace"), options=("--output", "binary"), expected=1)
        self.assertEqual(report["results"][0]["actual_hex"], "20" * 32)
        self.assertNotIn("error", report["results"][0])

    def test_real_binary_digest_with_trailing_whitespace(self):
        for i in range(10000):
            data = i.to_bytes(4, "big")
            if hashlib.sha256(data).digest()[-1] in b" \r\n\t\v\f":
                break
        else:
            self.fail("failed to construct digest with trailing whitespace")
        source, _ = self.source(data)
        report = self.replay(source, self.target("binary"), options=("--output", "binary"))
        self.assertEqual(report["summary"]["passed"], 1)

    def test_argv_is_not_a_shell_command_and_paths_allow_spaces(self):
        spaced = self.cwd / "python with spaces"
        spaced.symlink_to(sys.executable)
        source, _ = self.source()
        literal = "literal; $(touch forbidden) `touch forbidden`"
        self.replay(source, [spaced, FIXTURE, "argument", literal])
        self.assertFalse((self.cwd / "forbidden").exists())

    def test_reports_are_not_overwritten_and_parent_must_exist(self):
        source, _ = self.source()
        before = source.read_bytes()
        marker = self.cwd / "started"
        self.cli("check", "--report", source, "--", *self.target("touch", marker), expected=2)
        self.assertEqual(source.read_bytes(), before)
        self.assertFalse(marker.exists())
        self.cli("check", "--report", self.cwd / "missing" / "report.json", "--",
                 *self.target("touch", marker), expected=2)
        self.assertFalse(marker.exists())
        self.cli("replay", source, "--report", source, "--", BUILD / "sha256-target", expected=2)
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(list(self.cwd.glob("*.tmp.*")), [])

    def test_report_command_is_never_executed(self):
        source, report = self.source()
        marker = self.cwd / "unexpected-command"
        report["target"] = {"command": self.target("touch", marker)}
        source.write_text(json.dumps(report))
        self.replay(source, [BUILD / "sha256-target"])
        self.assertFalse(marker.exists())
        self.cli("replay", source, "--report", self.path(), expected=2)

    def test_replay_requires_a_saved_failure(self):
        source, report = self.source()
        report["results"][0]["status"] = "pass"
        source.write_text(json.dumps(report))
        self.cli("replay", source, "--report", self.path(), "--", BUILD / "sha256-target", expected=2)
        source, _ = self.source()
        self.cli("replay", source, "--report", self.path(), "--case", "missing", "--",
                 BUILD / "sha256-target", expected=2)

    def test_bad_replay_reports_are_rejected_before_target_runs(self):
        source, original = self.source()
        variants = []
        for key, value in (("schema_version", 2), ("algorithm", "sha1"), ("results", {})):
            changed = copy.deepcopy(original)
            changed[key] = value
            variants.append(json.dumps(changed))
        for key, value in (("input_hex", "ff"), ("expected_hex", "00" * 32),
                           ("input_bytes", -1), ("input_bytes", 0.5), ("input_bytes", 1048577),
                           ("status", "unknown"), ("id", "bad\x00id"), ("id", "a" * 80),
                           ("category", [])):
            changed = copy.deepcopy(original)
            changed["results"][0][key] = value
            variants.append(json.dumps(changed))
        changed = copy.deepcopy(original)
        changed["results"] *= 2
        variants.append(json.dumps(changed))
        valid = json.dumps(original)
        variants += [valid + "{}", valid + "\x00x", valid[:-1],
                     valid.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1'),
                     "[" * 40 + "0" + "]" * 40]
        marker = self.cwd / "must-not-start"
        for index, data in enumerate(variants):
            with self.subTest(index=index):
                source.write_text(data)
                output = self.path()
                self.cli("replay", source, "--report", output, "--", *self.target("touch", marker), expected=2)
                self.assertFalse(output.exists())
                self.assertFalse(marker.exists())

    def test_random_suite_is_repeatable_and_seed_changes_inputs(self):
        reports = []
        for seed in (17, 17, 18):
            _, report = self.check(options=("--seed", seed, "--random-cases", 5, "--max-bytes", 512))
            reports.append([r["expected_hex"] for r in report["results"] if r["category"] == "random"])
        self.assertEqual(reports[0], reports[1])
        self.assertNotEqual(reports[0], reports[2])

    def test_invalid_numeric_options(self):
        for option, value in (("--seed", "4294967296"), ("--seed", "-1"), ("--seed", "nan"),
                              ("--timeout-ms", "0"), ("--max-bytes", "65537"),
                              ("--random-cases", "257"), ("--random-cases", "1.5")):
            with self.subTest(option=option, value=value):
                self.cli("check", "--report", self.path(), option, value, "--", BUILD / "sha256-target", expected=2)

    def test_timeout_cleans_up_descendants_holding_pipes(self):
        marker = self.cwd / "escaped-descendant"
        _, report = self.check(self.target("descendant", marker), options=("--timeout-ms", "250"), expected=2)
        self.assertEqual(report["results"][0]["error"]["kind"], "timeout")
        time.sleep(0.9)
        self.assertFalse(marker.exists())

    def test_interrupt_saves_partial_report(self):
        marker = self.cwd / "ready"
        report_path = self.path()
        process = subprocess.Popen(
            [str(BUILD / "hashprobe"), "check", "--report", str(report_path), "--", *self.target("wait", marker)],
            cwd=self.cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists())
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 130, stdout + stderr)
            report = json.loads(report_path.read_text())
            self.assertEqual(report["status"], "interrupted")
            self.assertFalse(report["summary"]["complete"])
            self.assertEqual(report["results"][0]["error"]["kind"], "interrupted")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--build-dir", default=str(BUILD))
    args, remaining = parser.parse_known_args()
    BUILD = Path(args.build_dir).resolve()
    unittest.main(argv=[sys.argv[0], "-v", *remaining])
