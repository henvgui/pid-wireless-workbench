"""Build and run pure C++ tests without opening serial ports or touching targets."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', help='g++ executable; defaults to PATH')
args = parser.parse_args()
compiler = args.compiler or shutil.which('g++')
if not compiler and Path(r'C:\ming\mingw64\bin\g++.exe').is_file():
    compiler = r'C:\ming\mingw64\bin\g++.exe'
if not compiler:
    parser.error('g++ not found; add it to PATH or use --compiler')
out = root/'build/native-tests'
out.mkdir(parents=True, exist_ok=True)
host = root/'ESP32_S3_RGB_Chase'
slave = root/'ESP32_S3_Slave_Diagnostic'
flash = root/'PID_Shared/PIDFlash'
link = root/'PID_Shared/PIDLink'
cases = []
for test in sorted((host/'tests').glob('*.cpp')):
    cases.append((test.stem, [test, host/'src/rgb_show.cpp', host/'src/step_control.cpp', host/'src/host_ui.cpp', host/'src/host_local_core.cpp'], [host/'include']))
cases.extend([
    ('bulk', [flash/'tests/bulk_test.cpp', flash/'src/bulk_core.cpp', flash/'src/flash_core.cpp'], [flash/'src']),
    ('slave_protocol', [slave/'tests/protocol_test.cpp', slave/'src/diagnostic_protocol.cpp'], [slave/'include']),
    ('pid_link', [link/'tests/core_test.cpp', link/'src/pid_link_core.cpp'], [link/'src']),
    ('flash_core', [flash/'tests/native_test.cpp', flash/'src/flash_core.cpp', flash/'src/stm32_rom.cpp'], [flash/'src']),
    ('swd', [flash/'tests/swd_test.cpp', flash/'src/flash_core.cpp', flash/'src/swd_programmer.cpp'], [flash/'src']),
    ('serial_ring', [flash/'tests/serial_log_test.cpp', flash/'src/flash_core.cpp'], [flash/'src']),
    ('serial_tx', [flash/'tests/serial_tx_test.cpp'], [flash/'src']),
    ('echo', [flash/'tests/echo_test.cpp', flash/'src/echo_target.c', flash/'src/flash_core.cpp'], [flash/'src']),
])
results = []
for name, sources, includes in cases:
    executable = out/(name+'.exe')
    command = [compiler, '-std=c++11', '-Wall', '-Wextra']
    for directory in includes:
        command.extend(['-I', str(directory)])
    build = subprocess.run(command+list(map(str, sources))+['-o', str(executable)], capture_output=True, text=True, timeout=120)
    result = dict(name=name, build=build.returncode, build_output=build.stdout+build.stderr)
    if build.returncode == 0:
        test = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        result.update(exit_code=test.returncode, output=test.stdout+test.stderr)
    results.append(result)
    print(name+': '+('PASS' if not result['build'] and result.get('exit_code') == 0 else 'FAIL'))
(out/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
raise SystemExit(any(result['build'] or result.get('exit_code') for result in results))
