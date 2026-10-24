import sys, threading, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'backend'))
from serial_terminal import SerialTerminal
from workbench_core import ApiError

class Wire:
    def __init__(self):
        self.commands=[];self.epoch=123;self.total=0;self.raw=bytearray();self.fail=False;self.replies={}
    def close(self):pass
    def query(self, command, kind, accepts, timeout=5):
        assert kind=='serial_stream'
        self.commands.append(command);words=command.split();op={'open':21,'read':22,'write':23,'close':24}[words[1]]
        reply=dict(id=int(words[2],16),op=op,offset=0,ok=True,error='ok',epoch=self.epoch,hex='',dropped=0,rx_errors=0,tx_total=self.total)
        if op==23:
            offset=int(words[3]);data=bytes.fromhex(words[5])
            if offset not in self.replies:
                assert offset==self.total
                self.total+=len(data);self.raw.extend(data);self.replies[offset]=self.total
            reply['tx_total']=self.replies[offset]
            if self.fail:self.fail=False;raise TimeoutError('lost ACK after UART enqueue')
        if op==22:
            reply['offset']=int(words[3]);reply['hex']=self.raw[reply['offset']:reply['offset']+96].hex()
        assert accepts(reply)
        return reply

class WirelessTests(unittest.TestCase):
    def setUp(self):
        self.wire=Wire();self.term=SerialTerminal(threading.Lock(),lambda port:self.wire,lambda port:'COM14',ApiError)
        self.config=dict(session='12345678',channel='h2')
        self.term.configure(self.config)
    def spec(self,raw=b'hello',key='one'):return dict(session='12345678',idempotency_key=key,hex=raw.hex())
    def test_h2_pins_and_config(self):
        self.assertIn('!serial open',self.wire.commands[0]);self.assertEqual(self.term.config['channel'],'h2')
    def test_all_binary_and_chunking(self):
        raw=bytes(range(256))*4;answer=self.term.send(self.spec(raw));self.assertEqual(answer['accepted'],1024)
        self.assertEqual(bytes(self.wire.raw),raw);self.assertEqual(len([c for c in self.wire.commands if ' write ' in c]),8)
    def test_lost_ack_and_retry_never_duplicates(self):
        raw=bytes(range(256));self.wire.fail=True
        with self.assertRaises(TimeoutError):self.term.send(self.spec(raw))
        with self.assertRaises(ApiError):self.term.send(self.spec(raw,'different'))
        answer=self.term.send(self.spec(raw));self.assertEqual(answer['accepted'],256);self.assertEqual(bytes(self.wire.raw),raw)
        count=len(self.wire.commands);self.assertEqual(self.term.send(self.spec(raw)),answer);self.assertEqual(len(self.wire.commands),count)
    def test_same_key_different_bytes_rejected(self):
        self.term.send(self.spec())
        with self.assertRaises(ApiError):self.term.send(self.spec(b'changed'))
        self.assertEqual(bytes(self.wire.raw),b'hello')
    def test_epoch_change_stops_send(self):
        self.wire.epoch+=1
        with self.assertRaises(ApiError) as error:self.term.send(self.spec())
        self.assertEqual(error.exception.code,'serial_session_expired')
    def test_rx_preserves_binary(self):
        raw=bytes(range(256));self.term.send(self.spec(raw))
        reply=self.term.read('12345678',0)
        self.assertEqual(b''.join(bytes.fromhex(c['hex']) for c in reply['chunks']),raw)
        self.assertEqual(self.term.read('12345678',0),reply)
    def test_cannot_switch_channel_without_close(self):
        with self.assertRaises(ApiError):self.term.configure(dict(self.config,session='12345679',channel='h1'))
    def test_invalid_sends_do_not_touch_uart(self):
        for value in [dict(self.spec(),hex=''),dict(self.spec(),hex='1'),dict(self.spec(),hex='xx'),dict(self.spec(),hex='aa'*1025),dict(self.spec(),idempotency_key='')]:
            with self.assertRaises(ApiError):self.term.send(value)
        self.assertEqual(self.wire.raw,b'')
    def test_closed_terminal_cannot_send(self):
        self.term.close({'session':'12345678'})
        with self.assertRaises(ApiError):self.term.send(self.spec())
    def test_loopback_explicit_only(self):
        self.term.close({'session':'12345678'})
        self.term.configure(dict(self.config,session='12345679',channel='loopback'))
        self.assertTrue(self.wire.commands[-1].endswith(' loop'))
if __name__=='__main__':unittest.main()
