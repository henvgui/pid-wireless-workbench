from pathlib import Path
import subprocess
import tempfile
import unittest


class RecoveryPolicyTests(unittest.TestCase):
    def test_transient_errors_reopen_and_explicit_stop_policy(self):
        root=Path(__file__).resolve().parents[1]
        compiler=Path("C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe")
        if not compiler.exists():self.skipTest("Windows .NET compiler required")
        source='''using System; using PidWorkbench;
class Test {
 static void Check(bool ok){if(!ok)throw new Exception("Recovery policy failed");}
 static void Main(){
  Check(SerialRecovery.Decide("device_busy","")==SerialRecoveryAction.Busy);
  Check(SerialRecovery.Decide("network_unavailable","")==SerialRecoveryAction.RestartService);
  Check(SerialRecovery.Decide("device_or_service_unavailable","")==SerialRecoveryAction.Retry);
  Check(SerialRecovery.Decide("serial_remote_error","stale_request")==SerialRecoveryAction.Reopen);
  Check(SerialRecovery.Decide("serial_remote_error","timeout")==SerialRecoveryAction.Retry);
  Check(SerialRecovery.Decide("serial_remote_error","no_link")==SerialRecoveryAction.Retry);
  Check(SerialRecovery.Decide("serial_remote_error","wrong_transfer")==SerialRecoveryAction.Reopen);
  Check(SerialRecovery.Decide("serial_remote_error","invalid")==SerialRecoveryAction.Reopen);
  Check(SerialRecovery.Decide("serial_session_expired","")==SerialRecoveryAction.Reopen);
  Check(SerialRecovery.Decide("serial_closed","")==SerialRecoveryAction.Stop);
  Check(SerialRecovery.Decide("serial_session_taken","")==SerialRecoveryAction.Stop);
  Check(SerialRecovery.Decide("serial_cursor_conflict","")==SerialRecoveryAction.Stop);
  Check(SerialRecovery.DelayMs(0)==500&&SerialRecovery.DelayMs(1)==1000&&SerialRecovery.DelayMs(99)==5000);
 }
}'''
        with tempfile.TemporaryDirectory() as directory:
            test=Path(directory)/"Test.cs";test.write_text(source,encoding="utf-8")
            exe=Path(directory)/"Test.exe"
            result=subprocess.run([str(compiler),"/nologo","/target:exe","/out:"+str(exe),str(root/"SerialRecovery.cs"),str(test)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stdout.decode(errors="replace"))
            self.assertEqual(subprocess.run([str(exe)],capture_output=True).returncode,0)
