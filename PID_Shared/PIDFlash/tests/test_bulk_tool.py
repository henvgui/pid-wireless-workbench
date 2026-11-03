import importlib.util
from pathlib import Path
import struct
import unittest
import zlib
import sys
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("bulk_stm32", Path(__file__).resolve().parents[1]/"tools/bulk_stm32.py")
bulk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bulk)


def decode(frame):
    if frame == b"\0":
        return None
    assert frame[0] == frame[-1] == 0
    encoded, raw, i = frame[1:-1], bytearray(), 0
    while i < len(encoded):
        code = encoded[i]
        assert code > 0
        raw.extend(encoded[i+1:i+code])
        i += code
        if code != 255 and i < len(encoded):
            raw.append(0)
    magic, version, kind, tid, offset, length, reserved = struct.unpack_from("<2sBBIIHH", raw)
    assert magic == b"BU" and version == 1 and not reserved and len(raw) == length+20
    assert struct.unpack_from("<I", raw, len(raw)-4)[0] == zlib.crc32(raw[:-4])
    return kind, tid, offset, bytes(raw[16:-4])


class Link:
    def __init__(self, size, invalid=False, fail_write=False):
        self.serial = self
        self.size = size
        self.invalid = invalid
        self.fail_write = fail_write
        self.rx = bytearray()
        self.confirmed = 0
        self.active = True
        self.exited = False
        self.peak_pending = 0

    def fw(self, command, transfer, op, offset):
        assert op == 26
        self.tid = transfer
        self.confirmed = offset
        self.rx = bytearray(b"\0"*offset)
        return {"bulk_window": 8}

    def write(self, frame):
        decoded = decode(frame)
        if decoded is None:
            return len(frame)
        kind, tid, offset, payload = decoded
        assert tid == self.tid
        if kind == 1:
            if self.fail_write:
                self.fail_write = False
                return len(frame)-1
            assert offset == len(self.rx)
            self.rx.extend(payload)
            self.peak_pending = max(self.peak_pending, len(self.rx)-self.confirmed)
            assert self.peak_pending <= bulk.HOST_CACHE
        elif kind == 2:
            self.active = False
            self.exited = True
        return len(frame)

    def receive(self, kind, accepts, timeout):
        if self.active:
            self.confirmed = len(self.rx)
        reply = {"type":"bulk", "id":self.tid, "received":self.confirmed,
                 "queued":len(self.rx), "credit":bulk.HOST_CACHE-(len(self.rx)-self.confirmed),
                 "error":"ok", "active":self.active,"retries":2}
        if self.invalid and self.active:
            reply["received"] = len(self.rx)+192
        assert accepts(reply)
        return reply


class BulkToolTests(unittest.TestCase):
    def test_read_only_recovery_does_not_replay_write_commands(self):
        tool_spec=importlib.util.spec_from_file_location("bulk_flash_tool",Path(__file__).resolve().parents[1]/"tools/flash_stm32.py")
        tool=importlib.util.module_from_spec(tool_spec)
        tool_spec.loader.exec_module(tool)
        link=tool.Link.__new__(tool.Link)
        link.serial=Mock()
        link.receive=Mock(side_effect=[TimeoutError(),{"type":"state"}])
        with patch.dict(sys.modules,{"bulk_stm32":bulk}):
            self.assertEqual(link.query("!get","state"),{"type":"state"})
        writes=[call.args[0] for call in link.serial.write.call_args_list]
        self.assertEqual(writes[0],b"!get\n")
        self.assertNotIn(b"!",writes[1])
        self.assertNotIn(b"?",writes[1])
        link.serial.reset_mock()
        link.receive=Mock(side_effect=TimeoutError())
        with self.assertRaises(TimeoutError):link.query("!fw flash 1 ERASE","fw")
        self.assertEqual(link.serial.write.call_count,1)

    def test_frame_all_bytes_and_command_text(self):
        payload = bytes(range(256))*12
        self.assertEqual(decode(bulk.encode_frame(1, 123, 192, payload)), (1,123,192,payload))
        self.assertEqual(decode(bulk.encode_frame(2, 123)), (2,123,0,b""))

    def test_credit_limits_and_partial_final_batch(self):
        payload = bytes(range(256))*200+b"last"
        link = Link(len(payload))
        result = bulk.transfer_image(link, 23, payload, 0)
        self.assertEqual(bytes(link.rx), payload)
        self.assertTrue(link.exited)
        self.assertEqual(result["bytes"], len(payload))
        self.assertEqual(link.peak_pending, bulk.HOST_CACHE)

    def test_uncertain_usb_write_exits_without_replay(self):
        link = Link(1000, fail_write=True)
        with self.assertRaisesRegex(IOError, "Partial"):
            bulk.transfer_image(link, 12, b"x"*1000, 0)
        self.assertTrue(link.exited)
        self.assertEqual(link.rx, b"")

    def test_invalid_future_ack_is_not_success(self):
        link = Link(1000, invalid=True)
        with self.assertRaisesRegex(RuntimeError, "progress"):
            bulk.transfer_image(link, 12, b"x"*1000, 0)
        self.assertTrue(link.exited)

    def test_resume_starts_at_contiguous_chunk_boundary(self):
        payload = b"\0"*192+b"x"*32769
        link = Link(len(payload))
        result = bulk.transfer_image(link, 12, payload, 192)
        self.assertEqual(bytes(link.rx), payload)
        self.assertEqual(result["bytes"], len(payload)-192)


if __name__ == "__main__":
    unittest.main()
