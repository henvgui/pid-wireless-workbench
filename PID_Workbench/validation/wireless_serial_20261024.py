"""Native Windows validation; original F407 is programmed without auto-start."""
from datetime import datetime,timezone
import hashlib,json,secrets,subprocess,sys,time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1];OUT=Path(__file__).with_suffix('')
if len(sys.argv)>2:OUT=OUT/sys.argv[2]
OUT.mkdir(parents=True,exist_ok=True)
OUTPUTS=ROOT.parent;STM=Path(r'C:\Users\twice\Desktop\STM32PRO\f407')
sys.path[:0]=[str(ROOT/'backend'),str(OUTPUTS/'PID_Shared/PIDFlash/tools')]
import api_client
from workbench_core import ports
from flash_stm32 import Link
mode=sys.argv[1];record={'mode':mode,'started_at':datetime.now(timezone.utc).isoformat()}
def read(p):return json.loads(p.read_text(encoding='utf-8'))
def run(args,cwd,log,timeout=180,allowed=(0,)):
    flags=subprocess.CREATE_NO_WINDOW
    with log.open('w',encoding='utf-8') as handle:
        result=subprocess.run(args,cwd=cwd,stdout=handle,stderr=subprocess.STDOUT,timeout=timeout,creationflags=flags)
    assert result.returncode in allowed,'Command failed; see '+str(log)
