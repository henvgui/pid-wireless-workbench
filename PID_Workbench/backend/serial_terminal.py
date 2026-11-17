"""H1 log receiver and H2 bidirectional wireless UART; one hardware gate."""
import secrets
import time
import datetime
import hashlib


class SerialTerminal:
    def __init__(self, gate, link_factory, resolve_port, error):
        self.gate, self.link_factory, self.resolve_port, self.error = gate, link_factory, resolve_port, error
        self.session = None
        self.config = None
        self.closed = True
        self.last_error = None
        self.last_read_at = None
        self.received_bytes = 0
        self.cache_cursor = None
        self.cached_read = None
        self.epoch = self.tx_total = 0
        self.operations = {}
        self.pending_send = None

    def status(self):
        return dict(session=self.session, config=self.config, closed=self.closed,
                    last_error=self.last_error, last_read_at=self.last_read_at, received_bytes=self.received_bytes,
                    epoch=self.epoch, tx_total=self.tx_total, pending_send=self.pending_send)

    def validate(self, value):
        if not isinstance(value, dict) or set(value) - {"session", "port", "baud", "bits", "parity", "stop", "channel"}:
            raise self.error("invalid_serial_config", "串口配置字段无效。")
        config = dict(baud=value.get("baud", 115200), bits=value.get("bits", 8),
                      parity=value.get("parity", "N"), stop=value.get("stop", 1), channel=value.get("channel", "h1"))
        if config["channel"] not in ("h1", "h2", "loopback"):
            raise self.error("invalid_serial_config", "channel 必须为 h1、h2 或 loopback。")
        for key, low, high in (("baud", 1200, 921600), ("bits", 5, 8), ("stop", 1, 2)):
            number = config[key]
            if type(number) is not int or not low <= number <= high:
                raise self.error("invalid_serial_config", f"{key} 范围为 {low}～{high}。")
        if config["parity"] not in ("N", "O", "E"):
            raise self.error("invalid_serial_config", "校验位必须为 N、O 或 E。")
        config["port"] = self.resolve_port(value.get("port", "auto"))
        session = value["session"] if "session" in value else secrets.token_hex(4)
        if not isinstance(session, str) or len(session) != 8 or any(c not in "0123456789abcdef" for c in session) or int(session, 16) == 0:
            raise self.error("invalid_serial_session", "session 必须为非零的 8 位小写十六进制数。")
        return session, config

    def acquire(self):
        if not self.gate.acquire(False):
            raise self.error("device_busy", "烧录或其他硬件操作进行中，日志轮询暂缓。", 409)

    def query(self, link, command, session, op, cursor=None):
        def accepts(reply):
            if reply.get("id") != int(session, 16) or reply.get("op") != op:
                return False
            return cursor is None or not reply.get("ok") or ((reply.get("offset", 0)-cursor) & 0xffffffff) < 0x80000000
        # Read/config/close are idempotent at a fixed session/cursor. The slave
        # retains bytes until acknowledged; no destructive retry is involved.
        value = link.query(command, "serial_stream" if op >= 21 else "serial_log", accepts, timeout=5)
        if not value.get("ok"):
            failure = self.error("serial_remote_error", "从机日志接口：" + str(value.get("error")), 503)
            failure.remote_error = value.get("error")
            raise failure
        return value

    def wire(self):
        return ("!log", 17, 108) if self.config["channel"] == "h1" else ("!serial", 21, 96)

    def check_epoch(self, value):
        if self.config["channel"] != "h1" and value.get("epoch") != self.epoch:
            raise self.error("serial_session_expired", "从机已重启或串口会话已改变，请重新连接；旧发送不自动重放。", 409)

    def configure(self, value):
        session, config = self.validate(value)
        self.acquire()
        link = None
        try:
            if session == self.session and config != self.config:
                raise self.error("serial_session_conflict", "修改配置请开始一个新日志会话。", 409)
            if not self.closed and self.config and (config["channel"], config["port"]) != (self.config["channel"], self.config["port"]):
                raise self.error("serial_session_conflict", "切换通道或主机端口前请先关闭当前会话。", 409)
            link = self.link_factory(None if config["port"] == "auto" else config["port"])
            parity = {"N": 0, "O": 1, "E": 2}[config["parity"]]
            h1 = config["channel"] == "h1"
            prefix, op = ("!log", 17) if h1 else ("!serial", 21)
            result = self.query(link, f"{prefix} open {session} {config['baud']} {config['bits']} {parity} {config['stop']}" + (" loop" if config["channel"] == "loopback" else ""), session, op)
            epoch = 0 if h1 else result.get("epoch", 0)
            if not h1 and (type(epoch) is not int or not 0 < epoch <= 0xffffffff):
                raise self.error("serial_protocol", "从机未返回有效的发送会话编号。", 503)
            if session == self.session and not self.closed and epoch != self.epoch:
                raise self.error("serial_session_expired", "从机发送会话已改变，请使用新 session 重新连接。", 409)
            if session != self.session or self.closed:
                self.cache_cursor = self.cached_read = None
                self.received_bytes = 0
                self.last_read_at = None
                self.operations = {}
                self.pending_send = None
            self.epoch, self.tx_total = epoch, result.get("tx_total", 0)
            self.session, self.config = session, config
            self.closed = False
            self.last_error = None
            return dict(session=session, config=config, cursor=0, rx_pin=44 if h1 else 18, tx_pin=None if h1 else 17, tx_enabled=not h1,
                        buffer_bytes=32768, slave=result.get("peer_mac"), status=result)
        finally:
            if link is not None:
                link.close()
            self.gate.release()

    def check(self, session):
        if session != self.session or self.config is None:
            raise self.error("serial_session_expired", "日志会话已改变或服务已重启，请重新开始接收。", 409)

    def read(self, session, cursor):
        if type(cursor) is not int or not 0 <= cursor <= 0xffffffff:
            raise self.error("invalid_serial_cursor", "cursor 必须是 0～4294967295。")
        self.acquire()
        link = None
        try:
            self.check(session)
            if self.closed:
                raise self.error("serial_closed", "日志接收已被关闭。", 409)
            if self.cached_read is not None:
                if cursor == self.cache_cursor:
                    return self.cached_read
                if cursor != self.cached_read["next_cursor"]:
                    raise self.error("serial_cursor_conflict", "读取游标不连续，请使用上次返回的 next_cursor。", 409)
                self.cache_cursor = self.cached_read = None
            link = self.link_factory(None if self.config["port"] == "auto" else self.config["port"])
            prefix, op, limit = self.wire()
            chunks = []
            requested = cursor
            end = time.monotonic() + .15
            dropped = errors = 0
            read_failed = False
            for _ in range(16):
                try:
                    value = self.query(link, f"{prefix} read {session} {cursor}", session, op+1, cursor)
                    self.check_epoch(value)
                except Exception as error:
                    self.last_error = str(error)
                    read_failed = True
                    if chunks:
                        break  # Deliver already-acknowledged bytes before reporting the next error.
                    raise
                raw = bytes.fromhex(value["hex"])
                if len(raw) > limit:
                    raise self.error("serial_protocol", "从机日志块超出协议长度。", 503)
                actual = value["offset"]
                gap = (actual-cursor) & 0xffffffff
                if raw or gap:
                    chunks.append(dict(offset=actual, gap=gap, hex=raw.hex()))
                    self.received_bytes += len(raw)
                cursor = (actual+len(raw)) & 0xffffffff
                dropped, errors = value["dropped"], value["rx_errors"]
                if len(raw) < limit or time.monotonic() >= end:
                    break
            result = dict(session=session, next_cursor=cursor, chunks=chunks, dropped=dropped,
                          rx_errors=errors, paused=value.get("paused", False), pause_events=value.get("pause_events", 0),
                          tx_total=self.tx_total, channel=self.config["channel"], slave=value.get("peer_mac"))
            self.last_read_at = datetime.datetime.now(datetime.timezone.utc).isoformat()
            if not read_failed:
                self.last_error = None
            if chunks:
                # Batched radio reads acknowledge earlier blocks. Retain the
                # entire HTTP response until the PC advances its cursor too.
                self.cache_cursor, self.cached_read = requested, result
            return result
        finally:
            if link is not None:
                link.close()
            self.gate.release()

    def close(self, value):
        if not isinstance(value, dict) or set(value) != {"session"}:
            raise self.error("invalid_serial_session", "关闭接收需要 session。")
        self.acquire()
        link = None
        try:
            session = value["session"]
            self.check(session)
            link = self.link_factory(None if self.config["port"] == "auto" else self.config["port"])
            prefix, op, _ = self.wire()
            self.query(link, f"{prefix} close {session}", session, 19 if op == 17 else 24)
            self.closed = True
            # Retain the session so a lost close reply can safely be retried.
            return dict(session=session, closed=True)
        finally:
            if link is not None:
                link.close()
            self.gate.release()

    def send(self, value):
        if not isinstance(value, dict) or set(value) != {"session", "idempotency_key", "hex"}:
            raise self.error("invalid_serial_send", "发送需要 session、idempotency_key、hex。")
        key, text = value["idempotency_key"], value["hex"]
        if not isinstance(key, str) or not 1 <= len(key) <= 128:
            raise self.error("invalid_serial_send", "idempotency_key 长度必须为 1～128。")
        if not isinstance(text, str) or not 2 <= len(text) <= 2048 or len(text)%2 or any(c not in "0123456789abcdefABCDEF" for c in text):
            raise self.error("invalid_serial_send", "hex 必须为 1～1024 字节的十六进制数据。")
        raw = bytes.fromhex(text)
        digest = hashlib.sha256(raw).hexdigest()
        self.acquire()
        link = None
        try:
            self.check(value["session"])
            if self.closed or self.config["channel"] == "h1":
                raise self.error("serial_send_disabled", "请先连接 H2 双向串口；H1 仅接收日志。", 409)
            operation = self.operations.get(key)
            if operation:
                if operation["sha256"] != digest:
                    raise self.error("idempotency_conflict", "同一发送编号不能对应不同数据。", 409)
                if operation.get("result"):
                    return operation["result"]
            else:
                if self.pending_send:
                    raise self.error("serial_send_pending", "上一次发送结果未确认，请以原编号重试或重新连接后由目标应用确认。", 409)
                if len(self.operations) >= 4096:
                    raise self.error("serial_send_limit", "本会话已达 4096 次发送，请重新连接。", 409)
                operation = dict(sha256=digest, base=self.tx_total, confirmed=0)
                self.operations[key] = operation
            self.pending_send = key
            link = self.link_factory(None if self.config["port"] == "auto" else self.config["port"])
            while operation["confirmed"] < len(raw):
                done = operation["confirmed"]
                block = raw[done:done+128]
                offset = operation["base"]+done
                result = self.query(link, f"!serial write {self.session} {offset} {self.epoch:08x} {block.hex()}", self.session, 23)
                self.check_epoch(result)
                if result.get("tx_total") != offset+len(block):
                    raise self.error("serial_protocol", "UART 发送确认长度不匹配；停止后续发送。", 503)
                operation["confirmed"] += len(block)
                self.tx_total = result["tx_total"]
            answer = dict(session=self.session, idempotency_key=key, accepted=len(raw), tx_total=self.tx_total,
                          sha256=digest, delivery="slave_uart_queued")
            operation["result"] = answer
            self.pending_send = None
            return answer
        finally:
            if link is not None:
                link.close()
            self.gate.release()
