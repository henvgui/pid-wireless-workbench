import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

LAUNCHER = Path(__file__).resolve().parents[1] / "integrations/keil/PidKeilDownload.exe"


@unittest.skipUnless(os.name == "nt" and LAUNCHER.is_file(), "Build the Windows Keil launcher first")
class LauncherTests(unittest.TestCase):
    def call(self, code, *args):
        return subprocess.run([str(LAUNCHER), sys.executable, "-c", code, *args],
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=15, creationflags=subprocess.CREATE_NO_WINDOW,
                              env=dict(os.environ, PID_KEIL_NO_PROGRESS="1"))

    def test_windows_subsystem_and_no_console_in_python_child(self):
        image = LAUNCHER.read_bytes()
        pe = struct.unpack_from("<I", image, 0x3c)[0]
        self.assertEqual(struct.unpack_from("<H", image, pe + 24 + 68)[0], 2)
        result = self.call("import ctypes,json;print(json.dumps({'console':int(ctypes.windll.kernel32.GetConsoleWindow())}))")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(b'"console": 0', result.stdout)
        self.assertIn(b"finished (exit 0)", result.stdout)

    def test_quoted_paths_empty_argument_and_trailing_slashes(self):
        args = ("", "folder with spaces", 'literal"quote', "C:\\path with spaces\\", "x\\\\\"y", "plain")
        result = self.call("import json,sys;print('ARGS='+json.dumps(sys.argv[1:]))", *args)
        self.assertEqual(result.returncode, 0, result.stdout)
        line = next(s[5:] for s in result.stdout.decode().splitlines() if s.startswith("ARGS="))
        self.assertEqual(json.loads(line), list(args))

    def test_stderr_and_nonzero_exit_are_preserved(self):
        result = self.call("import sys;print('EXPECTED ERROR',file=sys.stderr);sys.exit(7)")
        self.assertEqual(result.returncode, 7)
        self.assertIn(b"EXPECTED ERROR", result.stdout)
        self.assertIn(b"finished (exit 7)", result.stdout)

    def test_both_streams_drained_without_wait_deadlock(self):
        result = self.call("import sys\nfor i in range(300):\n print('O'*400)\n print('E'*400,file=sys.stderr)")
        self.assertEqual(result.returncode, 0, result.stdout[-1000:])
        self.assertEqual(result.stdout.count(b"O" * 400), 300)
        self.assertEqual(result.stdout.count(b"E" * 400), 300)

    def test_progress_is_relayed_before_process_finishes(self):
        process = subprocess.Popen([str(LAUNCHER), sys.executable, "-c",
                                    "import time;print('[PID] write 75%',flush=True);time.sleep(.6);print('END',flush=True)"],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW, env=dict(os.environ, PID_KEIL_NO_PROGRESS="1"))
        try:
            while True:
                line = process.stdout.readline()
                self.assertTrue(line)
                if b"write 75%" in line:
                    break
            self.assertIsNone(process.poll())
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_gui_progress_auto_closes_and_keeps_success_output(self):
        code = "import time;print('[PID] transfer 31%',flush=True);time.sleep(.2);print('[PID] verify 95%',flush=True);time.sleep(.2);print('[PID] done 100%',flush=True);print('[PID] Programming Done. Verify OK. Application running.',flush=True)"
        result = subprocess.run([str(LAUNCHER), sys.executable, "-c", code], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=12,
                                creationflags=subprocess.CREATE_NO_WINDOW,
                                env=dict(os.environ, PID_KEIL_NO_PROGRESS="0", PID_KEIL_TEST_OFFSCREEN="1"))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(b"Verify OK", result.stdout)
        self.assertIn(b"finished (exit 0)", result.stdout)

    def test_rendered_states_real_progress_failure_and_dpi(self):
        with tempfile.TemporaryDirectory(prefix="keil progress ") as directory:
            result = subprocess.run([str(LAUNCHER), "--render-preview", directory], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 0, result.stdout)
            values = json.loads((Path(directory) / "preview.json").read_text(encoding="utf-8-sig"))["states"]
            self.assertEqual([(x["name"], x["percent"]) for x in values],
                             [("prepare", 0), ("transfer", 31), ("write", 78), ("verify", 94), ("success", 100), ("failure", 78)])
            self.assertTrue(all(v["pmv2"] for v in values))
            self.assertTrue(all((Path(directory) / (v["name"] + ".png")).stat().st_size > 5000 for v in values))


if __name__ == "__main__":
    unittest.main()
