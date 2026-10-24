"""ESP32-S3 native USB upload with identity checks and reset/re-enumeration recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import site
import sys
import time


def find_port(identity, enumerate_ports, deadline=8):
    until=time.monotonic()+deadline
    while True:
        matches=[p.device for p in enumerate_ports() if (p.serial_number or "").lower()==identity.lower()
                 and p.vid==0x303a and p.pid==0x1001]
        if len(matches)==1:return matches[0]
        if len(matches)>1:raise RuntimeError("Multiple USB devices have the selected identity")
        if time.monotonic()>=until:raise RuntimeError("Selected ESP32 USB device did not reconnect: "+identity)
        time.sleep(.2)


def connect(identity, enumerate_ports, make_chip, report):
    last=None
    for attempt,mode in enumerate(("usb_reset","no_reset","usb_reset"),1):
        chip=None
        try:
            port=find_port(identity,enumerate_ports)
            print(f"[USB] Connect {identity} on {port}; {mode} ({attempt}/3)",flush=True)
            chip=make_chip(port)
            chip.connect(mode=mode,attempts=1)
            actual=":".join(f"{b:02x}" for b in chip.read_mac())
            if actual.lower()!=identity.lower():raise ValueError("ROM identity differs from selected USB device")
            report["port"]=port;report["rom_identity"]=actual
            return chip
        except ValueError:
            if chip is not None:chip._port.close()
            raise
        except Exception as error:
            last=error;report.setdefault("connection_errors",[]).append(str(error))
            if chip is not None:chip._port.close()
            # A USB reset can invalidate the old Windows handle. Re-enumerate
            # by identity and sync the replacement port without resetting again.
            time.sleep(.6)
    raise RuntimeError("Automatic USB download entry failed: "+str(last))


def application_info(identity,enumerate_ports,serial_module,timeout=10):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        link=None
        try:
            port=find_port(identity,enumerate_ports,deadline=.5)
            link=serial_module.Serial();link.port=port;link.baudrate=115200
            link.timeout=.15;link.write_timeout=.5;link.dtr=True;link.rts=False;link.open()
            link.reset_input_buffer();link.write(b"!info\n")
            read_until=time.monotonic()+.8
            while time.monotonic()<read_until:
                try:value=json.loads(link.readline())
                except (ValueError,UnicodeError):continue
                if value.get("type") in ("info","ready") and value.get("firmware"):return value
        except (OSError,RuntimeError):pass
        finally:
            if link is not None:link.close()
        time.sleep(.2)
    raise RuntimeError("USB write/reset completed but application did not reply to !info")


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity",required=True)
    parser.add_argument("--esptool-dir",type=Path,default=Path.home()/".platformio/packages/tool-esptoolpy")
    parser.add_argument("--image",nargs=2,action="append",default=[],metavar=("ADDRESS","FILE"))
    parser.add_argument("--probe",action="store_true",help="Test automatic entry/exit without writing Flash")
    parser.add_argument("--report",type=Path)
    args=parser.parse_args(argv)
    report={"identity":args.identity,"probe_only":args.probe,"flash_write_started":False,"images":[]}
    chip=None
    try:
        images=[]
        for address,path in args.image:
            offset=int(address,0);file=Path(path).resolve(strict=True);raw=file.read_bytes()
            if offset<0 or not raw or offset+len(raw)>16*1024*1024:raise ValueError("Image outside 16MB Flash")
            images.append((offset,file))
            report["images"].append(dict(address=offset,path=str(file),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
        for index,(offset,file) in enumerate(sorted(images)):
            if index and offset<sorted(images)[index-1][0]+sorted(images)[index-1][1].stat().st_size:raise ValueError("Overlapping images")
        if not args.probe and not images:raise ValueError("No Flash images supplied")
        sys.path.insert(0,str(args.esptool_dir));site.addsitedir(str(args.esptool_dir/"_contrib"))
        import esptool
        import serial
        from serial.tools import list_ports
        from esptool.targets.esp32s3 import ESP32S3ROM
        chip=connect(args.identity,list_ports.comports,lambda port:ESP32S3ROM(port,115200),report)
        if not args.probe:
            command=["--chip","esp32s3","--no-stub","--before","no_reset","--after","no_reset","write_flash",
                     "--flash_mode","keep","--flash_size","16MB"]
            for address,file in images:command.extend((hex(address),str(file)))
            # Do not reopen USB between ROM identity verification and write.
            report["flash_write_started"]=True
            esptool.main(command,esp=chip)
            report["write_and_hash_verified"]=True
        print("[USB] Restart application automatically",flush=True)
        try:chip.hard_reset()
        except (OSError,serial.SerialException) as error:report["reset_disconnect"]=str(error)
        chip._port.close();chip=None
        report["application"]=application_info(args.identity,list_ports.comports,serial)
        report["ok"]=True
        print("[USB] Application running: "+report["application"]["firmware"],flush=True)
        return 0
    except Exception as error:
        report.update(ok=False,error=str(error));print("[USB] ERROR: "+str(error),flush=True)
        return 1
    finally:
        if chip is not None:chip._port.close()
        if args.report:
            args.report.parent.mkdir(parents=True,exist_ok=True)
            args.report.write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")


if __name__=="__main__":sys.exit(main())
