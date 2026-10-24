"""Compile and execute pure host/slave/protocol tests from current sources."""
import json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2];out=Path(__file__).with_name('project_audit_20261024');out.mkdir(exist_ok=True)
compiler=r'C:\ming\mingw64\bin\g++.exe';cases=[]
host=root/'ESP32_S3_RGB_Chase';slave=root/'ESP32_S3_Slave_Diagnostic';flash=root/'PID_Shared/PIDFlash';link=root/'PID_Shared/PIDLink'
for test in (host/'tests').glob('*.cpp'):
    cases.append((test.stem,[test,host/'src/rgb_show.cpp',host/'src/step_control.cpp',host/'src/host_ui.cpp',host/'src/host_local_core.cpp'],[host/'include']))
cases.extend([
 ('slave_protocol',[slave/'tests/protocol_test.cpp',slave/'src/diagnostic_protocol.cpp'],[slave/'include']),
 ('pid_link',[link/'tests/core_test.cpp',link/'src/pid_link_core.cpp'],[link/'src']),
 ('flash_core',[flash/'tests/native_test.cpp',flash/'src/flash_core.cpp',flash/'src/stm32_rom.cpp'],[flash/'src']),
 ('swd',[flash/'tests/swd_test.cpp',flash/'src/flash_core.cpp',flash/'src/swd_programmer.cpp'],[flash/'src']),
 ('serial_ring',[flash/'tests/serial_log_test.cpp',flash/'src/flash_core.cpp'],[flash/'src']),
 ('serial_tx',[flash/'tests/serial_tx_test.cpp'],[flash/'src']),
 ('echo',[flash/'tests/echo_test.cpp',flash/'src/echo_target.c',flash/'src/flash_core.cpp'],[flash/'src'])])
results=[]
for name,sources,includes in cases:
    executable=out/(name+'.exe');args=[compiler,'-std=c++11','-Wall','-Wextra']
    for directory in includes:args.extend(['-I',str(directory)])
    build=subprocess.run(args+list(map(str,sources))+['-o',str(executable)],capture_output=True,text=True)
    result=dict(name=name,build=build.returncode,warnings=build.stderr)
    if build.returncode==0:
        test=subprocess.run([str(executable)],capture_output=True,text=True,timeout=30)
        result.update(exit_code=test.returncode,output=test.stdout+test.stderr)
    results.append(result)
(out/'native_tests.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps([dict(name=r['name'],build=r['build'],exit=r.get('exit_code')) for r in results]))
raise SystemExit(any(r['build'] or r.get('exit_code') for r in results))
