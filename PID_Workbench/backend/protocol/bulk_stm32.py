"""Credit-limited native USB batches; ESP32 owns selective-repeat radio window."""
import json
import struct
import time
import zlib

USB_BATCH = 3072
HOST_CACHE = 6144


def encode_frame(kind, transfer, offset=0, payload=b""):
    if kind not in (1, 2, 3) or len(payload) > USB_BATCH or kind != 1 and payload:
        raise ValueError("Invalid bulk USB frame")
    raw = struct.pack("<2sBBIIHH", b"BU", 1, kind, transfer, offset, len(payload), 0)+payload
    raw += struct.pack("<I", zlib.crc32(raw))
    result = bytearray(b"\0\0")
    code_at, code = 1, 1
    for byte in raw:
        if byte == 0:
            result[code_at] = code
            code_at, code = len(result), 1
            result.append(0)
        else:
            result.append(byte)
            code += 1
            if code == 255:
                result[code_at] = code
                code_at, code = len(result), 1
                result.append(0)
    result[code_at] = code
    result.append(0)
    return bytes(result)


def write_frame(link, kind, transfer, offset=0, payload=b""):
    frame = encode_frame(kind, transfer, offset, payload)
    written = link.serial.write(frame)
    if written != len(frame):
        raise IOError("Partial bulk USB write; receipt is uncertain")


def transfer_image(link, transfer, image, start, on_progress=None, drop_every=0):
    """No target reset/erase. Errors return to text mode before caller cleanup."""
    if not 0 <= start < len(image) or start % 192:
        raise ValueError("Invalid bulk start offset")
    status = link.fw(f"!fw bulk {transfer:08x} {start} {drop_every}", transfer, 26, start)
    if status.get("bulk_window") != 8:
        raise RuntimeError("Bulk window negotiation failed")
    sent = confirmed = start
    started = last_progress = time.monotonic()
    last_activity = started
    retries = 0
    completed = False
    try:
        while confirmed < len(image):
            if time.monotonic()-last_activity > 5:
                raise TimeoutError("Bulk transfer made no confirmed progress; target was not erased")
            while sent < len(image):
                payload = image[sent:sent+USB_BATCH]
                if len(payload) > HOST_CACHE-(sent-confirmed):
                    break
                write_frame(link, 1, transfer, sent, payload)
                sent += len(payload)
            try:
                value = link.receive("bulk", lambda v: v.get("id") == transfer, timeout=.3)
            except TimeoutError:
                if time.monotonic()-last_activity > 5:
                    raise TimeoutError("Bulk transfer stalled; no erase was requested")
                write_frame(link, 3, transfer)
                continue
            if value.get("error") != "ok" or value.get("active") is not True:
                raise RuntimeError("Bulk transfer stopped: "+str(value))
            received, queued = value.get("received"), value.get("queued")
            if type(received) is not int or type(queued) is not int or not confirmed <= received <= queued <= sent:
                raise RuntimeError("Invalid bulk progress/credit acknowledgement")
            if value.get("credit") != HOST_CACHE-(queued-received):
                raise RuntimeError("Invalid bulk receive credit")
            if received > confirmed:
                last_activity = time.monotonic()
            confirmed, retries = received, value.get("retries", 0)
            if on_progress and (time.monotonic()-last_progress >= .2 or confirmed == len(image)):
                on_progress(confirmed, retries)
                last_progress = time.monotonic()
        completed = True
        return {"mode": "bulk_window8", "bytes": len(image)-start, "seconds": time.monotonic()-started,
                "radio_retries": retries, "usb_batch": USB_BATCH, "window": 8,
                "injected_data_drops": value.get("injected_data_drops",0),
                "injected_ack_drops": value.get("injected_ack_drops",0)}
    finally:
        try:
            # Extra zero terminates any interrupted COBS frame before Exit.
            link.serial.write(b"\0")
            write_frame(link, 2, transfer)
            link.receive("bulk", lambda v: v.get("id") == transfer and v.get("active") is False, timeout=3)
        except Exception:
            if completed:
                raise RuntimeError("Image received, but USB bulk exit not acknowledged; target was not erased")
