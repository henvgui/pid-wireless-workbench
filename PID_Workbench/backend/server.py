"""Loopback-only authenticated PID Workbench API. GUI and AI share one engine."""
import argparse
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import sys
from urllib.parse import parse_qs, urlsplit

from workbench_core import ApiError, DATA_DIR, Engine, VERSION, capabilities, prepare


def token(root=DATA_DIR):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    path = root / "api_token.txt"
    try:
        with path.open("x", encoding="ascii") as file:
            file.write(secrets.token_hex(32))
    except FileExistsError:
        pass
    value = path.read_text(encoding="ascii").strip()
    if not re.fullmatch("[a-f0-9]{64}", value):
        raise RuntimeError("API token file is invalid")
    return value


def openapi():
    properties = {"action": {"type": "string", "enum": capabilities()["actions"]},
                  "idempotency_key": {"type": "string", "minLength": 1, "maxLength": 120},
                  "chip": {"type": "string", "enum": [p["id"] for p in capabilities()["chips"]]},
                  "transport": {"type": "string", "const": "esp_now", "default": "esp_now", "description": "PC -> host USB -> ESP-NOW -> slave -> target interface -> target MCU"},
                  "target_interface": {"type": "string", "enum": ["uart", "swd"], "description": "Slave-side target interface. F407 defaults to SWD; others default to UART. Check chips.interfaces; SWD supports F407VGT6/VET6 with current host/slave firmware. VET6 has passed physical validation; VGT6 remains pending."},
                  "method": {"type": "string", "enum": [p["id"] for p in capabilities()["methods"]], "description": "Compatibility alias for the slave target interface; must agree with target_interface when both are supplied."},
                  "firmware": {"type": "string", "description": "Absolute local .bin path, linked at 0x08000000"},
                  "expected_sha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
                  "port": {"type": "string", "default": "auto", "description": "ESP32 host USB COM port for every target interface. Slave USB and unrelated adapters are rejected."},
                  "run": {"type": "boolean", "default": True},
                  "swd_legacy": {"type": "boolean", "default": False, "description": "F407 SWD only: manually use word programming instead of the 1 KiB SRAM algorithm. Requires updated host/slave; no automatic retry after an ambiguous Flash operation."},
                  "text": {"type": "string", "maxLength": 16384},
                  "file": {"type": "string"}, "bytes": {"type": "integer", "minimum": 1, "maximum": 4194304},
                  "parameters": {"type": "object", "properties": {p: {"type": "number"} for p in ("p", "i", "d", "step")},
                                 "required": ["p", "i", "d", "step"], "additionalProperties": False}}
    response = {"description": "JSON envelope: ok/data on success; ok/error with code/message on failure"}
    paths = {}
    for path, operation in [("/v1/capabilities", "capabilities"), ("/v1/ports", "listPorts"),
                            ("/v1/device", "readDeviceState"), ("/v1/jobs", "listJobs"), ("/v1/jobs/{id}", "getJob")]:
        paths[path] = {"get": {"operationId": operation, "responses": {"200": response}}}
    schema = {"type": "object", "properties": properties, "required": ["action", "idempotency_key"], "additionalProperties": False,
              "allOf": [{"if": {"properties": {"action": {"const": "flash"}}}, "then": {"required": ["chip", "firmware"]}}]}
    paths["/v1/jobs"]["post"] = {"operationId": "startJob", "description": "One active job. Reuse idempotency_key only for the identical request; completed/failed jobs are returned without rerunning.",
        "requestBody": {"required": True, "content": {"application/json": {"schema": schema}}}, "responses": {"202": response, "400": response, "409": response, "501": response}}
    paths["/v1/jobs/{id}"]["get"]["parameters"] = [{"name": "id", "in": "path", "required": True, "schema": {"type": "string", "pattern": "^[0-9a-f]{24}$"}}]
    paths["/v1/device"]["get"]["parameters"] = [{"name": "port", "in": "query", "schema": {"type": "string", "default": "auto"}}]
    paths["/v1/plan"] = {"post": {"operationId": "validateFirmwarePlan", "description": "Validate local firmware, vectors and SHA256 without accessing or writing the target.",
        "requestBody": {"required": True, "content": {"application/json": {"schema": dict(schema, required=["action", "chip", "firmware"])}}}, "responses": {"200": response, "400": response, "501": response}}}
    serial_schema = {"type":"object", "additionalProperties":False, "required":["session"], "properties":{
        "session":{"type":"string","pattern":"^(?!00000000)[0-9a-f]{8}$"},
        "port":{"type":"string","default":"auto"}, "baud":{"type":"integer","minimum":1200,"maximum":921600,"default":115200},
        "bits":{"type":"integer","minimum":5,"maximum":8,"default":8},
        "parity":{"type":"string","enum":["N","O","E"],"default":"N"},
        "stop":{"type":"integer","enum":[1,2],"default":1},
        "channel":{"type":"string","enum":["h1","h2","loopback"],"default":"h1"}}}
    for endpoint,operation,body in [("configure","configureSerialLog",serial_schema),
            ("close","closeSerialLog",dict(serial_schema,properties={"session":serial_schema["properties"]["session"]}))]:
        paths["/v1/serial/"+endpoint] = {"post":{"operationId":operation,
            "description":"Wireless UART: H2 TX17/RX18 bidirectional, H1 RX44 logs, or internal loopback with target isolated. Shares flash hardware gate.",
            "requestBody":{"required":True,"content":{"application/json":{"schema":body}}},"responses":{"200":response,"409":response}}}
    paths["/v1/serial/read"]={"get":{"operationId":"readSerialLog","description":"Returns raw hex chunks, next_cursor, explicit gaps, dropped bytes and UART error events. Reuse the same cursor after a lost response; never silently advance it.",
        "parameters":[{"name":"session","in":"query","required":True,"schema":serial_schema["properties"]["session"]},
                      {"name":"cursor","in":"query","required":True,"schema":{"type":"integer","minimum":0,"maximum":4294967295}}],
        "responses":{"200":response,"409":response}}}
    paths["/v1/serial/status"]={"get":{"operationId":"serialLogStatus",
        "description":"Local receiver session/config, closed flag, last error/read time and byte count. Does not access hardware. Not proof of current target UART output.",
        "responses":{"200":response}}}
    paths["/v1/serial/send"]={"post":{"operationId":"sendWirelessSerial",
        "description":"H2 only. 1..1024 raw bytes as hex. Keep the same session, key and payload when retrying; never retry uncertain data with a new key. ACK means queued in slave UART, not processed by target. Idempotency lasts for the current live backend/slave session; restart requires application-level reconciliation.",
        "requestBody":{"required":True,"content":{"application/json":{"schema":{"type":"object","additionalProperties":False,
            "required":["session","idempotency_key","hex"],"properties":{"session":serial_schema["properties"]["session"],
            "idempotency_key":{"type":"string","minLength":1,"maxLength":128},"hex":{"type":"string","minLength":2,"maxLength":2048,"pattern":"^(?:[0-9a-fA-F]{2})+$"}}}}}},
        "responses":{"200":response,"409":response}}}
    return {"openapi": "3.1.0", "info": {"title": "PID Workbench local hardware API", "version": VERSION,
            "description": "Hardware-specific workbench: PC -> ESP32 host USB -> ESP-NOW -> ESP32 slave -> UART / SWD -> target MCU. SWD supports F407VGT6/VET6; VET6 physical validation passed, VGT6 pending. No computer-side ST-Link route."},
            "servers": [{"url": "http://127.0.0.1:18765"}], "security": [{"LocalToken": []}],
            "components": {"securitySchemes": {"LocalToken": {"type": "http", "scheme": "bearer"}}}, "paths": paths}


