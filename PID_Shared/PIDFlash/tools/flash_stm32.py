"""Stage STM32 F103/G431/F407 .bin over ESP-NOW; --interface selects the slave UART or SWD."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import struct
import sys
import time

import serial
from serial.tools import list_ports


def target_profile(part):
    if part.upper() == "STM32F103C8T6":
        return {"name": "f103c8", "id": 2, "flash_kb": 64, "sram_end": 0x20005000, "ccm_end": 0}
    if part.upper() == "STM32F407VGT6":
        return {"name": "f407vg", "id": 3, "flash_kb": 1024, "sram_end": 0x20020000, "ccm_end": 0}
    if part.upper() == "STM32F407VET6":
        return {"name": "f407ve", "id": 4, "flash_kb": 512, "sram_end": 0x20020000, "ccm_end": 0}
    # Examples: STM32G431CBT6, STM32G431KBU6, STM32G431RBT6.
    m = re.fullmatch(r"STM32G431[C K M R V](6|8|B)[A-Z0-9]+".replace(" ", ""), part.upper())
    if not m:
        raise ValueError("Supported: STM32F103C8T6, STM32F407VGT6/VET6 or a complete G431 part number")
    return {"name": "g431", "id": 1, "flash_kb": {"6": 32, "8": 64, "B": 128}[m[1]],
            "sram_end": 0x20008000, "ccm_end": 0x10002800}


def flash_kb(part):
    return target_profile(part)["flash_kb"]


def validate_image(image, kb, target=None):
    target = target or target_profile("STM32G431CBT6")
    if not 8 <= len(image) <= kb * 1024:
        raise ValueError("Image size exceeds the selected part or is too short")
    sp, pc = struct.unpack_from("<II", image)
    if not ((0x20000000 < sp <= target["sram_end"] or
             target["ccm_end"] and 0x10000000 < sp <= target["ccm_end"])
            and sp % 8 == 0 and pc & 1 and 0x08000008 <= pc & ~1 < 0x08000000 + len(image)):
        raise ValueError(f"Invalid {target['name']} vector table; use a .bin linked at 0x08000000")


class Link:
    def __init__(self, port=None):
        if not port:
            port = next((p.device for p in list_ports.comports()
                         if p.serial_number == "44:1B:F6:8D:10:20"), None)
        if not port:
            raise RuntimeError("Host not found; specify --port COMxx")
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=.05, write_timeout=2)
        self.serial.dtr, self.serial.rts, self.serial.port = True, False, port
        self.serial.open()
        self.buffer = b""
        self.retries = 0
        self.expected_profile = None
        self.expected_interface = "uart"

    def query(self, command, kind, accept=lambda x: True, timeout=6):
        self.serial.write((command + "\n").encode("ascii"))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if b"\n" not in self.buffer:
                self.buffer += self.serial.read(self.serial.in_waiting or 1)
                if len(self.buffer) > 65536:
                    raise RuntimeError("Invalid oversized USB response")
                continue
            line, self.buffer = self.buffer.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if value.get("type") == "error":
                raise RuntimeError(value)
            if value.get("type") == kind and accept(value):
                return value
        raise TimeoutError(command[:70])

    def fw(self, command, transfer, op, offset=0):
        for attempt in range(4):
            try:
                value = self.query(command, "fw", lambda x: x.get("id") == transfer
                                   and x.get("op") == op and x.get("offset") == offset)
                if value["ok"]:
                    if self.expected_profile is not None and value.get("profile") != self.expected_profile:
                        raise RuntimeError("Receiver target profile differs; update both ESP32 firmwares")
                    if value.get("interface", "uart") != getattr(self,"expected_interface","uart"):
                        raise RuntimeError("Receiver interface differs; update both ESP32 firmwares")
                    self.retries += value["retries"]
                    return value
                if value["error"] not in ("timeout", "no_link"):
                    if value["error"] == "no_memory":
                        raise RuntimeError("Slave staging memory unavailable; images above 128 KiB require slave_flash_psram on N16R8")
                    raise RuntimeError(value)
            except TimeoutError:
                if attempt == 3:
                    raise
            self.retries += 1
            time.sleep(.5)
        raise RuntimeError("Wireless transfer unavailable; staging may be resumed with the journal")

    def close(self):
        self.serial.close()

def negotiated_chunk(status):
    """Old hosts/receivers retain 128-byte packets; both new ends permit 192."""
    value=status.get("max_chunk",128)
    if type(value) is not int or value not in (128,192):
        raise RuntimeError("Invalid receiver transfer chunk capability")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--part", default="STM32F103C8T6")
    parser.add_argument("--interface", choices=["uart","swd"], default="uart", help="Slave target interface; SWD supports F407VGT6/VET6")
    parser.add_argument("--port")
    parser.add_argument("--execute", action="store_true", help="Erase/program image-covered target pages, then read back; leave target in ROM")
    parser.add_argument("--run", action="store_true", help="Explicitly send ROM GO after successful full readback; requires --execute")
    parser.add_argument("--resume", type=Path, help="Reuse transfer ID and verify identical manifest; RAM staging survives only while slave stays powered")
    parser.add_argument("--journal", type=Path, default=Path("stm32_transfer.json"))
    parser.add_argument("--timeout", type=float, help="Programming timeout; defaults to 180s for F103/G431, 1800s for F407")
    args = parser.parse_args()
    if args.run and not args.execute:
        parser.error("--run requires --execute")
    image = args.binary.read_bytes()
    target = target_profile(args.part)
    if args.interface == "swd" and target["id"] not in (3, 4):
        parser.error("Slave SWD currently supports STM32F407VGT6/VET6")
    if args.timeout is not None and args.timeout <= 0:
        parser.error("--timeout must be positive")
    kb = target["flash_kb"]
    validate_image(image, kb, target)
    digest = hashlib.sha256(image).hexdigest()
    record = {"part": args.part.upper(), "file": str(args.binary.resolve()), "size": len(image),
              "flash_kb": kb, "profile": target["id"], "target_interface":args.interface,
              "sha256": digest, "address": "0x08000000", "executed": False}
    if args.resume:
        previous = json.loads(args.resume.read_text(encoding="utf-8"))
        if any(previous[k] != record[k] for k in ("part", "size", "flash_kb", "sha256")):
            raise ValueError("Resume manifest differs from the original image")
        if previous.get("target_interface","uart") != args.interface:
            raise ValueError("Resume interface differs from the original image")
        transfer = int(previous["id"])
    else:
        transfer = secrets.randbits(32) or 1
    record["id"] = transfer
    tid = f"{transfer:08x}"
    print(f"Part={args.part} size={len(image)} SHA256={digest} transfer={tid}", flush=True)
    link = None
    original_stream = None
    begin_dispatched = flash_dispatched = False
    def save():
        args.journal.write_text(json.dumps(record, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    save()
    try:
        link = Link(args.port)
        link.expected_profile = target["id"]
        link.expected_interface = args.interface
        initial = link.query("!get", "state")
        original_stream = initial["stream"]
        link.query("!stream off", "state")
        link.query("!stop", "state")
        deadline = time.monotonic()+8
        while True:
            current = link.query("!get", "state")
            if not current["run_request"] and current["ack"] and current.get("synced"):
                break
            if time.monotonic() > deadline:
                raise RuntimeError("STOP has not been confirmed by the slave")
            time.sleep(.1)
        suffix = " swd" if args.interface == "swd" else ""
        begin_dispatched = True
        status = link.fw(f"!fw begin {tid} {len(image)} {kb} {digest} {target['name']}"+suffix, transfer, 1)
        chunk=negotiated_chunk(status)
        record["chunk_bytes"]=chunk
        last_report=time.monotonic()
        offset = status["received"]
        if not 0 <= offset <= len(image):
            raise RuntimeError("Invalid receiver offset")
        while offset < len(image):
            block = image[offset:offset+chunk]
            status = link.fw(f"!fw data {tid} {offset} {block.hex()}", transfer, 2, offset)
            if status["received"] != offset + len(block):
                raise RuntimeError("Receiver acknowledgement offset mismatch")
            offset = status["received"]
            if time.monotonic()-last_report>=.2 or offset == len(image):
                print(f"Received {offset}/{len(image)}; retries={link.retries}", flush=True)
                record.update(received=offset, retries=link.retries)
                save()
                last_report=time.monotonic()
        if status["state"] not in ("done", "programming"):
            status = link.fw(f"!fw seal {tid}", transfer, 3)
        record["staged"] = True
        record["status"] = status
        save()
        if not args.execute:
            print("Staged and SHA256 verified. Target was not erased or programmed.")
            return
        if not status.get("target_enabled"):
            raise RuntimeError("Slave is staging-only; build slave_flash for target programming")
        drain_deadline = time.monotonic()+30
        while status.get("serial_waiting"):
            if time.monotonic() > drain_deadline:
                raise TimeoutError("H2 UART TX did not drain; target was not erased")
            time.sleep(.05)
            status = link.fw(f"!fw status {tid}", transfer, 5)
        flash_dispatched = True  # Even a lost ACK may mean erase/program started.
        status = link.fw(f"!fw flash {tid} ERASE" + (" RUN" if args.run else ""), transfer, 4)
        record["executed"] = True
        save()
        deadline = time.monotonic()+(args.timeout or (1800 if target["id"] in (3, 4) else 180))
        while status["state"] == "programming":
            if time.monotonic() > deadline:
                raise TimeoutError("Target operation still running; do not automatically re-erase")
            time.sleep(.25)
            status = link.fw(f"!fw status {tid}", transfer, 5)
            print(f"Stage={status['stage']} bytes={status['progress']}/{len(image)}", flush=True)
            record["status"] = status
            save()
        if status["state"] != "done" or status["job_error"] != "ok":
            raise RuntimeError(status)
        record["verified"] = True
        if args.run:
            acknowledged = status.get("start_ack") if args.interface=="swd" else status.get("go_ack")
            if not acknowledged:
                raise RuntimeError("Image verified but target start was not acknowledged")
            record["start_ack"] = True
            record["go_ack"] = args.interface=="uart"
            print("Programmed, full readback verified, and target start acknowledged ("+args.interface+").")
        else:
            print("Programmed and full readback verified. "+("Target remains halted; reset to run." if args.interface=="swd" else "Target remains in ROM; reset with BOOT0 LOW to run."))
    except BaseException as exc:
        record["error"] = str(exc)
        raise
    finally:
        if link:
            record["retries"] = link.retries
            if "error" in record and begin_dispatched and not flash_dispatched:
                # No ERASE was dispatched, so releasing our staging transaction
                # is safe. Never abort or re-erase after ambiguous Flash ACKs.
                try:
                    aborted = link.query(f"!fw abort {tid}", "fw", lambda value:
                        value.get("id")==transfer and value.get("op")==6, timeout=4)
                    record["staging_released"] = bool(aborted.get("ok") and aborted.get("state")=="idle")
                    if not record["staging_released"]:
                        record["cleanup_error"] = str(aborted.get("error", "staging not idle"))
                except Exception as error:
                    record["staging_released"] = False
                    record["cleanup_error"] = str(error)
            if original_stream is not None:
                try:
                    link.query("!stream " + ("on" if original_stream else "off"), "state")
                except Exception:
                    pass
            link.close()
        save()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
