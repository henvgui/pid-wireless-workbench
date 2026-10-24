"""Native Windows audit helpers. Fault tests do not issue STM32 erase or RUN."""
from datetime import datetime,timezone
import json,os,secrets,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=Path(__file__).with_name('project_audit_20261024');OUT.mkdir(exist_ok=True)
sys.path[:0]=[str(ROOT/'backend')]
import api_client
from workbench_core import ports
from protocol import flash_stm32 as tool
mode=sys.argv[1];record=dict(mode=mode,started_at=datetime.now(timezone.utc).isoformat())
def get(path):return api_client.request(path,start=False)['data']
def post(path,value):return api_client.request(path,value,start=False)['data']
def collect(link,session,op,timeout=5):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if b'\n' not in link.buffer:
            link.buffer+=link.serial.read(link.serial.in_waiting or 1);continue
        line,link.buffer=link.buffer.split(b'\n',1)
        try:value=json.loads(line)
        except (ValueError,UnicodeError):continue
        if value.get('type')=='serial_stream' and value.get('id')==int(session,16) and value.get('op')==op:return value
    raise TimeoutError('Accepted USB request lost its response')
try:
    api_client.ensure_server();jobs=get('/v1/jobs')
    assert not any(j['state'] in ('queued','running') for j in jobs),'An existing hardware job is active'
    if mode=='inspect':
        record.update(health=api_client.health(),ports=ports(),serial=get('/v1/serial/status'))
        processes=subprocess.run(['powershell.exe','-NoProfile','-Command',"Get-CimInstance Win32_Process -Filter \"Name='PID_Workbench.exe'\" | Select-Object ProcessId,ExecutablePath | ConvertTo-Json -Compress"],capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=10)
        record['gui_processes']=processes.stdout.strip()
        end=time.monotonic()+8
        while True:
            record['device']=get('/v1/device')
            if not record['device']['busy'] or time.monotonic()>end:break
            time.sleep(.1)
    elif mode in ('ui','recovery'):
        env=dict(os.environ,PID_UI_TEST_OFFSCREEN='1')
        result=subprocess.run([str(ROOT/'PID_Workbench.exe'),'--audit-recovery-test' if mode=='recovery' else '--ui-test',str(OUT/mode)],cwd=ROOT,env=env,creationflags=subprocess.CREATE_NO_WINDOW,timeout=90)
        assert result.returncode==0,'GUI validation failed'
    elif mode=='close_gui_residue':
        import ctypes
        from ctypes import wintypes
        processes=subprocess.run(['powershell.exe','-NoProfile','-Command',"Get-CimInstance Win32_Process -Filter \"Name='PID_Workbench.exe'\" | Select-Object ProcessId,ExecutablePath | ConvertTo-Json -Compress"],capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=10)
        entries=json.loads(processes.stdout) if processes.stdout.strip() else []
        if isinstance(entries,dict):entries=[entries]
        kernel=ctypes.windll.kernel32;kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.TerminateProcess.argtypes=(wintypes.HANDLE,wintypes.UINT);kernel.WaitForSingleObject.argtypes=(wintypes.HANDLE,wintypes.DWORD);kernel.CloseHandle.argtypes=(wintypes.HANDLE,)
        stopped=[]
        for entry in entries:
            assert str(entry['ExecutablePath']).lower()==str(ROOT/'PID_Workbench.exe').lower(),'Unrelated Workbench process'
            handle=kernel.OpenProcess(1|0x100000,False,entry['ProcessId']);assert handle
            try:assert kernel.TerminateProcess(handle,0);kernel.WaitForSingleObject(handle,5000)
            finally:kernel.CloseHandle(handle)
            stopped.append(entry['ProcessId'])
        record['stopped']=stopped
    elif mode=='stop_backend':
        import ctypes
        from ctypes import wintypes
        health=api_client.health();assert health
        kernel=ctypes.windll.kernel32;kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.TerminateProcess.argtypes=(wintypes.HANDLE,wintypes.UINT);kernel.WaitForSingleObject.argtypes=(wintypes.HANDLE,wintypes.DWORD);kernel.CloseHandle.argtypes=(wintypes.HANDLE,)
        handle=kernel.OpenProcess(1|0x100000,False,health['pid']);assert handle
        try:assert kernel.TerminateProcess(handle,0);kernel.WaitForSingleObject(handle,5000)
        finally:kernel.CloseHandle(handle)
        record['stopped']=health
    elif mode=='burst':
        assert get('/v1/serial/status').get('closed',True),'Close GUI serial before maintenance'
        session=secrets.token_hex(4);link=tool.Link()
        try:
            link.query(f'!serial open {session} 115200 8 0 1 loop','serial_stream',lambda x:x.get('id')==int(session,16) and x.get('op')==21)
            passed=0
            for _ in range(20):
                link.serial.write((f'!serial read {session} 0\n'*2).encode('ascii'))
                value=collect(link,session,22)
                assert value['ok'] and value['active'];passed+=1
            record.update(pairs=passed,target_isolated=True)
        finally:
            try:link.query(f'!serial close {session}','serial_stream',lambda x:x.get('id')==int(session,16) and x.get('op')==24)
            finally:link.close()
    elif mode=='preflash_failure':
        assert get('/v1/serial/status').get('closed',True),'Close GUI serial before maintenance'
        session=secrets.token_hex(4);original=tool.Link
        class Interrupted(original):
            def fw(self,command,*args,**kwargs):
                if command.startswith('!fw data'):raise RuntimeError('Audit: interrupted before ERASE')
                assert not command.startswith('!fw flash'),'Audit must not erase target'
                return super().fw(command,*args,**kwargs)
        opened=post('/v1/serial/configure',dict(session=session,channel='loopback'))
        try:
            image=next((ROOT/'validation/wireless_upgrade_20261024/optimized_flash').glob('*/firmware.bin'))
            tool.Link=Interrupted;old_args=sys.argv
            sys.argv=['flash_stm32.py',str(image),'--part','STM32F407VET6','--interface','swd','--execute','--journal',str(OUT/'interrupted_transfer.json')]
            try:
                try:tool.main();raise AssertionError('Injection did not interrupt')
                except RuntimeError as error:assert str(error)=='Audit: interrupted before ERASE'
            finally:tool.Link=original;sys.argv=old_args
            journal=json.loads((OUT/'interrupted_transfer.json').read_text(encoding='utf-8'))
            assert journal['staging_released'] and not journal.get('executed')
            raw=b'UART resumed after failed transfer\r\n'
            post('/v1/serial/send',dict(session=session,idempotency_key='after-failure',hex=raw.hex()))
            cursor=0;received=bytearray();end=time.monotonic()+5
            while len(received)<len(raw) and time.monotonic()<end:
                response=get(f'/v1/serial/read?session={session}&cursor={cursor}')
                for chunk in response['chunks']:assert not chunk['gap'];received.extend(bytes.fromhex(chunk['hex']))
                cursor=response['next_cursor'];time.sleep(.02)
            assert received==raw and not response['paused']
            record.update(staging_released=True,uart_resumed=True,erase_dispatched=False,run_dispatched=False,pause_events=response['pause_events'])
        finally:post('/v1/serial/close',dict(session=session))
    else:raise ValueError(mode)
    record['ok']=True
except Exception as error:record.update(ok=False,error=str(error))
record['completed_at']=datetime.now(timezone.utc).isoformat()
(OUT/(mode+'.json')).write_text(json.dumps(record,ensure_ascii=False,indent=2),encoding='utf-8')
