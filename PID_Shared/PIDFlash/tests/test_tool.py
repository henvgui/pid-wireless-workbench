import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

module_path = Path(__file__).resolve().parents[1] / "tools/flash_stm32.py"
spec = importlib.util.spec_from_file_location("flash_stm32", module_path)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def image(size=1027, stack=0x20005000):
    data = bytearray(size)
    struct.pack_into("<II", data, 0, stack, 0x08000009)
    return bytes(data)


class FakeLink(tool.Link):
    instances = []

    def __init__(self, port=None):
        self.commands = []
        self.expected_profile = None
        self.retries = 0
        self.closed = False
        self.received = bytearray()
        self.instances.append(self)

    def query(self, command, kind, accept=lambda x: True, timeout=6):
        self.commands.append(command)
        if not command.startswith("!fw"):
            return {"type": "state", "stream": True, "run_request": False, "ack": True, "synced": True}
        words = command.split()
        op = {"begin": 1, "data": 2, "seal": 3, "flash": 4, "status": 5, "abort": 6}[words[1]]
        offset = 0
        if op == 1:
            self.size = int(words[3])
            self.sha256 = words[5]
            self.profile = {"f103c8":2,"g431":1,"f407vg":3,"f407ve":4}[words[6]]
            self.interface = words[7] if len(words)==8 else "uart"
            self.state = "receiving"
        if op == 2:
            offset = int(words[3])
            assert len(self.received) == offset
            self.received.extend(bytes.fromhex(words[4]))
        if op == 3:
            assert hashlib.sha256(self.received).hexdigest() == self.sha256
            self.state = "ready"
        return {"type": "fw", "id": int(words[2], 16), "op": op, "offset": offset,
                "ok": True, "received": len(self.received), "size": self.size,
                "state": self.state, "target_enabled": False, "retries": 0,
                "profile": self.profile, "target": {2:"f103c8",1:"g431",3:"f407vg",4:"f407ve"}[self.profile],"interface":self.interface}

    def close(self):
        self.closed = True


