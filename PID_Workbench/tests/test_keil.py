import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integrations" / "keil"))
import pid_keil as bridge
import configure_keil as installer


def record(kind, data=b"", offset=0):
    raw = bytes([len(data)]) + offset.to_bytes(2, "big") + bytes([kind]) + data
    return ":" + (raw + bytes([-sum(raw) & 255])).hex().upper()


def ihex(image, offset=0):
    return "\n".join([record(4, b"\x08\x00"), record(0, image, offset), record(1)])


IMAGE = struct.pack("<II", 0x20001000, 0x08000009) + b"\x00\xbf" * 4


class HexTests(unittest.TestCase):
    def test_addressed_image_and_erased_gap(self):
        text = "\n".join([record(4, b"\x08\x00"), record(0, IMAGE), record(0, b"END", 32), record(1)])
        self.assertEqual(bridge.hex_image(text, 512 * 1024), IMAGE + b"\xff" * 16 + b"END")

    def test_checksum_eof_and_record_length(self):
        for text in (ihex(IMAGE).replace("F2", "F3"), ihex(IMAGE).rsplit("\n", 1)[0],
                     ihex(IMAGE) + "\n" + record(0, b"x"), ":00000000FF\n" + record(1)):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bridge.hex_image(text, 512 * 1024)

    def test_relocation_ram_and_capacity_rejected(self):
        for text in (ihex(IMAGE, 0x4000),
                     "\n".join([record(4, b"\x20\x00"), record(0, IMAGE), record(1)]),
                     "\n".join([record(4, b"\x08\x00"), record(0, IMAGE), record(4, b"\x08\x08"), record(0, b"x"), record(1)])):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bridge.hex_image(text, 512 * 1024)

    def test_conflicting_overlap_rejected(self):
        text = "\n".join([record(4, b"\x08\x00"), record(0, IMAGE), record(0, b"x"), record(1)])
        with self.assertRaises(ValueError):
            bridge.hex_image(text, 512 * 1024)

    def test_segment_record_and_entry_metadata(self):
        text = "\n".join([record(2, b"\x80\x00"), record(4, b"\x08\x00"), record(0, IMAGE),
                          record(5, b"\x08\x00\x00\x09"), record(1)])
        self.assertEqual(bridge.hex_image(text, 512 * 1024), IMAGE)

    def test_check_is_offline_and_validates_vectors(self):
        with tempfile.TemporaryDirectory(prefix="keil with spaces ") as tmp:
            root = Path(tmp)
            source = root / "target.hex"
            source.write_text(ihex(IMAGE), encoding="ascii")
            with patch.object(bridge, "request", side_effect=AssertionError("API called during --check")), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bridge.main(["--check", "--firmware", str(source)]), 0)
            attempts = list(root.glob("flash_records/keil/*/attempt.json"))
            self.assertEqual(len(attempts), 1)
            self.assertEqual(json.loads(attempts[0].read_text())["status"], "checked")
            source.write_text(ihex(b"\x00" * 16), encoding="ascii")
            with self.assertRaises(Exception), contextlib.redirect_stdout(io.StringIO()):
                bridge.main(["--check", "--firmware", str(source)])

    def test_converter_failure_never_submits(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "app.axf"
            source.write_bytes(b"bad axf")
            converter = root / "fromelf.exe"
            converter.touch()
            with patch.object(bridge.subprocess, "run") as run, patch.object(bridge, "request") as api:
                run.return_value.returncode = 1
                run.return_value.stdout = b"ERROR"
                with self.assertRaises(ValueError):
                    bridge.convert(source, root, "STM32F407VET6", converter)
                api.assert_not_called()


class JobTests(unittest.TestCase):
    def test_progress_metrics_and_legacy_job(self):
        self.assertEqual(bridge.progress_line({'phase':'write','progress':.78}),'write 78%')
        line=bridge.progress_line({'phase':'verify','progress':.94,'work_done':8192,'work_total':8880})
        self.assertEqual(line,'verify 94% bytes=8192/8880 speed=0')
        self.assertEqual(bridge.progress_line({'phase':'verify','progress':.94,'work_done':9000,'work_total':8880}),'verify 94%')
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        image = self.root / "firmware.bin"
        image.write_bytes(IMAGE)
        self.spec = {"action": "flash", "chip": "STM32F407VET6", "target_interface": "swd", "port": "auto",
                     "firmware": str(image), "expected_sha256": hashlib.sha256(IMAGE).hexdigest(),
                     "run": True, "idempotency_key": "same-intent-test-key"}
        self.path = self.root / "attempt.json"
        self.saved = {"request": self.spec, "status": "prepared"}
        bridge.save(self.path, self.saved)
        self.job = {"id": "testjob", "state": "succeeded", "phase": "done", "progress": 1,
                    "spec": {"sha256": self.spec["expected_sha256"]}, "result": {"verified": True, "start_ack": True}}

    def test_no_false_success_for_missing_verify_start_or_hash(self):
        for field in ("state", "verified", "start_ack", "sha256"):
            value = json.loads(json.dumps(self.job))
            if field == "state":
                value[field] = "failed"
            elif field == "sha256":
                value["spec"][field] = "wrong"
            else:
                value["result"][field] = False
            with self.subTest(field=field), self.assertRaises(ValueError):
                bridge.check_success(value, self.spec)

    def test_new_attempt_persists_key_before_submit(self):
        def api(path, spec=None):
            if path == "/v1/jobs":
                persisted = json.loads(self.path.read_text())
                self.assertEqual(persisted["status"], "submitting")
                self.assertEqual(persisted["request"]["idempotency_key"], spec["idempotency_key"])
            return {"ok": True, "data": self.job}
        with patch.object(bridge, "preflight", return_value={}), patch.object(bridge, "request", side_effect=api), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bridge.execute_record(self.path), 0)
        self.assertEqual(json.loads(self.path.read_text())["status"], "succeeded")

    def test_resume_known_job_only_queries_no_post_or_preflight(self):
        bridge.save(self.path, dict(self.saved, job_id="testjob"))
        with patch.object(bridge, "preflight") as pre, patch.object(bridge, "request", return_value={"data": self.job}) as api, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bridge.execute_record(self.path, resume=True), 0)
        pre.assert_not_called()
        api.assert_called_once_with("/v1/jobs/testjob")

    def test_uncertain_post_resumes_exact_request_same_key(self):
        with patch.object(bridge, "preflight", return_value={}), \
             patch.object(bridge, "request", side_effect=OSError("response lost")), self.assertRaises(OSError):
            bridge.execute_record(self.path)
        with patch.object(bridge, "request", return_value={"data": self.job}) as api, contextlib.redirect_stdout(io.StringIO()):
            bridge.execute_record(self.path, resume=True)
        self.assertEqual(api.call_args_list[0].args, ("/v1/jobs", self.spec))

    def test_timeout_keeps_existing_job(self):
        bridge.save(self.path, dict(self.saved, job_id="testjob"))
        with patch.object(bridge, "request", return_value={"data": dict(self.job, state="running")}) as api, \
             patch.object(bridge.time, "monotonic", side_effect=[0, 2]), \
             self.assertRaises(TimeoutError), contextlib.redirect_stdout(io.StringIO()):
            bridge.execute_record(self.path, timeout=1, resume=True)
        self.assertEqual(json.loads(self.path.read_text())["job_id"], "testjob")
        api.assert_called_once_with("/v1/jobs/testjob")

    def test_modified_snapshot_is_not_resumed(self):
        Path(self.spec["firmware"]).write_bytes(IMAGE + b"x")
        with patch.object(bridge, "request") as api, self.assertRaises(ValueError):
            bridge.execute_record(self.path, resume=True)
        api.assert_not_called()