try:
    api_client.ensure_server()
    jobs=api_client.request('/v1/jobs',start=False)['data']
    assert not any(j['state'] in ('queued','running') for j in jobs),'Hardware job active'
    record['ports']=ports()
    if mode=='inspect':
        record['health']=api_client.health()
        record['device']=api_client.request('/v1/device',start=False)['data']
        record['serial']=api_client.request('/v1/serial/status',start=False)['data']
    elif mode=='quiesce':
        status=api_client.request('/v1/serial/status',start=False)['data']
        if status.get('session') and not status.get('closed',True):
            try:record['close']=api_client.request('/v1/serial/close',{'session':status['session']},start=False)['data']
            except Exception as error:
                if 'wrong_transfer' not in str(error):raise
                api_client.request('/v1/serial/configure',dict(status['config'],session=status['session']),start=False)
                record['close']=api_client.request('/v1/serial/close',{'session':status['session']},start=False)['data']
    elif mode in ('baseline_flash','optimized_flash','keil_flash'):
        assert api_client.request('/v1/serial/status',start=False)['data'].get('closed',True),'Close serial capture first'
        axf=STM/'MDK-ARM/f407/f407.axf'
        if mode=='baseline_flash':
            run([r'C:\Keil_v5\UV4\UV4.exe','-b',str(STM/'MDK-ARM/f407.uvprojx'),'-t','f407','-o',str(OUT/'f407_build.log')],STM/'MDK-ARM',OUT/'build_launcher.log',allowed=(0,1))
            assert ' - 0 Error(s)' in (OUT/'f407_build.log').read_text(encoding='utf-8',errors='replace')
        args=[sys.executable,'-u',str(ROOT/'integrations/keil/pid_keil.py'),'--firmware',str(axf),
              '--project',str(STM/'MDK-ARM/f407.uvprojx'),'--chip','STM32F407VET6','--interface','swd',
              '--fromelf',r'C:\Keil_v5\ARM\ARMCC\bin\fromelf.exe','--no-run','--records-dir',str(OUT/mode)]
        if mode=='keil_flash':args=[str(ROOT/'integrations/keil/PidKeilDownload.exe')]+args
        run(args,STM/'MDK-ARM',OUT/(mode+'.log'))
        job_path=max((OUT/mode).glob('*/job.json'),key=lambda p:p.stat().st_mtime)
        job=read(job_path);record['job']=job;record['job_path']=str(job_path)
        assert job['state']=='succeeded' and job['result']['verified'] and not job['spec']['run']
        record['seconds']=(datetime.fromisoformat(job['completed_at'])-datetime.fromisoformat(job['started_at'])).total_seconds()
    elif mode in ('upload_host','upload_slave'):
        assert api_client.request('/v1/serial/status',start=False)['data'].get('closed',True)
        slave=mode=='upload_slave';project=OUTPUTS/('ESP32_S3_Slave_Diagnostic' if slave else 'ESP32_S3_RGB_Chase')
        env='slave_flash_psram' if slave else 'host_flash'
        run([str(Path(sys.executable).with_name('platformio.exe')),'run','-e',env,'-t','upload'],project,OUT/(mode+'.log'))
        record['sha256']=hashlib.sha256((project/'.pio/build'/env/'firmware.bin').read_bytes()).hexdigest()
        if slave:
            record['usb']=read(project/'.pio/build'/env/'usb_upload.json')
            assert record['usb']['ok'] and record['usb']['write_and_hash_verified']
    elif mode=='restore_parameters':
        baseline=read(OUT/'inspect.json')['device']['state'];link=Link()
        try:
            link.query('!pid '+ ' '.join(str(baseline[k]) for k in ('p','i','d')),'state')
            link.query('!step '+str(baseline['step']),'state')
            link.query('!stream '+('on' if baseline['stream'] else 'off'),'state')
            record['state']=link.query('!get','state')
        finally:link.close()
    elif mode=='restore_log':
        baseline=read(OUT/'inspect.json')['serial']
        if baseline.get('session') and not baseline.get('closed',True):
            record['restored']=api_client.request('/v1/serial/configure',dict(baseline['config'],session=baseline['session']),start=False)['data']
    elif mode=='state':
        record['device']=api_client.request('/v1/device',start=False)['data']
        s=record['device']['state'];baseline=read(OUT/'inspect.json')['device']['state']
        assert s['synced'] and not s['run_request'] and 'mode' not in s,'Restored host state mismatch'
        assert all(s[k]==baseline[k] for k in ('p','i','d','step','stream','saved','dirty')),'Host parameters changed'
    elif mode=='restart_backend':
        health=api_client.health();record['before']=health
        if health:
            # The authenticated service was checked idle above. Stop only its own PID.
            import ctypes
            from ctypes import wintypes
            ctypes.windll.kernel32.OpenProcess.restype=wintypes.HANDLE
            ctypes.windll.kernel32.TerminateProcess.argtypes=(wintypes.HANDLE,wintypes.UINT)
            ctypes.windll.kernel32.WaitForSingleObject.argtypes=(wintypes.HANDLE,wintypes.DWORD)
            ctypes.windll.kernel32.CloseHandle.argtypes=(wintypes.HANDLE,)
            handle=ctypes.windll.kernel32.OpenProcess(1|0x100000,False,health['pid'])
            assert handle,'Cannot open the checked backend process'
            try:
                assert ctypes.windll.kernel32.TerminateProcess(handle,0),'Cannot stop checked backend'
                ctypes.windll.kernel32.WaitForSingleObject(handle,5000)
            finally:ctypes.windll.kernel32.CloseHandle(handle)
        record['after']=api_client.ensure_server()
    elif mode=='loopback':
        session=secrets.token_hex(4);cursor=0;total=0
        api_client.request('/v1/serial/configure',dict(session=session,channel='loopback',baud=115200,bits=8,parity='N',stop=1),start=False)
        hashes=[]
        try:
            for index in range(16):
                payload=(bytes(range(256))*4) if index%2==0 else ('无线串口完整性测试 PID '+str(index)+'\r\n').encode()*20
                payload=payload[:1024];key='loop-'+str(index)
                spec=dict(session=session,idempotency_key=key,hex=payload.hex())
                sent=api_client.request('/v1/serial/send',spec,start=False)['data']
                repeated=api_client.request('/v1/serial/send',spec,start=False)['data']
                assert sent==repeated,'HTTP idempotency changed result'
                deadline=time.monotonic()+8;received=bytearray()
                while len(received)<len(payload) and time.monotonic()<deadline:
                    response=api_client.request(f'/v1/serial/read?session={session}&cursor={cursor}',start=False)['data']
                    for chunk in response['chunks']:
                        assert not chunk['gap'],'RX gap'
                        received.extend(bytes.fromhex(chunk['hex']))
                    assert not response['dropped'] and not response['rx_errors'],'UART loss or errors'
                    cursor=response['next_cursor'];time.sleep(.02)
                assert bytes(received)==payload,'Loopback bytes differ'
                total+=len(payload);hashes.append(hashlib.sha256(payload).hexdigest())
            time.sleep(.2)
            tail=api_client.request(f'/v1/serial/read?session={session}&cursor={cursor}',start=False)['data']
            assert not tail['chunks'],'Duplicate UART transmission'
            record.update(bytes=total,bursts=16,sha256=hashes,zero_gaps=True,zero_uart_errors=True,retry_duplicates=0,target_isolated=True)
        finally:api_client.request('/v1/serial/close',dict(session=session),start=False)
    elif mode=='flash_resume':
        session=secrets.token_hex(4);cursor=0
        opened=api_client.request('/v1/serial/configure',dict(session=session,channel='loopback',baud=115200,bits=8,parity='N',stop=1),start=False)['data']
        epoch=opened['status']['epoch']
        image=next((ROOT/'validation/wireless_upgrade_20261024/optimized_flash').glob('*/firmware.bin'))
        assert hashlib.sha256(image.read_bytes()).hexdigest()=='1796653585af5ce29e3ad0b0e31332ba286ec4065fca4081d602552e925dd9a4'
        try:
            # The accepted original target image is unchanged and stays halted.
            job=api_client.request('/v1/jobs',dict(action='flash',chip='STM32F407VET6',target_interface='swd',firmware=str(image),run=False,idempotency_key='serial-resume-'+secrets.token_hex(8)),start=False)['data']
            end=time.monotonic()+90
            while job['state'] in ('queued','running') and time.monotonic()<end:
                time.sleep(.3);job=api_client.request('/v1/jobs/'+job['id'],start=False)['data']
            record['job']=job
            assert job['state']=='succeeded' and job['result']['verified'] and not job['spec']['run']
            raw='烧录完成后串口恢复\r\n'.encode();spec=dict(session=session,idempotency_key='after-flash',hex=raw.hex())
            record['send']=api_client.request('/v1/serial/send',spec,start=False)['data']
            received=bytearray();end=time.monotonic()+5
            while len(received)<len(raw) and time.monotonic()<end:
                response=api_client.request(f'/v1/serial/read?session={session}&cursor={cursor}',start=False)['data']
                for chunk in response['chunks']:
                    assert not chunk['gap'];received.extend(bytes.fromhex(chunk['hex']))
                cursor=response['next_cursor'];time.sleep(.05)
            assert received==raw and response['pause_events']>=1 and not response['paused']
            record.update(epoch_before=epoch,epoch_after=api_client.request('/v1/serial/status',start=False)['data']['epoch'],pause_events=response['pause_events'],resumed=True)
            assert record['epoch_after']==epoch
        finally:api_client.request('/v1/serial/close',dict(session=session),start=False)
    elif mode in ('gui','ui'):
        args=[str(ROOT/'PID_Workbench.exe'),'--wireless-serial-test' if mode=='gui' else '--ui-test',str(OUT/mode)]
        import os
        os.environ['PID_UI_TEST_OFFSCREEN']='1'
        run(args,ROOT,OUT/(mode+'.log'),timeout=90)
    elif mode=='launch':
        from workbench_core import DATA_DIR
        prefs=Path(DATA_DIR)/'serial_ui_settings.json'
        settings=json.loads(prefs.read_text(encoding='utf-8-sig')) if prefs.exists() else {}
        settings.update(channel=0,baud='115200',bits='8',parity=0,stop=0,format=0,encoding=0,sendFormat=0,ending=3)
        prefs.write_text(json.dumps(settings,ensure_ascii=False,indent=2),encoding='utf-8')
        application=subprocess.Popen([str(ROOT/'PID_Workbench.exe'),'--serial-log'],cwd=ROOT)
        record['pid']=application.pid;end=time.monotonic()+15
        while time.monotonic()<end:
            status=api_client.request('/v1/serial/status',start=False)['data']
            if not status.get('closed',True) and status.get('config',{}).get('channel')=='h2':break
            time.sleep(.2)
        assert not status.get('closed',True) and status['config']['channel']=='h2','GUI did not connect H2'
        record['serial']=status
    elif mode=='h2_receive':
        session=secrets.token_hex(4)
        record['config']=api_client.request('/v1/serial/configure',dict(session=session,channel='h2',baud=115200,bits=8,parity='N',stop=1),start=False)['data']
        cursor=0;raw=bytearray();end=time.monotonic()+2
        while time.monotonic()<end:
            result=api_client.request(f'/v1/serial/read?session={session}&cursor={cursor}',start=False)['data']
            for chunk in result['chunks']:raw.extend(bytes.fromhex(chunk['hex']))
            cursor=result['next_cursor'];time.sleep(.1)
        record['received_bytes']=len(raw);record['sample']=raw[:1000].decode('utf-8',errors='replace')
        api_client.request('/v1/serial/close',dict(session=session),start=False)
    else:raise ValueError(mode)
    record['ok']=True
except Exception as error:record.update(ok=False,error=str(error))
record['completed_at']=datetime.now(timezone.utc).isoformat()
(OUT/(mode+'.json')).write_text(json.dumps(record,ensure_ascii=False,indent=2),encoding='utf-8')
