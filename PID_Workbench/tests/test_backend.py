import hashlib
from http.server import HTTPServer
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"backend"))
import workbench_core as core
import rom_uart
from server import ApiServer
import pidctl


def image(size=1031):
    data = bytearray((i*37)&255 for i in range(size))
    struct.pack_into("<II", data, 0, 0x20005000, 0x08000009)
    return bytes(data)


class FakeSerial:
    def __init__(self, f103=True, **kwargs):
        self.mode = "sync"; self.rx = bytearray(); self.flash = bytearray(b"\xaa"*131072)
        self.kb = 128; self.device_id = 0x410 if f103 else 0x468; self.erases = []; self.writes = 0; self.go = False
        self.capacity_address=0x1FFFF7E0 if f103 else 0x1FFF75E0;self.erase_command=0x43 if f103 else 0x44
        self.page_size=1024 if f103 else 2048;self.alignment=4 if f103 else 8;self.write_sizes=[]
        self.closed = False; self.nack_write = False; self.corrupt = False
    def open(self): pass
    def close(self): self.closed = True
    def reset_input_buffer(self): self.rx.clear()
    def read(self, n):
        data = bytes(self.rx[:n]); del self.rx[:n]; return data
    def ack(self): self.rx.extend(b"\x79")
    def write(self, data):
        data = bytes(data)
        if self.mode == "sync":
            assert data == b"\x7f"; self.ack(); self.mode = "command"
        elif self.mode == "command":
            assert len(data) == 2 and data[0]^data[1] == 255
            self.cmd = data[0]; self.ack()
            if self.cmd == 0:
                commands = bytes([0, 2, 0x11, 0x21, 0x31, self.erase_command])
                self.rx.extend(bytes([len(commands), 0x22])+commands);self.ack()
            elif self.cmd == 2:
                self.rx.extend(b"\x01"+self.device_id.to_bytes(2,"big"));self.ack()
            elif self.cmd == self.erase_command: self.mode = "erase"
            else:
                assert self.cmd in (0x11,0x31,0x21);self.mode="address"
        elif self.mode == "address":
            assert len(data)==5 and not data[0]^data[1]^data[2]^data[3]^data[4]
            self.address=int.from_bytes(data[:4],"big");self.ack()
            if self.cmd==0x21: self.go=True;self.mode="command"
            else: self.mode="read_count" if self.cmd==0x11 else "write"
        elif self.mode=="read_count":
            assert data[0]^data[1]==255;self.ack();n=data[0]+1
            if self.address==self.capacity_address:self.rx.extend(self.kb.to_bytes(2,"little"))
            else:
                offset=self.address-0x08000000;block=bytearray(self.flash[offset:offset+n])
                if self.corrupt:block[0]^=1
                self.rx.extend(block)
            self.mode="command"
        elif self.mode=="erase":
            width=1 if self.erase_command==0x43 else 2
            pages=int.from_bytes(data[:width],"big")+1
            assert len(data)==width*(pages+1)+1
            check=0
            for b in data:check^=b
            assert check==0
            for index in range(pages):
                page=int.from_bytes(data[width*(index+1):width*(index+2)],"big")
                assert page<64;self.erases.append(page);self.flash[page*self.page_size:(page+1)*self.page_size]=b"\xff"*self.page_size
            self.ack();self.mode="command"
        elif self.mode=="write":
            count=data[0]+1;assert count%self.alignment==0 and len(data)==count+2;self.write_sizes.append(count)
            check=0
            for b in data:check^=b
            assert check==0
            offset=self.address-0x08000000;assert self.flash[offset:offset+count]==b"\xff"*count
            self.flash[offset:offset+count]=data[1:-1];self.writes+=1
            if self.nack_write:self.rx.extend(b"\x1f")
            else:self.ack()
            self.mode="command"
        return len(data)