class ToolTests(unittest.TestCase):
    def test_swd_manual_legacy_flag_and_capability_gate(self):
        for supported in (False,True):
            class F407Link(FakeLink):
                def query(self, command, *args, **kwargs):
                    value=super().query(command,*args,**kwargs)
                    if command.startswith('!fw'):
                        value.update(target_enabled=True,swd_ram=supported)
                    if command.startswith('!fw flash'):
                        assert command.endswith(' RUN LEGACY')
                        value.update(state='done',job_error='ok',start_ack=True,swd_algorithm='legacy_word')
                    return value
            with tempfile.TemporaryDirectory() as directory:
                binary=Path(directory)/'app.bin';binary.write_bytes(image())
                journal=Path(directory)/'record.json'
                args=['flash_stm32.py',str(binary),'--part','STM32F407VET6','--interface','swd','--execute','--run','--swd-legacy','--journal',str(journal)]
                with patch.object(sys,'argv',args),patch.object(tool,'Link',F407Link),contextlib.redirect_stdout(io.StringIO()):
                    if supported:tool.main()
                    else:
                        with self.assertRaises(RuntimeError):tool.main()
                commands=F407Link.instances[-1].commands
                self.assertEqual(sum(c.startswith('!fw flash') for c in commands),int(supported))
                record=json.loads(journal.read_text())
                if supported:self.assertEqual(record['algorithm'],'legacy_word');self.assertTrue(record['verified'] and record['start_ack'])
    def test_swd_manual_flag_requires_executing_swd(self):
        for suffix in ([],['--execute'],['--interface','swd']):
            with patch.object(sys,'argv',['flash_stm32.py','unused.bin','--swd-legacy']+suffix),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):tool.main()
    def test_failed_transfer_releases_staging_without_erase(self):
        class InterruptedLink(FakeLink):
            def query(self, command, *args, **kwargs):
                if command.startswith('!fw data'):
                    self.commands.append(command);raise TimeoutError()
                response=super().query(command,*args,**kwargs)
                if command.startswith('!fw abort'):response.update(state='idle')
                return response
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'app.bin';binary.write_bytes(image())
            journal=Path(directory)/'record.json'
            args=['flash_stm32.py',str(binary),'--execute','--journal',str(journal)]
            with patch.object(sys,'argv',args),patch.object(tool,'Link',InterruptedLink),patch.object(tool.time,'sleep'),contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(TimeoutError):tool.main()
            link=InterruptedLink.instances[-1]
            self.assertTrue(any(c.startswith('!fw abort') for c in link.commands))
            self.assertFalse(any(c.startswith('!fw flash') for c in link.commands))
            self.assertTrue(json.loads(journal.read_text())['staging_released'])

    def test_ambiguous_flash_ack_never_aborts_or_replays_erase(self):
        class AmbiguousLink(FakeLink):
            def fw(self, command, *args, **kwargs):
                if command.startswith('!fw flash'):
                    self.commands.append(command);raise TimeoutError('erase ACK lost')
                return super().fw(command,*args,**kwargs)
            def query(self, command, *args, **kwargs):
                response=super().query(command,*args,**kwargs)
                if command.startswith('!fw'):response['target_enabled']=True
                return response
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'app.bin';binary.write_bytes(image())
            args=['flash_stm32.py',str(binary),'--execute','--journal',str(Path(directory)/'record.json')]
            with patch.object(sys,'argv',args),patch.object(tool,'Link',AmbiguousLink),contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(TimeoutError):tool.main()
            commands=AmbiguousLink.instances[-1].commands
            self.assertEqual(sum(c.startswith('!fw flash') for c in commands),1)
            self.assertFalse(any(c.startswith('!fw abort') for c in commands))

    def test_flash_waits_for_terminal_tx_drain(self):
        class DrainingLink(FakeLink):
            def query(self, command, *args, **kwargs):
                response=super().query(command,*args,**kwargs)
                if command.startswith('!fw'):
                    response['target_enabled']=True
                    response['serial_waiting']=command.startswith('!fw seal')
                if command.startswith('!fw flash'):
                    assert self.commands[-2].startswith('!fw status')
                    response.update(state='done',job_error='ok',go_ack=False)
                return response
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'app.bin';binary.write_bytes(image())
            args=['flash_stm32.py',str(binary),'--execute','--journal',str(Path(directory)/'record.json')]
            with patch.object(sys,'argv',args),patch.object(tool,'Link',DrainingLink),patch.object(tool.time,'sleep'),contextlib.redirect_stdout(io.StringIO()):
                tool.main()
            self.assertEqual(sum(c.startswith('!fw flash') for c in DrainingLink.instances[-1].commands),1)

    def test_negotiated_chunks_and_legacy_fallback(self):
        self.assertEqual(tool.negotiated_chunk({}),128)
        self.assertEqual(tool.negotiated_chunk({'max_chunk':192}),192)
        for value in (True,0,127,193,256,'192',None):
            with self.assertRaises(RuntimeError):tool.negotiated_chunk({'max_chunk':value})

    def test_new_receiver_uses_larger_chunks_without_changing_data(self):
        class NewLink(FakeLink):
            def query(self,*args,**kwargs):
                value=super().query(*args,**kwargs)
                if value.get('type')=='fw':value['max_chunk']=192
                return value
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'app.bin';binary.write_bytes(image(1027))
            journal=Path(directory)/'job.json'
            with patch.object(sys,'argv',['flash_stm32.py',str(binary),'--journal',str(journal)]),patch.object(tool,'Link',NewLink),contextlib.redirect_stdout(io.StringIO()):
                tool.main()
            link=NewLink.instances[-1]
            self.assertEqual(link.received,binary.read_bytes())
            lengths=[len(bytes.fromhex(c.split()[4])) for c in link.commands if c.startswith('!fw data')]
            self.assertEqual(lengths,[192]*5+[67])
            self.assertEqual(json.loads(journal.read_text())['chunk_bytes'],192)
    def test_run_requires_execute(self):
        args = ["flash_stm32.py", "unused.bin", "--run"]
        with patch.object(sys, "argv", args), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                tool.main()
        self.assertEqual(error.exception.code, 2)

    def test_explicit_run_after_verified_program(self):
        class ProgrammingLink(FakeLink):
            def query(self, command, kind, accept=lambda x: True, timeout=6):
                response = super().query(command, kind, accept, timeout)
                if command.startswith("!fw"):
                    response["target_enabled"] = True
                if command.startswith("!fw flash"):
                    self.assert_ready = self.state == "ready"
                    response.update(state="done", job_error="ok", go_ack=command.endswith(" RUN"))
                return response

        with tempfile.TemporaryDirectory(prefix="pidflash_f103_test_") as directory:
            binary = Path(directory) / "f103.bin"
            binary.write_bytes(image())
            journal = Path(directory) / "transfer.json"
            args = ["flash_stm32.py", str(binary), "--journal", str(journal), "--execute", "--run"]
            with patch.object(sys, "argv", args), patch.object(tool, "Link", ProgrammingLink), contextlib.redirect_stdout(io.StringIO()):
                tool.main()
            result = json.loads(journal.read_text())
            self.assertTrue(result["verified"] and result["go_ack"])
            link = ProgrammingLink.instances[-1]
            self.assertTrue(link.assert_ready and link.closed)
            self.assertEqual(sum(command.startswith("!fw flash") for command in link.commands), 1)
            self.assertTrue(any(command.endswith(" ERASE RUN") for command in link.commands))

    def test_profiles_and_memory_bounds(self):
        f103 = tool.target_profile("STM32F103C8T6")
        self.assertEqual((f103["id"], f103["flash_kb"], f103["sram_end"]), (2, 64, 0x20005000))
        self.assertEqual(tool.flash_kb("STM32G431CBT6"), 128)
        tool.validate_image(image(65536), 64, f103)
        for data in (image(65537), image(stack=0x20005008), image(stack=0x10001000), b"bad"):
            with self.assertRaises(ValueError):
                tool.validate_image(data, 64, f103)
        with self.assertRaises(ValueError):
            tool.target_profile("STM32F103CBT6")
        f407=tool.target_profile('STM32F407VGT6')
        self.assertEqual((f407['id'],f407['flash_kb'],f407['sram_end']),(3,1024,0x20020000))
        tool.validate_image(image(1048576,0x20020000),1024,f407)
        for data in (image(1048577),image(stack=0x20020008),image(stack=0x10010000)):
            with self.assertRaises(ValueError):tool.validate_image(data,1024,f407)
    def test_f407_swd_manifest_and_interface_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'app.bin';binary.write_bytes(image(131073,0x20020000))
            args=['flash_stm32.py',str(binary),'--part','STM32F407VGT6','--interface','swd','--journal',str(Path(directory)/'job.json')]
            with patch.object(sys,'argv',args),patch.object(tool,'Link',FakeLink),contextlib.redirect_stdout(io.StringIO()):tool.main()
            link=FakeLink.instances[-1];self.assertTrue(any(c.endswith(' f407vg swd') for c in link.commands))
            self.assertEqual(link.received,binary.read_bytes())
        link=tool.Link.__new__(tool.Link);link.retries=0;link.expected_profile=3;link.expected_interface='swd'
        link.query=lambda *args,**kwargs:{'ok':True,'profile':3,'interface':'uart'}
        with self.assertRaisesRegex(RuntimeError,'interface differs'):link.fw('!fw seal 1234',0x1234,3)

    def test_f407ve_capacity_and_manifest(self):
        part=tool.target_profile('STM32F407VET6')
        self.assertEqual((part['id'],part['flash_kb']),(4,512))
        with self.assertRaises(ValueError):tool.validate_image(image(524289,0x20020000),512,part)
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'app.bin';binary.write_bytes(image(1756,0x20020000))
            args=['flash_stm32.py',str(binary),'--part','STM32F407VET6','--interface','swd','--journal',str(Path(directory)/'job.json')]
            with patch.object(sys,'argv',args),patch.object(tool,'Link',FakeLink),contextlib.redirect_stdout(io.StringIO()):tool.main()
            self.assertTrue(any(c.endswith(' f407ve swd') and ' 512 ' in c for c in FakeLink.instances[-1].commands))

    def test_wrong_or_legacy_profile_ack_rejected(self):
        for reply in ({"ok": True, "profile": 1}, {"ok": True}):
            link = tool.Link.__new__(tool.Link)
            link.retries = 0
            link.expected_profile = 2
            link.query = lambda *args, **kwargs: reply
            with self.assertRaisesRegex(RuntimeError, "profile differs"):
                link.fw("!fw seal 1234", 0x1234, 3)

    def test_default_part_and_stage_only(self):
        with tempfile.TemporaryDirectory(prefix="pidflash_f103_test_") as directory:
            binary = Path(directory) / "f103.bin"
            journal = Path(directory) / "transfer.json"
            binary.write_bytes(image())
            args = ["flash_stm32.py", str(binary), "--journal", str(journal)]
            with patch.object(sys, "argv", args), patch.object(tool, "Link", FakeLink), contextlib.redirect_stdout(io.StringIO()):
                tool.main()
            result = json.loads(journal.read_text())
            self.assertEqual(result["part"], "STM32F103C8T6")
            self.assertEqual(result["profile"], 2)
            self.assertTrue(result["staged"])
            self.assertFalse(result["executed"])
            link = FakeLink.instances[-1]
            self.assertTrue(link.closed)
            self.assertEqual(link.received, binary.read_bytes())
            self.assertTrue(any(c.startswith("!fw begin ") and c.endswith(" f103c8") for c in link.commands))
            self.assertFalse(any(c.startswith("!fw flash") for c in link.commands))

    def test_execute_rejected_when_target_interface_disabled(self):
        with tempfile.TemporaryDirectory(prefix="pidflash_f103_test_") as directory:
            binary = Path(directory) / "f103.bin"
            binary.write_bytes(image())
            args = ["flash_stm32.py", str(binary), "--journal", str(Path(directory) / "transfer.json"), "--execute"]
            with patch.object(sys, "argv", args), patch.object(tool, "Link", FakeLink), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "staging-only"):
                    tool.main()
            link = FakeLink.instances[-1]
            self.assertTrue(link.closed)
            self.assertFalse(any(c.startswith("!fw flash") for c in link.commands))


if __name__ == "__main__":
    unittest.main()
