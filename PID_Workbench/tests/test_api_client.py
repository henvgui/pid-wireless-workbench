from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend"))
import api_client as client


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.respond()

    def do_POST(self):
        self.respond()

    def respond(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))) if self.command == "POST" else b""
        if self.path == "/health":
            self.reply(200, {"app": "pid-workbench", "pid": 123})
            return
        self.server.attempts += 1
        if self.headers.get("Authorization") != "Bearer " + self.server.token:
            self.reply(401, {"ok": False, "error": {"code": "unauthorized", "message": "Token required"}})
            return
        if self.server.failure:
            detail=self.server.failure if isinstance(self.server.failure,dict) else {"code":"device_busy","message":"Already running"}
            self.reply(409, {"ok": False, "error": detail})
            return
        if self.command == "POST":
            self.server.writes += 1
            spec = json.loads(body)
            self.reply(202, {"ok": True, "data": spec})
        else:
            self.reply(200, {"ok": True, "data": {"version": "test"}})

    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class ClientPathTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.native = root / "Local" / "PIDWorkbench"
        self.native.mkdir(parents=True)
        self.packaged = root / "Local" / "Packages" / "OpenAI.Codex_test" / "LocalCache" / "Local" / "PIDWorkbench"
        self.packaged.mkdir(parents=True)
        self.token = "b" * 64
        (self.packaged / "api_token.txt").write_text(self.token, encoding="ascii")
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.token = self.token
        self.server.writes = self.server.attempts = 0
        self.server.failure = False
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.close)
        for mock in (patch.object(client, "BASE", "http://127.0.0.1:" + str(self.server.server_port)),
                     patch.object(client, "DATA_DIR", self.native),
                     patch.dict(client.os.environ, {"LOCALAPPDATA": str(root / "Local"), "USERPROFILE": str(root), "PID_WORKBENCH_DATA": ""})):
            mock.start()
            self.addCleanup(mock.stop)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def test_keil_missing_native_file_reuses_packaged_service_token(self):
        with patch.object(client.subprocess, "Popen") as launch:
            self.assertTrue(client.request("/v1/capabilities")["ok"])
            spec = {"action": "flash", "idempotency_key": "same-intent"}
            self.assertEqual(client.request("/v1/jobs", spec)["data"], spec)
        launch.assert_not_called()
        self.assertEqual(self.server.writes, 1)
        self.assertFalse((self.native / "api_token.txt").exists())

    def test_wrong_native_token_only_retries_after_explicit_401(self):
        (self.native / "api_token.txt").write_text("a" * 64)
        self.assertTrue(client.request("/v1/jobs", {"idempotency_key": "unchanged"})["ok"])
        self.assertEqual(self.server.attempts, 2)
        self.assertEqual(self.server.writes, 1)
    def test_gui_service_returns_verified_credential_path_without_token(self):
        import pidctl
        (self.native / 'api_token.txt').write_text('a'*64)
        with patch.object(sys,'argv',['pidctl','service']),patch.object(pidctl,'ensure_server',return_value={'app':'pid-workbench'}):
            result=pidctl.main()
        self.assertEqual(Path(result['data']['credential_path']),self.packaged/'api_token.txt')
        self.assertNotIn(self.token,json.dumps(result))
        self.assertEqual(self.server.writes,0)

    def test_native_service_still_works_without_package_scan_fallback(self):
        (self.native / "api_token.txt").write_text(self.token)
        self.assertTrue(client.request("/v1/capabilities")["ok"])
        self.assertEqual(self.server.attempts, 1)

    def test_explicit_data_directory_is_respected(self):
        with patch.dict(client.os.environ, {"PID_WORKBENCH_DATA": str(self.native)}):
            with self.assertRaises(client.ApiError) as error:
                client.request("/v1/jobs", {"idempotency_key": "must-not-write"})
        self.assertEqual(error.exception.code, "api_token_missing")
        self.assertEqual(self.server.writes, 0)

    def test_missing_or_malformed_credentials_report_structured_error(self):
        (self.packaged / "api_token.txt").write_text("invalid credential")
        with self.assertRaises(client.ApiError) as error:
            client.request("/v1/capabilities")
        self.assertEqual(error.exception.code, "api_token_missing")
        self.assertNotIn("invalid credential", str(error.exception))
        self.assertEqual(self.server.attempts, 0)

    def test_busy_response_does_not_replay_post_with_other_token(self):
        (self.native / "api_token.txt").write_text(self.token)
        self.server.failure = True
        with self.assertRaises(client.ApiError) as error:
            client.request("/v1/jobs", {"idempotency_key": "same"})
        self.assertEqual(error.exception.code, "device_busy")
        self.assertEqual(self.server.attempts, 1)
        self.assertEqual(self.server.writes, 0)

    def test_structured_serial_remote_reason_is_preserved(self):
        (self.native / "api_token.txt").write_text(self.token)
        self.server.failure={"code":"serial_remote_error","message":"Lost session","remote_error":"wrong_transfer"}
        with self.assertRaises(client.ApiError) as error:
            client.request("/v1/serial/read?session=a123bc45&cursor=0")
        self.assertEqual(error.exception.remote_error,"wrong_transfer")
        self.assertEqual(self.server.attempts,1)

    def test_disconnected_post_is_never_automatically_replayed(self):
        (self.native / "api_token.txt").write_text(self.token)
        with patch.object(client.OPENER, "open", side_effect=urllib.error.URLError("response lost")) as call:
            with self.assertRaises(urllib.error.URLError):
                client.request("/v1/jobs", {"idempotency_key": "same"}, start=False)
        call.assert_called_once()

    def test_service_start_detaches_all_keil_standard_handles(self):
        with patch.object(client, "health", side_effect=[None, {"app": "pid-workbench"}]), \
             patch.object(client.time, "sleep"), patch.object(client.subprocess, "Popen") as spawn:
            self.assertEqual(client.ensure_server()["app"], "pid-workbench")
        self.assertEqual(spawn.call_args.kwargs["stdin"], client.subprocess.DEVNULL)
        self.assertTrue(spawn.call_args.kwargs["close_fds"])


if __name__ == "__main__":
    unittest.main()
