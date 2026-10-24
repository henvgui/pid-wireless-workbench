"""End-to-end PC -> host -> ESP-NOW -> slave UART -> STM32 -> PC integrity test."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import struct
import sys
import time
import zlib

from flash_stm32 import Link

TARGET_MARKERS = {b"F103ECHO": "STM32F103C8T6", b"F407ECHO": "STM32F407VGT6", b"F407VE01": "STM32F407VET6"}


def generate_text(size):
    result = bytearray()
    line = 0
    while len(result) < size:
        text = f"{line:08d} | PID P=1.25 I=0.12 D=0.15 STEP=0.01 | STM32双向回传完整性测试 ABCxyz0123456789\r\n".encode("utf-8")
        remaining = size - len(result)
        result.extend(text if len(text) <= remaining else b"." * remaining)
        line += 1
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--part", choices=list(TARGET_MARKERS.values()), help="Optional expected test application; otherwise detect its UART marker")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--file", type=Path, help="Send an existing file byte-for-byte (text or binary)")
    source.add_argument("--text", help="Send the given text as UTF-8")
    parser.add_argument("--bytes", type=int, default=131072, help="Generated UTF-8 text size, default 128 KiB")
    parser.add_argument("--drop-every", type=int, default=0, help="Test only: discard every Nth echo radio send at each ESP; 0 disables")
    parser.add_argument("--corrupt-uart-every", type=int, default=0, help="Test only: corrupt the first UART attempt of every Nth data block")
    parser.add_argument("--journal", type=Path, default=Path("validation/echo_roundtrip.json"))
    args = parser.parse_args()
    if not 1 <= args.bytes <= 4 * 1024 * 1024 or not 0 <= args.drop_every <= 100000 or not 0 <= args.corrupt_uart_every <= 100000:
        parser.error("Use 1..4194304 bytes and fault injection intervals 0..100000")
    data = args.file.read_bytes() if args.file else args.text.encode("utf-8") if args.text is not None else generate_text(args.bytes)
    if not 1 <= len(data) <= 4 * 1024 * 1024:
        raise ValueError("Test file must contain 1..4194304 bytes")
    args.journal.parent.mkdir(parents=True, exist_ok=True)
    sent_path = args.journal.with_name(args.journal.stem + "_sent.txt")
    returned_path = args.journal.with_name(args.journal.stem + "_returned.txt")
    sent_path.write_bytes(data)
    transfer = secrets.randbits(32) or 1
    tid = f"{transfer:08x}"
    result = {"id": transfer, "bytes": len(data), "chunk_bytes": 128,
              "source_sha256": hashlib.sha256(data).hexdigest(), "drop_every": args.drop_every,
              "corrupt_uart_every": args.corrupt_uart_every, "sent_file": str(sent_path.resolve()),
              "returned_file": str(returned_path.resolve()), "passed": False}
    returned = bytearray()
    link = None
    original_stream = None
    opened = closed = False
    radio_retries = pc_retries = 0
    def save():
        args.journal.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    def exchange(command, op, offset=0):
        nonlocal radio_retries, pc_retries
        for attempt in range(3):
            try:
                reply = link.query(command, "echo", lambda r: r.get("id") == transfer
                                   and r.get("op") == op and r.get("offset") == offset, timeout=5)
                radio_retries += reply["retries"]
                result["host_injected_radio_drops"] = reply.get("injected_drops", 0)
                if reply["ok"]:
                    return bytes.fromhex(reply["hex"])
                if reply["error"] not in ("timeout", "no_link", "busy"):
                    raise RuntimeError(reply)
            except TimeoutError:
                if attempt == 2:
                    raise
            if attempt < 2:
                pc_retries += 1
                time.sleep(.2)
        raise RuntimeError("No STM32 acknowledgement after bounded retries")
    save()
    try:
        link = Link(args.port)
        initial = link.query("!get", "state")
        original_stream = initial["stream"]
        link.query("!stream off", "state")
        link.query("!stop", "state")
        deadline = time.monotonic() + 8
        while True:
            state = link.query("!get", "state")
            if state.get("synced") and state.get("ack") and not state["run_request"]:
                break
            if time.monotonic() > deadline:
                raise RuntimeError("STOP not acknowledged")
            time.sleep(.1)
        started = time.monotonic()
        marker = exchange(f"!echo open {tid} {args.drop_every} {args.corrupt_uart_every}", 8)
        if marker not in TARGET_MARKERS:
            raise RuntimeError("Unexpected target application marker")
        opened = True
        part = TARGET_MARKERS[marker]
        result.update(target_part=part, target_marker=marker.decode("ascii"))
        if args.part and args.part != part:
            raise RuntimeError(f"Expected {args.part} application, received {part}")
        print(f"{part} UART handshake OK; {len(data)} bytes; transfer {tid}", flush=True)
        for offset in range(0, len(data), 128):
            block = data[offset:offset+128]
            echoed = exchange(f"!echo data {tid} {offset} {block.hex()}", 9, offset)
            if echoed != block:
                raise RuntimeError(f"Echo mismatch at byte offset {offset}")
            returned.extend(echoed)
            if len(returned) % 8192 == 0 or len(returned) == len(data):
                print(f"Verified round trip {len(returned)}/{len(data)}; radio retries={radio_retries}", flush=True)
        counters = exchange(f"!echo close {tid} {len(data)}", 10, len(data))
        closed = True
        if len(counters) != 24:
            raise RuntimeError("Invalid target statistics length")
        total, crc, frames, uart_retries, slave_drops, uart_corruptions = struct.unpack("<6I", counters)
        if (total, crc, frames) != (len(data), zlib.crc32(data), (len(data)+127)//128):
            raise RuntimeError(f"Target length/CRC32/unique-frame counters disagree: {total}, {crc:08x}, {frames}")
        result.update(target_bytes=total, target_crc32=f"{crc:08x}", target_unique_frames=frames,
                      uart_retries=uart_retries, slave_injected_radio_drops=slave_drops,
                      injected_uart_corruptions=uart_corruptions,
                      returned_sha256=hashlib.sha256(returned).hexdigest(),
                      seconds=round(time.monotonic()-started, 3))
        if bytes(returned) != data or result["source_sha256"] != result["returned_sha256"]:
            raise RuntimeError("Whole-file comparison/SHA256 mismatch")
        if args.text is not None:
            if hasattr(sys.stdout, "reconfigure"):
                sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
            print(part + " returned: " + repr(returned.decode("utf-8")), flush=True)
        result["passed"] = True
        print(f"PASS: {total} bytes echoed by {part}, CRC32 and SHA256 match; "
              f"radio retries={radio_retries}, UART retries={uart_retries}, seconds={result['seconds']}", flush=True)
    except BaseException as error:
        result["error"] = str(error)
        raise
    finally:
        if link:
            if opened and not closed:
                try:
                    exchange(f"!echo close {tid} {len(returned)}", 10, len(returned))
                except Exception:
                    pass
            if original_stream is not None:
                try:
                    link.query("!stream " + ("on" if original_stream else "off"), "state")
                except Exception:
                    pass
            link.close()
        returned_path.write_bytes(returned)
        result.update(returned_bytes=len(returned), radio_retries=radio_retries, pc_retries=pc_retries)
        save()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