class DirectUartTests(unittest.TestCase):
    def execute(self, serial, run=True):
        return rom_uart.program(image(), core.CHIPS[0], "COM99", run, lambda *args:None, lambda **kwargs:serial)
    def test_f103_128_report_still_erases_only_image_pages_and_verifies_then_go(self):
        serial=FakeSerial();result=self.execute(serial)
        self.assertTrue(result["verified"] and result["go_ack"] and serial.closed and serial.go)
        self.assertEqual(serial.erases,[0,1]);self.assertEqual(serial.flash[:1031],image())
        self.assertEqual(serial.flash[65536:],b"\xaa"*65536)
    def test_wrong_family_stops_before_erase(self):
        serial=FakeSerial();serial.device_id=0x468
        with self.assertRaises(rom_uart.RomError):self.execute(serial)
        self.assertEqual(serial.erases,[]);self.assertTrue(serial.closed)
    def test_wrong_capacity_stops_before_erase(self):
        serial=FakeSerial();serial.kb=32
        with self.assertRaises(rom_uart.RomError):self.execute(serial)
        self.assertEqual(serial.erases,[])
    def test_nack_write_not_replayed(self):
        serial=FakeSerial();serial.nack_write=True
        with self.assertRaises(rom_uart.RomError):self.execute(serial)
        self.assertEqual(serial.writes,1);self.assertFalse(serial.go)
    def test_corrupt_readback_prevents_go(self):
        serial=FakeSerial();serial.corrupt=True
        with self.assertRaises(rom_uart.RomError):self.execute(serial)
        self.assertFalse(serial.go)
    def test_g431_extended_erase_2k_page_and_eight_byte_alignment(self):
        serial=FakeSerial(f103=False)
        result=rom_uart.program(image(1027),core.CHIPS[1],"COM99",True,lambda *args:None,lambda **kwargs:serial)
        self.assertTrue(result["verified"] and result["go_ack"])
        self.assertEqual(serial.erases,[0]);self.assertEqual(serial.write_sizes[-1],8)


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.binary=self.root/"app.bin";self.binary.write_bytes(image())
        self.spec={"action":"flash","chip":"STM32F103C8T6","method":"wireless_uart","firmware":str(self.binary),"port":"auto","run":True,"idempotency_key":"build-1"}
    def tearDown(self):self.temp.cleanup()
    def test_vector_and_expected_sha_validation(self):
        plan,data=core.prepare(self.spec);self.assertEqual(plan["sha256"],hashlib.sha256(image()).hexdigest())
        with self.assertRaises(core.ApiError):core.prepare(dict(self.spec,expected_sha256="0"*64))
        self.binary.write_bytes(b"\0"*40)
        with self.assertRaises(core.ApiError):core.prepare(self.spec)
    def test_unknown_chip_method_and_fields_rejected(self):
        for changes in ({"chip":"GD32F103"},{"method":"unimplemented"},{"shell":"erase all"},{"run":"true"}):
            with self.assertRaises(core.ApiError):core.prepare(dict(self.spec,**changes))
    def test_f407ve_swd_and_512k_limit(self):
        self.binary.write_bytes(image(1756))
        plan,_=core.prepare(dict(self.spec,chip="STM32F407VET6",method="wireless_swd"))
        self.assertEqual((plan['flash_kb'],plan['target_interface']),(512,'swd'))
        self.binary.write_bytes(image(524289))
        with self.assertRaises(core.ApiError) as error:core.prepare(dict(self.spec,chip="STM32F407VET6",method="wireless_swd"))
        self.assertEqual(error.exception.code,'image_too_large')
    def test_only_host_usb_is_used_for_every_action(self):
        with patch.object(core,"ports",return_value=[{"port":"COM14","role":"host"},{"port":"COM18","role":"slave"},{"port":"COM7","role":"other"}]):
            for spec in (self.spec,{"action":"echo","text":"test"},{"action":"stop"}):
                self.assertEqual(core.prepare(dict(spec,port="auto"))[0]["port"],"COM14")
                for wrong in ("COM18","COM7"):
                    with self.assertRaises(core.ApiError) as error:core.prepare(dict(spec,port=wrong))
                    self.assertEqual(error.exception.code,"wrong_port_role")
    def test_catalog_and_explicit_route_match_legacy_uart_method(self):
        caps=core.capabilities();self.assertEqual(caps["transport"],"esp_now")
        self.assertEqual({m["id"] for m in caps["methods"]},{"wireless_uart","wireless_swd"})
        self.assertTrue(all(m["transport"]=="esp_now" for m in caps["methods"]))
        self.assertEqual([m["target_interface"] for m in caps["methods"] if m["available"]],["uart","swd"])
        explicit=dict(self.spec,transport="esp_now",target_interface="uart");explicit.pop("method")
        self.assertEqual(core.prepare(explicit)[0],core.prepare(self.spec)[0])
        for change,code in (({"method":"stlink_swd"},"unsupported_method"),({"method":"serial_rom"},"unsupported_method"),
                            ({"transport":"usb"},"unsupported_transport"),({"target_interface":"spi"},"unsupported_interface"),
                            ({"target_interface":"swd"},"interface_conflict")):
            with self.assertRaises(core.ApiError) as error:core.prepare(dict(self.spec,**change))
            self.assertEqual(error.exception.code,code)
    def test_unsupported_swd_chip_rejected_before_task_or_hardware_access(self):
        engine=core.Engine(self.root/"store")
        requests=[dict(self.spec,method="wireless_swd"),dict(self.spec,target_interface="swd")]
        requests[1].pop("method")
        with patch.object(engine,"worker") as worker,patch.object(core.subprocess,"Popen") as child:
            for spec in requests:
                for operation in (core.prepare,engine.submit):
                    with self.assertRaises(core.ApiError) as error:operation(spec)
                    self.assertEqual(error.exception.code,"unsupported_chip_interface");self.assertEqual(error.exception.status,400)
            worker.assert_not_called();child.assert_not_called()
        self.assertEqual(engine.store.list(),[]);self.assertFalse((engine.store.root/"jobs").exists())
    def test_f407_default_swd_full_capacity_and_main_sram_validation(self):
        data=bytearray(image(1024*1024));struct.pack_into('<I',data,0,0x20020000);self.binary.write_bytes(data)
        spec=dict(self.spec,chip='STM32F407VGT6');spec.pop('method')
        plan,_=core.prepare(spec);self.assertEqual(plan['target_interface'],'swd');self.assertEqual(plan['flash_kb'],1024)
        self.assertEqual(core.prepare(dict(spec,target_interface='uart'))[0]['method'],'wireless_uart')
        for stack in (0x20020008,0x10010000):
            struct.pack_into('<I',data,0,stack);self.binary.write_bytes(data)
            with self.assertRaises(core.ApiError):core.prepare(spec)
    def test_f407_swd_worker_uses_radio_tool_and_requires_start_ack(self):
        for accepted in (False,True):
            engine=core.Engine(self.root/('accepted' if accepted else 'rejected'))
            spec=dict(self.spec,chip='STM32F407VGT6',method='wireless_swd');plan,_=core.prepare(spec)
            directory=engine.store.root;identifier='4'*24
            (directory/'firmware.bin').write_bytes(image())
            job={'id':identifier,'idempotency_key':'swd','fingerprint':'x','state':'queued','directory':str(directory),'spec':plan,'progress':0}
            engine.store.insert(job);engine.gate.acquire()
            def child(identifier,args):
                self.assertEqual(args[args.index('--interface')+1],'swd')
                self.assertIn('flash_stm32.py',args[1]);self.assertIn('--execute',args)
                (directory/'transfer.json').write_text(json.dumps({'verified':True,'start_ack':accepted,'go_ack':True,'target_interface':'swd'}))
                return 0
            with patch.object(engine,'child',child):engine.worker(identifier)
            self.assertEqual(engine.store.get(identifier)['state'],'succeeded' if accepted else 'failed')
    def test_serial_status_cli_and_missing_session(self):
        with patch.object(sys,"argv",["pidctl","serial","status"]),patch.object(pidctl,"request",return_value={"ok":True}) as api:
            pidctl.main();api.assert_called_once_with("/v1/serial/status")
        for operation in ("configure","read","close"):
            with patch.object(sys,"argv",["pidctl","serial",operation]),patch.object(pidctl,"request") as api:
                with self.assertRaises(core.ApiError):pidctl.main()
                api.assert_not_called()

    def test_cli_uses_slave_interface_and_fixed_transport(self):
        arguments=["pidctl","plan","--chip","STM32F103C8T6","--firmware",str(self.binary)]
        for option in ([],["--interface","uart"],["--method","wireless_uart"]):
            with patch.object(sys,"argv",arguments+option),patch.object(pidctl,"request",return_value={"ok":True}) as api:
                pidctl.main();spec=api.call_args[0][1];self.assertEqual(spec["transport"],"esp_now")
                self.assertEqual(core.prepare(spec)[0]["target_interface"],"uart")
        for method in ("serial_rom","stlink_swd"):
            with patch.object(sys,"argv",arguments+["--method",method]),patch.object(pidctl,"request") as api:
                with self.assertRaises(core.ApiError):pidctl.main()
                api.assert_not_called()
    def test_duplicate_key_persisted_and_snapshot_isolated(self):
        engine=core.Engine(self.root/"store");started=threading.Event();finish=threading.Event();calls=[]
        def worker(identifier):
            calls.append(identifier);started.set();finish.wait(3);engine.store.update(identifier,state="succeeded",result={"verified":True});engine.gate.release()
        with patch.object(engine,"worker",worker):
            first=engine.submit(self.spec);self.assertTrue(started.wait(1))
            self.assertEqual(engine.submit(self.spec)["id"],first["id"])
            with patch.object(core,"ports",return_value=[{"port":"COM99","role":"host"}]):
                self.assertEqual(engine.submit(self.spec)["id"],first["id"])
            with self.assertRaises(core.ApiError):engine.submit(dict(self.spec,idempotency_key="build-2"))
            changed=bytearray(image());changed[-1]^=1;self.binary.write_bytes(changed)
            self.assertEqual((Path(first["directory"])/"firmware.bin").read_bytes(),image())
            with self.assertRaises(core.ApiError):engine.submit(self.spec)
            self.binary.write_bytes(image());finish.set()
            deadline=time.monotonic()+2
            while engine.store.get(first["id"])["state"]!="succeeded" and time.monotonic()<deadline:time.sleep(.01)
        restarted=core.Engine(self.root/"store")
        self.assertEqual(restarted.submit(self.spec)["id"],first["id"]);self.assertEqual(len(calls),1)
    def test_interrupted_job_is_marked_unknown_and_not_replayed(self):
        engine=core.Engine(self.root/"store");job={"id":"1"*24,"idempotency_key":"same","fingerprint":"x","state":"running","directory":str(self.root),"spec":{},"result":None}
        engine.store.insert(job);restarted=core.Engine(self.root/"store")
        self.assertEqual(restarted.store.get(job["id"])["state"],"interrupted")
    def test_old_wireless_uart_job_key_remains_valid(self):
        engine=core.Engine(self.root/"store");plan,_=core.prepare(self.spec)
        legacy=dict(plan,probe_serial="");legacy.pop("transport");legacy.pop("target_interface")
        job={"id":"2"*24,"idempotency_key":"build-1","fingerprint":"old-format","state":"succeeded","directory":str(self.root),"spec":legacy,"result":{"verified":True}}
        engine.store.insert(job)
        with patch.object(engine,"worker") as worker:
            self.assertEqual(engine.submit(self.spec)["id"],job["id"]);worker.assert_not_called()
    def test_execution_guard_never_falls_back_to_a_pc_adapter(self):
        for method in ("wireless_swd","serial_rom","stlink_swd"):
            with self.subTest(method=method):
                engine=core.Engine(self.root/method);directory=engine.store.root;identifier="3"*24
                job={"id":identifier,"idempotency_key":"guard","fingerprint":"x","state":"queued","directory":str(directory),"spec":dict(self.spec,method=method),"progress":0}
                engine.store.insert(job);engine.gate.acquire()
                with patch.object(engine,"child") as child,patch.object(core,"Link") as link:
                    engine.worker(identifier);child.assert_not_called();link.assert_not_called()
                self.assertEqual(engine.store.get(identifier)["state"],"failed");self.assertFalse(engine.gate.locked())


