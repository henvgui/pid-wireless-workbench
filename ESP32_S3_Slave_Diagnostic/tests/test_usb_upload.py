import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
import tempfile
from unittest.mock import patch

spec=importlib.util.spec_from_file_location("usb_upload",Path(__file__).resolve().parents[1]/"tools/usb_upload.py")
usb=importlib.util.module_from_spec(spec);spec.loader.exec_module(usb)
MAC="44:1b:f6:8d:10:18"

class Port:
    def __init__(self):self.closed=False
    def close(self):self.closed=True

class Chip:
    def __init__(self,error=None,identity=MAC):self._port=Port();self.error=error;self.identity=identity;self.modes=[]
    def connect(self,**kwargs):
        self.modes.append(kwargs["mode"])
        if self.error:raise self.error
    def read_mac(self):return bytes.fromhex(self.identity.replace(":",""))

class Tests(unittest.TestCase):
    def test_usb_reenumeration_reopens_by_identity_and_syncs_without_reset(self):
        old=Chip(OSError(22,"device disconnected"));new=Chip();names=[]
        def create(port):names.append(port);return old if len(names)==1 else new
        with patch.object(usb,"find_port",side_effect=["COM18","COM21"]),patch.object(usb.time,"sleep"):
            report={};self.assertIs(usb.connect(MAC,None,create,report),new)
        self.assertTrue(old._port.closed);self.assertEqual(names,["COM18","COM21"])
        self.assertEqual(old.modes,["usb_reset"]);self.assertEqual(new.modes,["no_reset"])
        self.assertEqual(report["port"],"COM21")

    def test_wrong_rom_identity_fails_without_retrying_another_reset(self):
        wrong=Chip(identity="44:1b:f6:8d:10:20");calls=[]
        with patch.object(usb,"find_port",return_value="COM18"):
            with self.assertRaises(ValueError):usb.connect(MAC,None,lambda p:(calls.append(p) or wrong),{})
        self.assertEqual(len(calls),1);self.assertTrue(wrong._port.closed)

    def test_ports_with_other_identity_or_vid_are_not_selected(self):
        candidates=[SimpleNamespace(device="COM14",serial_number="44:1B:F6:8D:10:20",vid=0x303a,pid=0x1001),
                    SimpleNamespace(device="COM8",serial_number=MAC,vid=0x1a86,pid=0x7523),
                    SimpleNamespace(device="COM21",serial_number=MAC.upper(),vid=0x303a,pid=0x1001)]
        self.assertEqual(usb.find_port(MAC,lambda:candidates,0),"COM21")
        with self.assertRaises(RuntimeError):usb.find_port(MAC,lambda:candidates[:2],0)

    def test_overlapping_images_are_rejected_before_usb_or_flash_access(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);image=root/"image.bin";image.write_bytes(b"x"*32);report=root/"report.json"
            with patch.object(usb,"connect") as connect:
                result=usb.main(["--identity",MAC,"--image","0x10000",str(image),"--image","0x10010",str(image),"--report",str(report)])
            self.assertEqual(result,1);connect.assert_not_called()
            self.assertFalse(__import__("json").loads(report.read_text())["flash_write_started"])

if __name__=="__main__":unittest.main()
