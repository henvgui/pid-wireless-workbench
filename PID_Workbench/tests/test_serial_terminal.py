import sys
from pathlib import Path
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend"))
from workbench_core import ApiError
from serial_terminal import SerialTerminal


class FakeLink:
    replies = []
    commands = []
    closed = 0
    def __init__(self, port): self.port = port
    def query(self, command, kind, accepts, timeout=5):
        self.commands.append(command)
        result = self.replies.pop(0)
        if isinstance(result, Exception): raise result
        assert kind == "serial_log" and accepts(result)
        return result
    def close(self): type(self).closed += 1


class SerialTests(unittest.TestCase):
    def setUp(self):
        FakeLink.replies = []; FakeLink.commands = []; FakeLink.closed = 0
        self.gate = threading.Lock()
        self.term = SerialTerminal(self.gate, FakeLink, lambda p: "COM14", ApiError)
        self.config = dict(session="12345678", baud=115200, bits=8, parity="N", stop=1)
    def reply(self, op, **values):
        return dict(id=int("12345678", 16), op=op, ok=True, hex="", offset=0, dropped=0, rx_errors=0, **values)
    def start(self):
        FakeLink.replies.append(self.reply(17))
        return self.term.configure(self.config)
    def test_config_and_h1_route(self):
        result = self.start()
        self.assertEqual(FakeLink.commands, ["!log open 12345678 115200 8 0 1"])
        self.assertEqual((result["rx_pin"], result["tx_enabled"]), (44, False))
        self.assertFalse(self.gate.locked())
    def test_invalid_config_never_touches_hardware(self):
        for changes in ({"baud": True}, {"baud": 0}, {"bits": 9}, {"stop": 3}, {"parity": "X"}, {"session": "00000000"}, {"session": 10}, {"shell": "anything"}):
            with self.assertRaises(ApiError): self.term.configure(dict(self.config, **changes))
        self.assertEqual(FakeLink.commands, [])
    def test_busy_gate_never_opens_port(self):
        self.gate.acquire()
        with self.assertRaises(ApiError) as error: self.term.configure(self.config)
        self.assertEqual(error.exception.status, 409)
        self.assertEqual(FakeLink.commands, [])
        self.gate.release()
    def test_read_binary_and_cursor(self):
        self.start(); reply=self.reply(18);reply.update(hex="00ff41",offset=0)
        FakeLink.replies.append(reply);result=self.term.read("12345678",0)
        self.assertEqual((result["chunks"][0]["hex"],result["next_cursor"]),("00ff41",3))
    def test_overflow_gap_is_visible(self):
        self.start();reply=self.reply(18);reply.update(offset=200,hex="6162",dropped=200,rx_errors=1)
        FakeLink.replies.append(reply);result=self.term.read("12345678",0)
        self.assertEqual((result["chunks"][0]["gap"],result["next_cursor"],result["dropped"]),(200,202,200))
    def test_failed_response_can_retry_same_cursor(self):
        self.start();FakeLink.replies.append(TimeoutError("lost reply"))
        with self.assertRaises(TimeoutError):self.term.read("12345678",0)
        reply=self.reply(18);reply.update(hex="6162");FakeLink.replies.append(reply)
        result=self.term.read("12345678",0)
        self.assertEqual(FakeLink.commands[-1],FakeLink.commands[-2]);self.assertEqual(result["next_cursor"],2)
        self.assertFalse(self.gate.locked())
    def test_stale_session_and_invalid_cursor(self):
        self.start()
        for session,cursor in (("ffffffff",0),("12345678",-1),("12345678",True),("12345678",2**32)):
            with self.assertRaises(ApiError):self.term.read(session,cursor)
        self.assertFalse(self.gate.locked())
    def test_close_restores_debug_uart_without_reset(self):
        self.start();FakeLink.replies.append(self.reply(19));self.assertTrue(self.term.close({"session":"12345678"})["closed"])
        self.assertEqual(FakeLink.commands[-1],"!log close 12345678")
    def test_reconfigure_same_session_cannot_change_format(self):
        self.start()
        with self.assertRaises(ApiError):self.term.configure(dict(self.config,bits=7))
        self.assertFalse(self.gate.locked())
    def test_lost_http_reply_replays_entire_cached_batch(self):
        self.start();reply=self.reply(18);reply.update(hex="616263")
        FakeLink.replies.append(reply)
        first=self.term.read("12345678",0);calls=len(FakeLink.commands)
        self.assertEqual(self.term.read("12345678",0),first)
        self.assertEqual(len(FakeLink.commands),calls)
        self.assertEqual(self.term.status()["received_bytes"],3)
    def test_empty_read_is_not_cached_forever(self):
        self.start();FakeLink.replies.append(self.reply(18))
        self.assertEqual(self.term.read("12345678",0)["chunks"],[])
        reply=self.reply(18);reply.update(hex="61");FakeLink.replies.append(reply)
        self.assertEqual(self.term.read("12345678",0)["next_cursor"],1)
    def test_partial_batch_survives_next_radio_failure(self):
        self.start();reply=self.reply(18);reply.update(hex="61"*108)
        FakeLink.replies.extend([reply,TimeoutError("radio interrupted")])
        first=self.term.read("12345678",0)
        self.assertEqual(first["next_cursor"],108)
        self.assertEqual(self.term.read("12345678",0),first)
        self.assertFalse(self.gate.locked())
    def test_remote_session_loss_has_structured_reason(self):
        self.start();reply=self.reply(18);reply.update(ok=False,error="wrong_transfer")
        FakeLink.replies.append(reply)
        with self.assertRaises(ApiError) as failure:self.term.read("12345678",0)
        self.assertEqual(failure.exception.remote_error,"wrong_transfer")
    def test_status_is_read_only_even_while_hardware_busy(self):
        self.start();calls=len(FakeLink.commands);self.gate.acquire()
        status=self.term.status();self.gate.release()
        self.assertEqual(status["session"],"12345678");self.assertFalse(status["closed"])
        self.assertEqual(len(FakeLink.commands),calls)
    def test_successful_read_clears_stale_error_and_reopen_clears_read_time(self):
        self.start();FakeLink.replies.append(TimeoutError('temporary failure'))
        with self.assertRaises(TimeoutError):self.term.read('12345678',0)
        self.assertEqual(self.term.status()['last_error'],'temporary failure')
        FakeLink.replies.append(self.reply(18));self.term.read('12345678',0)
        self.assertIsNone(self.term.status()['last_error'])
        self.assertIsNotNone(self.term.status()['last_read_at'])
        FakeLink.replies.append(self.reply(19));self.term.close({'session':'12345678'})
        FakeLink.replies.append(self.reply(17));self.term.configure(self.config)
        self.assertIsNone(self.term.status()['last_read_at'])
    def test_external_close_is_distinguishable_from_restart(self):
        self.start();FakeLink.replies.append(self.reply(19));self.term.close({"session":"12345678"})
        self.assertTrue(self.term.status()["closed"])
        with self.assertRaises(ApiError) as failure:self.term.read("12345678",0)
        self.assertEqual(failure.exception.code,"serial_closed")


if __name__ == "__main__": unittest.main()