class ApiServer(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address, engine, api_token):
        self.engine, self.api_token = engine, api_token
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    server_version = "PIDWorkbench/1"
    def log_message(self, format, *args):
        pass  # Do not accidentally log bearer tokens or firmware contents.

    def reply(self, status, value):
        data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def execute(self, post=False):
        try:
            url = urlsplit(self.path)
            if url.path == "/health" and not post:
                self.reply(200, {"app": "pid-workbench", "version": VERSION, "pid": os.getpid(), "api_version": "v1"})
                return
            if self.headers.get("Origin"):
                raise ApiError("origin_not_allowed", "仅允许本机应用调用此接口。", 403)
            auth = self.headers.get("Authorization", "")
            expected = "Bearer " + self.server.api_token
            if not hmac.compare_digest(auth.encode("utf-8"), expected.encode("ascii")):
                raise ApiError("unauthorized", "需要本机 API token。", 401)
            engine = self.server.engine
            if post:
                if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                    raise ApiError("content_type", "请求必须使用 application/json。", 415)
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError as error:
                    raise ApiError("invalid_json", "Content-Length 无效。") from error
                if not 1 <= length <= 65536:
                    raise ApiError("request_too_large", "JSON 请求大小应为 1..65536 字节。", 413)
                try:
                    def bad_constant(value):
                        raise ValueError(value)
                    spec = json.loads(self.rfile.read(length), parse_constant=bad_constant)
                except (ValueError, UnicodeDecodeError) as error:
                    raise ApiError("invalid_json", "JSON 格式无效。") from error
                if url.path == "/v1/plan":
                    if not isinstance(spec, dict) or spec.get("action") != "flash":
                        raise ApiError("invalid_request", "plan 需要 action:flash。")
                    result, _ = prepare(spec)
                    self.reply(200, {"ok": True, "data": result})
                elif url.path == "/v1/serial/configure":
                    self.reply(200, {"ok": True, "data": engine.serial_log.configure(spec)})
                elif url.path == "/v1/serial/close":
                    self.reply(200, {"ok": True, "data": engine.serial_log.close(spec)})
                elif url.path == "/v1/serial/send":
                    self.reply(200, {"ok": True, "data": engine.serial_log.send(spec)})
                elif url.path == "/v1/jobs":
                    self.reply(202, {"ok": True, "data": engine.submit(spec)})
                else:
                    raise ApiError("not_found", "接口不存在。", 404)
            else:
                if url.path == "/v1/capabilities": result = capabilities()
                elif url.path == "/v1/ports": result = engine.list_ports()
                elif url.path == "/v1/device": result = engine.device(parse_qs(url.query).get("port", ["auto"])[0])
                elif url.path == "/v1/serial/status": result = engine.serial_log.status()
                elif url.path == "/v1/serial/read":
                    query = parse_qs(url.query)
                    cursor = query.get("cursor", ["0"])[0]
                    if not cursor.isascii() or not cursor.isdigit() or len(cursor) > 10:
                        raise ApiError("invalid_serial_cursor", "cursor 必须是非负整数。")
                    result = engine.serial_log.read(query.get("session", [""])[0], int(cursor))
                elif url.path == "/v1/openapi.json": result = openapi()
                elif url.path == "/v1/jobs": result = [engine.store.public(j, include_log=False) for j in engine.store.list()]
                elif re.fullmatch(r"/v1/jobs/[0-9a-f]{24}", url.path): result = engine.store.public(engine.store.get(url.path.rsplit("/", 1)[1]))
                else: raise ApiError("not_found", "接口不存在。", 404)
                self.reply(200, {"ok": True, "data": result})
        except ApiError as error:
            detail = {"code": error.code, "message": str(error)}
            if hasattr(error, "remote_error"): detail["remote_error"] = error.remote_error
            self.reply(error.status, {"ok": False, "error": detail})
        except Exception as error:
            self.reply(503, {"ok": False, "error": {"code": "device_or_service_unavailable", "message": str(error)}})

    def do_GET(self): self.execute()
    def do_POST(self): self.execute(True)
    def do_OPTIONS(self): self.reply(403, {"ok": False, "error": {"code": "origin_not_allowed", "message": "Cross-origin calls disabled"}})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765)
    args = parser.parse_args()
    server = ApiServer(("127.0.0.1", args.port), Engine(), token())
    server.serve_forever(poll_interval=.3)


if __name__ == "__main__":
    main()