class ProgressMetricsTests(unittest.TestCase):
    def test_phase_bytes_are_persisted_and_rate_baseline_resets_only_on_phase_change(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);engine=core.Engine(root);identifier='5'*24
            engine.store.insert({'id':identifier,'idempotency_key':'metrics','fingerprint':'metrics','state':'running',
                                 'phase':'validate','progress':.02,'directory':str(root),'spec':{}})
            engine.emit(identifier,'transfer',.4,'Received 1024/2048',1024,2048)
            first=engine.store.get(identifier)
            engine.emit(identifier,'transfer',.6,'Received 2048/2048',2048,2048)
            next=engine.store.get(identifier)
            self.assertEqual((next['work_done'],next['work_total'],next['phase_first_done']),(2048,2048,1024))
            self.assertEqual(next['phase_started_at'],first['phase_started_at'])
            engine.emit(identifier,'verify',.9,'Stage=4 bytes=512/2048',512,2048)
            last=engine.store.get(identifier)
            self.assertEqual((last['phase_first_done'],last['work_done'],last['work_total']),(512,512,2048))
            self.assertEqual(last['progress'],.9)


class HttpTests(unittest.TestCase):
    def test_auth_origin_and_schema(self):
        with tempfile.TemporaryDirectory() as tmp:
            engine=core.Engine(tmp);server=ApiServer(("127.0.0.1",0),engine,"a"*64)
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            base="http://127.0.0.1:"+str(server.server_address[1])
            opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
            def call(path,headers=None):return opener.open(urllib.request.Request(base+path,headers=headers or {}),timeout=2)
            try:
                with call("/health") as response:self.assertEqual(json.load(response)["app"],"pid-workbench")
                with self.assertRaises(urllib.error.HTTPError) as error:call("/v1/capabilities")
                self.assertEqual(error.exception.code,401)
                with self.assertRaises(urllib.error.HTTPError) as error:call("/v1/ports",{"Authorization":"Bearer "+"a"*64,"Origin":"https://example.com"})
                self.assertEqual(error.exception.code,403)
                auth={"Authorization":"Bearer "+"a"*64,"Content-Type":"application/json"}
                with call("/v1/openapi.json",auth) as response:
                    schema=json.load(response)["data"];self.assertIn("/v1/jobs",schema["paths"])
                    properties=schema["paths"]["/v1/jobs"]["post"]["requestBody"]["content"]["application/json"]["schema"]["properties"]
                    self.assertEqual(properties["transport"]["const"],"esp_now");self.assertNotIn("probe_serial",properties)
                with call("/v1/capabilities",auth) as response:
                    caps=json.load(response)["data"];self.assertEqual(caps["transport"],"esp_now")
                    self.assertEqual([m["target_interface"] for m in caps["methods"] if m["available"]],["uart","swd"])
                for path in ("/v1/plan","/v1/jobs"):
                    for fields,status,code in (({"target_interface":"swd"},400,"unsupported_chip_interface"),
                                               ({"method":"serial_rom"},400,"unsupported_method"),
                                               ({"method":"stlink_swd"},400,"unsupported_method")):
                        body={"action":"flash","chip":"STM32F103C8T6","firmware":"C:/unused.bin","idempotency_key":"must-not-execute",**fields}
                        req=urllib.request.Request(base+path,data=json.dumps(body).encode(),headers=auth)
                        with self.assertRaises(urllib.error.HTTPError) as error:opener.open(req,timeout=2)
                        self.assertEqual(error.exception.code,status)
                        with error.exception as response:self.assertEqual(json.load(response)["error"]["code"],code)
                self.assertEqual(engine.store.list(),[])
            finally:server.shutdown();server.server_close();thread.join(2)


if __name__=="__main__":unittest.main()