class RetryTests(unittest.TestCase):
    def setUp(self):
        JobTests.setUp(self)
        self.path = self.root / "records" / "original" / "attempt.json"
        self.path.parent.mkdir(parents=True)
        self.saved["status"] = "attention_required"
        bridge.save(self.path, self.saved)
        self.prior = dict(self.job, state="failed", idempotency_key=self.spec["idempotency_key"])

    def test_failed_job_gets_new_key_same_snapshot_and_rescans_usb(self):
        def execute(path, timeout):
            saved = json.loads(path.read_text())
            self.assertEqual(saved["previous_attempt"], str(self.path))
            self.assertEqual(saved["retry_mode"], "new_after_failure")
            self.assertNotEqual(saved["request"]["idempotency_key"], self.spec["idempotency_key"])
            self.assertEqual(saved["request"]["port"], "auto")
            self.assertEqual(Path(saved["request"]["firmware"]).read_bytes(), IMAGE)
            for field in ("chip", "target_interface", "expected_sha256", "run"):
                self.assertEqual(saved["request"][field], self.spec[field])
            return 0
        with patch.object(bridge, "request", return_value={"data": [self.prior]}) as api, \
             patch.object(bridge, "execute_record", side_effect=execute), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bridge.retry_record(self.path), 0)
        api.assert_called_once_with("/v1/jobs")
        self.assertEqual(json.loads(self.path.read_text())["status"], "attention_required")

    def test_active_or_already_succeeded_job_is_resumed_without_new_intent(self):
        for state in ("queued", "running", "succeeded"):
            bridge.save(self.path, self.saved)
            with patch.object(bridge, "request", return_value={"data": [dict(self.prior, state=state)]}) as api, \
                 patch.object(bridge, "execute_record", return_value=0) as execute, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bridge.retry_record(self.path, timeout=45), 0)
            api.assert_called_once_with("/v1/jobs")
            execute.assert_called_once_with(self.path, 45, resume=True)
            self.assertEqual(json.loads(self.path.read_text())["job_id"], "testjob")
            self.assertEqual(json.loads(self.path.read_text())["request"], self.spec)

    def test_missing_submit_response_keeps_key_and_original_snapshot_path(self):
        with patch.object(bridge, "request", return_value={"data": []}), \
             patch.object(bridge, "execute_record", return_value=0) as execute, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bridge.retry_record(self.path), 0)
        new = json.loads(execute.call_args.args[0].read_text())
        self.assertEqual(new["request"]["idempotency_key"], self.spec["idempotency_key"])
        self.assertEqual(new["request"]["firmware"], self.spec["firmware"])
        self.assertEqual(new["retry_mode"], "resolve_original_intent")

    def test_known_job_missing_or_task_lookup_failure_never_submits(self):
        bridge.save(self.path, dict(self.saved, job_id="missing"))
        for response in ({"data": []}, OSError("API offline")):
            with patch.object(bridge, "request", side_effect=[response]), \
                 patch.object(bridge, "execute_record") as execute, contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises((ValueError, OSError)):
                    bridge.retry_record(self.path)
            execute.assert_not_called()

    def test_changed_image_or_successful_record_never_calls_api(self):
        for success in (False, True):
            Path(self.spec["firmware"]).write_bytes(IMAGE if success else IMAGE+b"x")
            bridge.save(self.path, dict(self.saved, status="succeeded" if success else "attention_required"))
            with patch.object(bridge, "request") as api, self.assertRaises(ValueError):
                bridge.retry_record(self.path)
            api.assert_not_called()

    def test_previous_task_wrong_hash_is_rejected(self):
        prior = dict(self.prior, spec={"sha256": "wrong"})
        with patch.object(bridge, "request", return_value={"data": [prior]}), \
             patch.object(bridge, "execute_record") as execute, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(ValueError):bridge.retry_record(self.path)
        execute.assert_not_called()

    def test_explicit_retry_submits_exactly_once_and_verifies_result(self):
        posts = []
        def api(path, spec=None):
            if path == "/v1/jobs" and spec is None:return {"data": [self.prior]}
            if path == "/v1/jobs":
                posts.append(spec)
                self.assertNotEqual(spec["idempotency_key"], self.spec["idempotency_key"])
                self.assertEqual(Path(spec["firmware"]).read_bytes(), IMAGE)
                return {"data": dict(self.job, id="retryjob")}
            self.assertEqual(path, "/v1/jobs/retryjob")
            return {"data": dict(self.job, id="retryjob")}
        with patch.object(bridge, "request", side_effect=api), patch.object(bridge, "preflight", return_value={}), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bridge.retry_record(self.path), 0)
        self.assertEqual(len(posts), 1)

    def test_retry_cli_cannot_also_resume_or_replace_the_image(self):
        for extra in (["--resume",str(self.path)],["--firmware",self.spec["firmware"]],["--check"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                bridge.main(["--retry-record",str(self.path)]+extra)


class ConfigureTests(unittest.TestCase):
    def test_only_utilities_changed_backup_and_repeat_are_safe(self):
        with tempfile.TemporaryDirectory(prefix="keil project ") as tmp:
            root = Path(tmp)
            project = root / "f407.uvprojx"
            exe = root / "python.exe"
            exe.touch()
            converter = root / "fromelf.exe"
            converter.touch()
            launcher = root / "PidKeilDownload.exe"
            launcher.touch()
            utilities = '<Utilities><Flash1><UseTargetDll>1</UseTargetDll><UseExternalTool>0</UseExternalTool><RunIndependent>1</RunIndependent><UpdateFlashBeforeDebugging>1</UpdateFlashBeforeDebugging></Flash1><bUseTDR>1</bUseTDR><Flash3 /></Utilities>'
            prefix = '<?xml version="1.0"?><Project><Targets><Target><TargetName>f407</TargetName><TargetOption><TargetCommonOption><Device>STM32F407VETx</Device><OutputName>f407</OutputName></TargetCommonOption>'
            suffix = '</TargetOption></Target></Targets></Project>\r\n'
            original = (prefix + utilities + suffix).encode()
            project.write_bytes(original)
            with contextlib.redirect_stdout(io.StringIO()):
                installer.configure(project, "f407", "STM32F407VET6", exe, converter, launcher)
            updated = project.read_bytes()
            self.assertTrue(updated.startswith(prefix.encode()) and updated.endswith(suffix.encode()))
            tree = ET.fromstring(updated)
            setting = tree.find("./Targets/Target/TargetOption/Utilities")
            self.assertEqual(setting.findtext("Flash1/UseExternalTool"), "1")
            self.assertEqual(setting.findtext("Flash1/RunIndependent"), "0")
            self.assertIn('"#L"', setting.findtext("Flash3"))
            self.assertTrue(setting.findtext("Flash3").startswith('"' + str(launcher) + '" ('))
            self.assertEqual(next(root.glob("*.bak")).read_bytes(), original)
            with contextlib.redirect_stdout(io.StringIO()):
                installer.configure(project, "f407", "STM32F407VET6", exe, converter, launcher)
            self.assertEqual(project.read_bytes(), updated)
            self.assertEqual(len(list(root.glob("*.bak"))), 1)
            with self.assertRaises(ValueError):
                installer.configure(project, "f407", "STM32F407VGT6", exe, converter, launcher)


if __name__ == "__main__":
    unittest.main()
