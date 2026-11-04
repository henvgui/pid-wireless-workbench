import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest

LAUNCHER = Path(__file__).resolve().parents[1] / "integrations/keil/PidKeilDownload.exe"


@unittest.skipUnless(os.name == "nt" and LAUNCHER.is_file(), "Build the Windows Keil launcher first")
class LauncherTests(unittest.TestCase):
    def retry_flow(self, has_record, keep_open=0):
        """Click only our own offscreen test window; stub never accesses hardware."""
        import ctypes
        from ctypes import wintypes
        user32=ctypes.WinDLL("user32",use_last_error=True)
        callback_type=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
        user32.EnumWindows.argtypes=[callback_type,wintypes.LPARAM]
        user32.EnumChildWindows.argtypes=[wintypes.HWND,callback_type,wintypes.LPARAM]
        user32.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowTextW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
        user32.IsWindowVisible.argtypes=[wintypes.HWND]
        user32.IsWindowEnabled.argtypes=[wintypes.HWND]
        user32.SendMessageTimeoutW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM,wintypes.UINT,wintypes.UINT,ctypes.POINTER(ctypes.c_size_t)]
        user32.SendMessageTimeoutW.restype=wintypes.LPARAM
        with tempfile.TemporaryDirectory(prefix="keil retry spaces ") as directory:
            root=Path(directory);script=root/'adapter.py'
            script.write_text('''from pathlib import Path
import json,sys
root=Path(__file__).parent;calls=root/'calls.json';record=root/'attempt.json'
history=json.loads(calls.read_text()) if calls.exists() else []
history.append(sys.argv[1:]);calls.write_text(json.dumps(history))
if len(history)==1:
 if HAS_RECORD:
  record.write_text('{}');print('[PID] Record '+str(record),flush=True)
 print('[PID] ERROR: simulated download failure',flush=True)
 sys.exit(9)
assert len(history)==2
if HAS_RECORD:assert sys.argv[1:]==['--retry-record',str(record),'--timeout','17'],sys.argv
else:assert history[0]==history[1]
print('[PID] Programming Done. Verify OK. Application running.',flush=True)
'''.replace('HAS_RECORD',repr(has_record)),encoding='utf-8')
            process=subprocess.Popen([str(LAUNCHER),sys.executable,str(script),'--firmware','ignored.bin','--timeout','17'],
                                     stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW,
                                     env=dict(os.environ,PID_KEIL_NO_PROGRESS="0",PID_KEIL_TEST_OFFSCREEN="1"))
            button=[]
            @callback_type
            def child(hwnd,_):
                text=ctypes.create_unicode_buffer(128);user32.GetWindowTextW(hwnd,text,len(text))
                if text.value=='重新烧录' and user32.IsWindowVisible(hwnd) and user32.IsWindowEnabled(hwnd):button.append(hwnd)
                return True
            @callback_type
            def window(hwnd,_):
                pid=wintypes.DWORD();user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
                if pid.value==process.pid:user32.EnumChildWindows(hwnd,child,0)
                return True
            try:
                deadline=time.monotonic()+8
                while time.monotonic()<deadline and not button and process.poll() is None:
                    user32.EnumWindows(window,0);time.sleep(.05)
                self.assertTrue(button,'Failed download did not show an enabled retry button')
                if keep_open:
                    time.sleep(keep_open)
                    self.assertIsNone(process.poll(),'Failure window closed before the user could retry')
                result=ctypes.c_size_t()
                self.assertTrue(user32.SendMessageTimeoutW(button[0],0x00f5,0,0,2,1500,ctypes.byref(result)))
                # A queued second click must not create another attempt.
                user32.SendMessageTimeoutW(button[0],0x00f5,0,0,2,1500,ctypes.byref(result))
                output=process.communicate(timeout=8)[0]
                self.assertEqual(process.returncode,0,output)
                self.assertIn(b'simulated download failure',output)
                self.assertIn(b'Verify OK',output)
                self.assertEqual(len(json.loads((root/'calls.json').read_text())),2)
            finally:
                if process.poll() is None:process.kill();process.communicate()

    def test_gui_manual_retry_reuses_record_keeps_window_open_and_returns_success(self):
        self.retry_flow(True,keep_open=8.3)

    def test_gui_retry_before_record_exists_repeats_conversion_arguments(self):
        self.retry_flow(False)

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
            self.assertEqual([v['name'] for v in values if v['retry']],['failure'])
            self.assertTrue(all((Path(directory) / (v["name"] + ".png")).stat().st_size > 5000 for v in values))


if __name__ == "__main__":
    unittest.main()
