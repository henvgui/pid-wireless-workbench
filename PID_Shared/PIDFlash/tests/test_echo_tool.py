import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import echo_stm32 as tool


class TargetLink:
    instances = []
    corrupt_data = corrupt_summary = time_out = False
    marker = b"F103ECHO"
    def __init__(self, port=None):
        self.received = bytearray()
        self.calls = []
        self.closed = False
        self.frames = 0
        self.instances.append(self)
    def query(self, command, kind, accept=lambda _: True, timeout=6):
        self.calls.append(command)
        words = command.split()
        if words[0] != "!echo":
            return {"type": "state", "stream": True, "run_request": False, "ack": True, "synced": True}
        op = {"open": 8, "data": 9, "close": 10}[words[1]]
        offset = int(words[3]) if op != 8 else 0
        result = {"type": "echo", "id": int(words[2], 16), "op": op, "offset": offset,
                  "ok": not self.time_out, "error": "timeout" if self.time_out else "ok", "retries": 0}
        payload = self.marker
        if op == 9:
            assert offset == len(self.received)
            payload = bytes.fromhex(words[4])
            self.received.extend(payload)
            self.frames += 1
            if self.corrupt_data:
                payload = bytes([payload[0] ^ 1]) + payload[1:]
        if op == 10:
            crc = zlib.crc32(self.received) ^ int(self.corrupt_summary)
            payload = struct.pack("<6I", len(self.received), crc, self.frames, 0, 0, 0)
        result["hex"] = payload.hex()
        assert accept(result)
        return result
    def close(self):
        self.closed = True


class ToolTests(unittest.TestCase):
    def run_case(self, fake, expect_error=False, extra_args=()):
        with tempfile.TemporaryDirectory() as tmp:
            journal = Path(tmp) / "test.json"
            args = ["echo_stm32.py", "--bytes", "1031", "--journal", str(journal), *extra_args]
            with patch.object(sys, "argv", args), patch.object(tool, "Link", fake), patch.object(tool.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
                if expect_error:
                    with self.assertRaises(RuntimeError):
                        tool.main()
                else:
                    tool.main()
            record = json.loads(journal.read_text(encoding="utf-8"))
            self.assertEqual(record["passed"], not expect_error)
            instance = fake.instances[-1]
            self.assertTrue(instance.closed)
            if not expect_error:
                self.assertEqual(record["source_sha256"], record["returned_sha256"])
                self.assertEqual(Path(record["sent_file"]).read_bytes(), Path(record["returned_file"]).read_bytes())
                self.assertEqual((record["target_bytes"], record["target_unique_frames"]), (1031, 9))
            return record, instance
    def test_whole_file_roundtrip(self):
        self.run_case(TargetLink)
    def test_f407_marker_and_whole_file_roundtrip(self):
        class F407(TargetLink):
            marker = b"F407ECHO"
        record, _ = self.run_case(F407, extra_args=("--part", "STM32F407VGT6"))
        self.assertEqual(record["target_part"], "STM32F407VGT6")
    def test_f407ve_application_identity(self):
        class F407VE(TargetLink):
            marker = b"F407VE01"
        record, _ = self.run_case(F407VE, extra_args=("--part", "STM32F407VET6"))
        self.assertEqual(record["target_part"], "STM32F407VET6")
    def test_unexpected_target_does_not_report_success(self):
        record, instance = self.run_case(TargetLink, True, ("--part", "STM32F407VGT6"))
        self.assertEqual(record["returned_bytes"], 0)
        self.assertTrue(any(c.startswith("!echo close") for c in instance.calls))
    def test_unknown_uart_marker_rejected(self):
        class Unknown(TargetLink):
            marker = b"BAD!ECHO"
        self.run_case(Unknown, True)
    def test_bad_echo_never_reports_success(self):
        class Corrupt(TargetLink):
            corrupt_data = True
        self.run_case(Corrupt, True)
    def test_bad_target_crc_never_reports_success(self):
        class BadSummary(TargetLink):
            corrupt_summary = True
        self.run_case(BadSummary, True)
    def test_timeout_has_bounded_retries(self):
        class NoTarget(TargetLink):
            time_out = True
        record, instance = self.run_case(NoTarget, True)
        self.assertEqual(sum(c.startswith("!echo open") for c in instance.calls), 3)
        self.assertEqual(record["returned_bytes"], 0)
    def test_generated_text_has_exact_length_and_valid_utf8(self):
        for size in (1, 127, 128, 129, 1031, 131072):
            text = tool.generate_text(size)
            self.assertEqual(len(text), size)
            text.decode("utf-8")
    def test_direct_unicode_text_is_returned_and_printed(self):
        with tempfile.TemporaryDirectory() as tmp:
            journal = Path(tmp) / "unicode.json"
            text = "你好 STM32，PID=1.23\n下一行 🐟"
            args = ["echo_stm32.py", "--text", text, "--journal", str(journal)]
            output = io.StringIO()
            with patch.object(sys, "argv", args), patch.object(tool, "Link", TargetLink), contextlib.redirect_stdout(output):
                tool.main()
            record = json.loads(journal.read_text(encoding="utf-8"))
            self.assertTrue(record["passed"])
            self.assertEqual(Path(record["returned_file"]).read_bytes(), text.encode("utf-8"))
            self.assertIn("STM32F103C8T6 returned:", output.getvalue())


if __name__ == "__main__":
    unittest.main()
