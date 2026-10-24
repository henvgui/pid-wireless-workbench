"""PID Workbench AI CLI: stdout is JSON; all writes use the shared local job API."""
import argparse
import json
from pathlib import Path
import secrets
import sys
import time
from urllib.parse import quote

from api_client import ensure_server, request
from workbench_core import ApiError


class JsonArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise ApiError("invalid_arguments", message)


def flash_options(parser):
    parser.add_argument("--chip", required=True)
    parser.add_argument("--interface", dest="target_interface", choices=["uart", "swd"], help="从机到目标板的接口；F407 默认 SWD，其他默认 UART")
    parser.add_argument("--method", choices=["wireless_uart", "wireless_swd"], help="兼容参数；推荐使用 --interface")
    parser.add_argument("--firmware", required=True, type=lambda p: str(Path(p).resolve()))
    parser.add_argument("--port", default="auto", help="ESP32 主机 USB 端口，默认自动识别")
    parser.add_argument("--expected-sha256")
    parser.add_argument("--no-run", action="store_true")


def job_options(parser):
    parser.add_argument("--idempotency-key")
    parser.add_argument("--wait", action="store_true")


def wait(identifier, timeout=1800):
    deadline = time.monotonic()+timeout
    while True:
        result = request("/v1/jobs/"+identifier)
        if result["data"]["state"] not in ("queued", "running"):
            return result
        if time.monotonic() > deadline:
            return {"ok": False, "error": {"code": "wait_timeout", "message": "等待超时；任务仍由后台管理，没有取消或重烧。"}, "data": result["data"]}
        time.sleep(.35)


def main():
    parser = JsonArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("capabilities", "ports", "jobs", "schema", "service"):
        commands.add_parser(name)
    device = commands.add_parser("device"); device.add_argument("--port", default="auto")
    plan = commands.add_parser("plan"); flash_options(plan)
    flash = commands.add_parser("flash"); flash_options(flash); job_options(flash)
    echo = commands.add_parser("echo"); job_options(echo); echo.add_argument("--port", default="auto")
    sources = echo.add_mutually_exclusive_group(required=True)
    sources.add_argument("--text"); sources.add_argument("--file", type=lambda p: str(Path(p).resolve())); sources.add_argument("--bytes", type=int)
    pid = commands.add_parser("pid"); job_options(pid); pid.add_argument("--port", default="auto")
    for name in ("p", "i", "d", "step"): pid.add_argument("--"+name, required=True)
    for name in ("run", "stop", "save", "load"):
        item = commands.add_parser(name); job_options(item); item.add_argument("--port", default="auto")
    item = commands.add_parser("submit"); item.add_argument("--request", type=Path, required=True); item.add_argument("--wait", action="store_true")
    item = commands.add_parser("job"); item.add_argument("id")
    item = commands.add_parser("wait"); item.add_argument("id"); item.add_argument("--timeout", type=float, default=1800)
    terminal = commands.add_parser("serial", help="H2 双向无线串口 / H1 日志")
    terminal.add_argument("operation", choices=["configure", "read", "send", "close", "status"])
    terminal.add_argument("--session", help="非零 8 位小写十六进制会话 ID，同一操作重试保持一致；status 不需要")
    terminal.add_argument("--port", default="auto")
    terminal.add_argument("--baud", type=int, default=115200)
    terminal.add_argument("--bits", type=int, choices=[5,6,7,8], default=8)
    terminal.add_argument("--parity", choices=["N","O","E"], default="N")
    terminal.add_argument("--stop", type=int, choices=[1,2], default=1)
    terminal.add_argument("--cursor", type=int, default=0)
    terminal.add_argument("--channel", choices=["h1","h2","loopback"], default="h1")
    terminal.add_argument("--text")
    terminal.add_argument("--hex")
    terminal.add_argument("--ending", choices=["none","cr","lf","crlf"], default="none")
    terminal.add_argument("--encoding", choices=["utf-8","gb18030","ascii"], default="utf-8")
    terminal.add_argument("--idempotency-key")
    args = parser.parse_args()
    if args.command in ("capabilities", "ports", "jobs", "schema"):
        return request("/v1/"+{"schema": "openapi.json"}.get(args.command, args.command))
    if args.command == "service":
        health = ensure_server()
        resolved = []
        request("/v1/capabilities", start=False, credential_paths=resolved)
        return {"ok": True, "data": dict(health, credential_path=resolved[0])}
    if args.command == "serial":
        if args.operation == "status": return request("/v1/serial/status")
        if not args.session: raise ApiError("invalid_arguments", "serial configure/read/close 需要 --session。")
        if args.operation == "configure":
            return request("/v1/serial/configure", dict(session=args.session, port=args.port, baud=args.baud,
                                                       bits=args.bits, parity=args.parity, stop=args.stop, channel=args.channel))
        if args.operation == "send":
            if (args.text is None) == (args.hex is None) or not args.idempotency_key:
                raise ApiError("invalid_arguments", "serial send 需要 --text 或 --hex，以及固定的 --idempotency-key。")
            payload = (args.text+{"none":"","cr":"\r","lf":"\n","crlf":"\r\n"}[args.ending]).encode(args.encoding).hex() if args.text is not None else args.hex
            return request("/v1/serial/send", dict(session=args.session, idempotency_key=args.idempotency_key, hex=payload))
        if args.operation == "close": return request("/v1/serial/close", {"session":args.session})
        return request("/v1/serial/read?session="+quote(args.session)+"&cursor="+str(args.cursor))
    if args.command == "device": return request("/v1/device?port="+quote(args.port))
    if args.command == "job": return request("/v1/jobs/"+args.id)
    if args.command == "wait": return wait(args.id, args.timeout)
    if args.command == "submit":
        spec = json.loads(args.request.read_text(encoding="utf-8-sig"))
    else:
        spec = {"action": "flash" if args.command == "plan" else args.command, "port": args.port}
        if args.command in ("plan", "flash"):
            spec.update(chip=args.chip, transport="esp_now", firmware=args.firmware, run=not args.no_run)
            if args.method: spec["method"] = args.method
            if args.target_interface: spec["target_interface"] = args.target_interface
            if args.expected_sha256: spec["expected_sha256"] = args.expected_sha256
        elif args.command == "echo":
            if args.text is not None: spec["text"] = args.text
            elif args.file: spec["file"] = args.file
            else: spec["bytes"] = args.bytes
        elif args.command == "pid": spec["parameters"] = {p: getattr(args, p) for p in ("p", "i", "d", "step")}
        spec["idempotency_key"] = getattr(args, "idempotency_key", None) or "cli-"+secrets.token_hex(12)
    if args.command == "plan": return request("/v1/plan", spec)
    result = request("/v1/jobs", spec)
    return wait(result["data"]["id"]) if args.wait else result


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        result = main()
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        data = result.get("data", {})
        failed = isinstance(data, dict) and data.get("state") in ("failed", "interrupted")
        sys.exit(0 if result.get("ok") and not failed else 1)
    except Exception as error:
        detail = {"code": getattr(error, "code", "client_error"), "message": str(error)}
        if hasattr(error,"remote_error"): detail["remote_error"] = error.remote_error
        print(json.dumps({"ok": False, "error": detail}, ensure_ascii=False))
        sys.exit(1)
