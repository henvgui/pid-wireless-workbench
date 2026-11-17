import contextlib
import datetime
import decimal
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import subprocess
import sys
import threading
import time

APP_DIR = Path(__file__).resolve().parents[1]
OUTPUTS = APP_DIR.parent
TOOLS = APP_DIR / "backend/protocol"
if not (TOOLS / "flash_stm32.py").is_file():
    TOOLS = OUTPUTS / "PID_Shared/PIDFlash/tools"
sys.path.insert(0, str(TOOLS))
from flash_stm32 import Link, discover_hosts, target_profile, usb_role, validate_image
from serial.tools import list_ports
from serial_terminal import SerialTerminal

VERSION = "2026.11.17"
DATA_DIR = Path(os.environ.get("PID_WORKBENCH_DATA", str(Path(os.environ.get("LOCALAPPDATA", APP_DIR)) / "PIDWorkbench")))
CHIPS = [{"id": name, "label": name, "flash_kb": kb, "profile": profile,
          "device_id": ident, "interfaces": ["uart","swd"] if profile.startswith("f407") else ["uart"],
          "default_interface": "swd" if profile.startswith("f407") else "uart",
          "physical_validation": "wireless_passed" if name == "STM32F103C8T6" else "wireless_swd_passed" if name == "STM32F407VET6" else "pending"}
         for name, kb, profile, ident in [
             ("STM32F103C8T6", 64, "f103c8", 0x410),
             ("STM32G431CBT6", 128, "g431", 0x468),
             ("STM32G431C8T6", 64, "g431", 0x468),
             ("STM32G431C6T6", 32, "g431", 0x468),
             ("STM32G431KBU6", 128, "g431", 0x468),
             ("STM32F407VGT6", 1024, "f407vg", 0x413),
             ("STM32F407VET6", 512, "f407ve", 0x413)]]
METHODS = [{"id": "wireless_uart", "label": "UART · STM32 ROM Bootloader", "transport": "esp_now",
            "target_interface": "uart", "available": True, "status": "implemented", "validation": "F103 实板通过；G431/F407 待实板验收"},
           {"id": "wireless_swd", "label": "SWD · 从机 → STM32F407", "transport": "esp_now",
            "target_interface": "swd", "available": True, "status": "implemented",
            "supported_chips": ["STM32F407VGT6", "STM32F407VET6"], "requires_slave_firmware_update": True,
            "validation": "F407VET6 已实板写入、完整读回及启动；VGT6 待实板验收"}]
HARDWARE_ROUTE = ["pc", "host_usb", "esp_now", "slave", "target_interface", "target_mcu"]


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class ApiError(RuntimeError):
    def __init__(self, code, message, status=400):
        self.code, self.status = code, status
        super().__init__(message)


def ports(devices=None):
    if devices is None:
        devices = list_ports.comports()
    return [{"port": port.device, "description": port.description,
             "serial_number": port.serial_number, "role": usb_role(port)} for port in devices]


def capabilities():
    return {"version": VERSION, "chips": CHIPS,
            "transport": "esp_now", "hardware_route": HARDWARE_ROUTE,
            "methods": [dict(item) for item in METHODS],
            "interface_extension": "新接口需同时实现从机驱动、目标芯片算法及端到端协议后才可启用。",
            "actions": ["flash", "echo", "pid", "run", "stop", "save", "load"],
            "api_version": "v1", "base_address": "0x08000000",
            "formats": ["bin"], "job_policy": "single_active_job; persistent idempotency; no automatic erase replay",
            "target_pid_protocol_implemented": False,
            "firmware_transfer": {"negotiated": True,"radio_chunk_bytes":192,"window_packets":8,
                                  "usb_batch_bytes":3072,"host_cache_bytes":6144,"fallback":"legacy_stop_wait",
                                  "integrity":"packet_crc32 + staged_sha256 + full_target_readback"},
            "swd_programmer": {"default":"ram_1k","block_bytes":1024,"chips":["STM32F407VET6","STM32F407VGT6"],
                               "requires_updated_firmware":True,"manual_fallback":"swd_legacy=true","full_readback":True},
            "serial_log": {"available": True, "rx_pin": 44, "interface": "H1 UART0", "direction": "receive_only", "buffer_bytes": 32768},
            "wireless_serial": {"channels": ["h2", "h1", "loopback"], "tx_pin": 17, "rx_pin": 18,
                                "max_send_bytes": 1024, "idempotency_required": True, "flash_pause_resume": True}}


def require_string(value, name, limit=32768):
    if not isinstance(value, str) or not value or len(value) > limit or "\x00" in value:
        raise ApiError("invalid_request", f"{name} 必须是非空字符串，长度不超过 {limit}。")
    return value


def resolve_port(value, host=False):
    if value == "auto" and host:
        hosts = [p["port"] for p in ports() if p["role"] == "host"]
        if len(hosts) > 1:
            raise ApiError("multiple_hosts", "检测到多台 ESP32 主机，请选择具体端口。")
        return hosts[0] if hosts else "auto"
    if not isinstance(value, str) or not re.fullmatch(r"COM[1-9][0-9]{0,3}", value.upper()):
        raise ApiError("invalid_port", "请选择具体 COM 串口；无线主机也可以使用 auto。")
    return value.upper()


def host_port(value):
    port = resolve_port(value, host=True)
    if any(p["port"] == port and p["role"] != "host" for p in ports()):
        raise ApiError("wrong_port_role", "请选择 ESP32 主机 USB；所有目标接口均通过主机和 ESP-NOW 访问从机。")
    return port


def flash_method(spec):
    if spec.get("transport", "esp_now") != "esp_now":
        raise ApiError("unsupported_transport", "本工作台统一通过主机 USB → ESP-NOW → 从机访问目标板。")
    requested = spec.get("method")
    if requested is not None and requested not in {m["id"] for m in METHODS}:
        raise ApiError("unsupported_method", "请选择从机的无线 UART 或 SWD 接口；电脑直连烧录不属于此硬件路径。")
    interface = spec.get("target_interface")
    if interface is not None and interface not in {m["target_interface"] for m in METHODS}:
        raise ApiError("unsupported_interface", "此从机目标接口尚未接入。")
    chip = next((c for c in CHIPS if c["id"]==spec.get("chip")),None)
    default = chip["default_interface"] if chip else "uart"
    method = next(m for m in METHODS if m["id"] == requested) if requested else next(
        m for m in METHODS if m["target_interface"] == (interface or default))
    if interface is not None and interface != method["target_interface"]:
        raise ApiError("interface_conflict", "method 与 target_interface 指向不同的从机接口。")
    if not method["available"]:
        raise ApiError("interface_not_implemented", method["reason"], 501)
    if chip and method["target_interface"] not in chip["interfaces"]:
        raise ApiError("unsupported_chip_interface", "此芯片尚未适配从机 "+method["target_interface"].upper()+"；SWD 当前支持 STM32F407VGT6/VET6。")
    return method


def canonical_plan(plan):
    """Keep completed UART job keys valid across the route-schema migration."""
    value = dict(plan)
    if value.get("swd_legacy") is False:
        value.pop("swd_legacy")
    if value.get("method") == "wireless_uart":
        value.setdefault("transport", "esp_now")
        value.setdefault("target_interface", "uart")
        if value.get("probe_serial") == "":
            value.pop("probe_serial")
    return value


def prepare(spec):
    if not isinstance(spec, dict):
        raise ApiError("invalid_request", "任务必须为 JSON 对象。")
    allowed = {"action", "chip", "method", "transport", "target_interface", "firmware", "port", "run", "expected_sha256",
               "idempotency_key", "text", "file", "bytes", "parameters", "swd_legacy"}
    if set(spec) - allowed:
        raise ApiError("unknown_field", "未知字段：" + ", ".join(sorted(set(spec)-allowed)))
    if spec.get("transport", "esp_now") != "esp_now":
        raise ApiError("unsupported_transport", "本工作台统一通过主机 USB → ESP-NOW → 从机访问目标板。")
    action = spec.get("action")
    if action not in capabilities()["actions"]:
        raise ApiError("invalid_action", "不支持此操作。")
    if action != "flash" and any(field in spec for field in ("method", "target_interface")):
        raise ApiError("invalid_request", "target_interface / method 目前用于选择烧录接口；回传固定使用从机 UART。")
    if "swd_legacy" in spec and (action != "flash" or not isinstance(spec["swd_legacy"],bool)):
        raise ApiError("invalid_request", "swd_legacy 必须为布尔值，仅用于 flash。")
    result = {"action": action, "port": spec.get("port", "auto")}
    image = None
    if action == "flash":
        chip = next((p for p in CHIPS if p["id"] == spec.get("chip")), None)
        if chip is None:
            raise ApiError("unsupported_chip", "请选择芯片目录中已适配的型号。")
        method = flash_method(spec)
        if spec.get("swd_legacy"):
            if method["target_interface"] != "swd":
                raise ApiError("invalid_request", "swd_legacy 仅适用于 F407 SWD。")
            result["swd_legacy"] = True
        path = Path(require_string(spec.get("firmware"), "firmware")).expanduser()
        if not path.is_absolute() or not path.is_file() or path.suffix.lower() != ".bin":
            raise ApiError("invalid_firmware", "请选择绝对路径的 .bin 文件，链接地址为 0x08000000。")
        if path.stat().st_size > chip["flash_kb"]*1024:
            raise ApiError("image_too_large", "镜像超出所选芯片的标称 Flash 容量。")
        image = path.read_bytes()
        try:
            validate_image(image, chip["flash_kb"], target_profile(chip["id"]))
        except ValueError as error:
            raise ApiError("invalid_vectors", str(error)) from error
        digest = hashlib.sha256(image).hexdigest()
        if spec.get("expected_sha256") is not None and spec["expected_sha256"] != digest:
            raise ApiError("image_changed", "固件 SHA256 与请求指定值不一致。")
        run = spec.get("run", True)
        if not isinstance(run, bool):
            raise ApiError("invalid_request", "run 必须是布尔值。")
        result.update(chip=chip["id"], method=method["id"], transport=method["transport"],
                      target_interface=method["target_interface"], firmware=str(path.resolve()), port=host_port(result["port"]),
                      run=run, sha256=digest, size=len(image), flash_kb=chip["flash_kb"], address="0x08000000")
    elif action == "echo":
        result["port"] = host_port(result["port"])
        sources = sum(key in spec for key in ("text", "file", "bytes"))
        if sources != 1:
            raise ApiError("invalid_echo_source", "text、file、bytes 必须且只能选择一种。")
        if "text" in spec:
            text = require_string(spec["text"], "text", 16384)
            result.update(text=text, size=len(text.encode("utf-8")))
        elif "file" in spec:
            path = Path(require_string(spec["file"], "file"))
            if not path.is_absolute() or not path.is_file() or not 1 <= path.stat().st_size <= 4*1024*1024:
                raise ApiError("invalid_echo_file", "回传文件需为绝对路径，大小 1 字节到 4 MiB。")
            image = path.read_bytes()
            result.update(file=str(path.resolve()), size=len(image), sha256=hashlib.sha256(image).hexdigest())
        else:
            size = spec["bytes"]
            if isinstance(size, bool) or not isinstance(size, int) or not 1 <= size <= 4*1024*1024:
                raise ApiError("invalid_echo_size", "生成文本大小应为 1 到 4194304 字节。")
            result.update(bytes=size, size=size)
    else:
        result["port"] = host_port(result["port"])
        if action == "pid":
            params = spec.get("parameters")
            if not isinstance(params, dict) or set(params) != {"p", "i", "d", "step"}:
                raise ApiError("invalid_parameters", "参数需要 p、i、d、step 四项。")
            cleaned = {}
            for key, value in params.items():
                if isinstance(value, bool) or not isinstance(value, (str, float, int)):
                    raise ApiError("invalid_parameters", f"{key} 不是有效数值。")
                try:
                    number = decimal.Decimal(str(value))
                except decimal.InvalidOperation as error:
                    raise ApiError("invalid_parameters", f"{key} 不是有效数值。") from error
                if not number.is_finite() or number < 0 or number > decimal.Decimal("9999.99") or number != number.quantize(decimal.Decimal(".01")):
                    raise ApiError("invalid_parameters", f"{key} 范围 0..9999.99，最多两位小数。")
                if key == "step" and number not in [decimal.Decimal(v) for v in (".01", ".1", "1", "10")]:
                    raise ApiError("invalid_parameters", "STEP 只能为 0.01、0.1、1、10。")
                cleaned[key] = str(number)
            result["parameters"] = cleaned
    return result, image


class Store:
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.db = self.root / "jobs.sqlite3"
        self.lock = threading.RLock()
        with self.connect() as db:
            db.execute("CREATE TABLE IF NOT EXISTS jobs (id TEXT PRIMARY KEY, key TEXT UNIQUE NOT NULL, fingerprint TEXT NOT NULL, body TEXT NOT NULL)")
        for job in self.list():
            if job["state"] in ("queued", "running"):
                self.update(job["id"], state="interrupted", error={"code": "service_restarted", "message": "后台服务曾中断；目标状态不确定。未自动重新烧录。"}, completed_at=now())

    @contextlib.contextmanager
    def connect(self):
        connection = sqlite3.connect(self.db)
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def get(self, identifier=None, key=None):
        with self.lock, self.connect() as db:
            row = db.execute("SELECT body FROM jobs WHERE " + ("id=?" if identifier else "key=?"), (identifier or key,)).fetchone()
        if not row:
            raise ApiError("job_not_found", "任务不存在。", 404)
        return json.loads(row[0])

    def list(self):
        with self.lock, self.connect() as db:
            return [json.loads(row[0]) for row in db.execute("SELECT body FROM jobs ORDER BY rowid DESC LIMIT 100")]

    def insert(self, job):
        with self.lock, self.connect() as db:
            db.execute("INSERT INTO jobs VALUES(?,?,?,?)", (job["id"], job["idempotency_key"], job["fingerprint"], json.dumps(job, ensure_ascii=False)))

    def update(self, identifier, **changes):
        with self.lock:
            job = self.get(identifier)
            job.update(changes)
            with self.connect() as db:
                db.execute("UPDATE jobs SET body=? WHERE id=?", (json.dumps(job, ensure_ascii=False), identifier))
        return job

    def public(self, job, include_log=True):
        result = {k: v for k, v in job.items() if k != "fingerprint"}
        path = Path(job["directory"]) / "events.log"
        if include_log and path.exists():
            with path.open("rb") as file:
                file.seek(max(0, path.stat().st_size-50000))
                result["log"] = file.read().decode("utf-8", "replace")
        else:
            result["log"] = ""
        return result


class Engine:
    def __init__(self, root=DATA_DIR):
        self.store = Store(root)
        self.gate = threading.Lock()
        self.serial_log = SerialTerminal(self.gate, Link, host_port, ApiError)
        self.submit_lock = threading.RLock()
        self.cached_state = None

    def list_ports(self):
        devices = list_ports.comports()
        if self.gate.acquire(False):
            try:
                discover_hosts(devices)
            finally:
                self.gate.release()
        return ports(devices)

    def device(self, port="auto"):
        if not self.gate.acquire(False):
            return {"busy": True, "state": self.cached_state, "cached": True}
        try:
            name = host_port(port)
            link = Link(None if name == "auto" else name)
            try:
                state = link.query("!get", "state")
                wireless = link.query("!link", "link")
                self.cached_state = state
                return {"busy": False, "state": state, "link": wireless, "cached": False, "port": link.serial.port}
            finally:
                link.close()
        finally:
            self.gate.release()

    def submit(self, spec):
        key = require_string(spec.get("idempotency_key") if isinstance(spec, dict) else None, "idempotency_key", 120)
        plan, image = prepare(spec)
        fingerprint = hashlib.sha256(json.dumps(plan, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        with self.submit_lock:
            try:
                prior = self.store.get(key=key)
            except ApiError as error:
                if error.status != 404:
                    raise
            else:
                if prior["fingerprint"] != fingerprint:
                    original = canonical_plan(prior["spec"])
                    candidate = canonical_plan(plan)
                    if spec.get("port", "auto") == "auto":
                        original["port"] = candidate["port"] = "auto"
                    if original != candidate:
                        raise ApiError("idempotency_conflict", "此任务键已用于不同请求或文件内容；新操作应使用新任务键。", 409)
                return self.store.public(prior)
            if not self.gate.acquire(False):
                raise ApiError("device_busy", "已有任务或串口查询正在进行，请稍后重试。", 409)
            identifier = secrets.token_hex(12)
            directory = self.store.root / "jobs" / identifier
            try:
                directory.mkdir(parents=True)
                if image is not None:
                    (directory / ("firmware.bin" if plan["action"] == "flash" else "input.dat")).write_bytes(image)
                job = {"id": identifier, "idempotency_key": key, "fingerprint": fingerprint,
                       "spec": plan, "state": "queued", "phase": "queued", "progress": 0,
                       "created_at": now(), "completed_at": None, "result": None, "error": None,
                       "directory": str(directory)}
                self.store.insert(job)
                threading.Thread(target=self.worker, args=(identifier,), daemon=True).start()
                return self.store.public(job)
            except BaseException:
                self.gate.release()
                raise

    def emit(self, identifier, phase, progress, message, work_done=None, work_total=None):
        job = self.store.get(identifier)
        with (Path(job["directory"]) / "events.log").open("a", encoding="utf-8") as file:
            file.write(f"{now()} [{phase}] {message}\n")
        changes=dict(phase=phase, progress=round(min(1, max(0, progress)), 4))
        if phase!=job.get("phase") or not job.get("phase_started_at"):
            changes.update(phase_started_at=now(),phase_first_done=work_done or 0,work_done=work_done,work_total=work_total)
        elif work_done is not None:
            changes.update(work_done=work_done,work_total=work_total)
        self.store.update(identifier, **changes)

    def child(self, identifier, args, echo_size=0):
        env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                   errors="replace", env=env, creationflags=flags, cwd=str(TOOLS))
        for line in process.stdout:
            text = line.strip()
            job = self.store.get(identifier)
            phase, progress = job["phase"], job["progress"]
            work_done=work_total=None
            received = re.search(r"Received (\d+)/(\d+)", text)
            stage = re.search(r"Stage=(\d+) bytes=(\d+)/(\d+)", text)
            echoed = re.search(r"Verified round trip (\d+)/(\d+)", text)
            if received:
                phase, progress = "transfer", .05+.55*int(received[1])/max(1, int(received[2]))
                work_done,work_total=int(received[1]),int(received[2])
            if stage:
                number = int(stage[1])
                phase = {0: "probe", 1: "probe", 2: "erase", 3: "write", 4: "verify", 5: "start"}.get(number, "target")
                progress = {0: .62, 1: .65, 2: .7, 3: .72+.12*int(stage[2])/max(1, int(stage[3])), 4: .85+.12*int(stage[2])/max(1, int(stage[3])), 5: .98}.get(number, .65)
                work_done,work_total=int(stage[2]),int(stage[3])
            if echoed:
                phase, progress = "echo", int(echoed[1])/max(1, int(echoed[2]))*.98
                work_done,work_total=int(echoed[1]),int(echoed[2])
            self.emit(identifier, phase, progress, text,work_done,work_total)
        return process.wait()

    def panel_command(self, spec):
        link = Link(None if spec["port"] == "auto" else spec["port"])
        try:
            if spec["action"] == "pid":
                values = spec["parameters"]
                link.query("!pid {p} {i} {d}".format(**values), "state")
                link.query("!step {step}".format(**values), "state")
            else:
                link.query("!"+spec["action"], "state")
            end = time.monotonic()+8
            while True:
                state = link.query("!get", "state")
                matches = True
                if spec["action"] in ("run", "stop"):
                    matches = state.get("ack") and state.get("run_request") == (spec["action"] == "run")
                elif spec["action"] == "pid":
                    matches = all(decimal.Decimal(str(state.get(p))) == decimal.Decimal(spec["parameters"][p]) for p in ("p", "i", "d", "step"))
                elif spec["action"] == "save":
                    matches = state.get("saved") and not state.get("dirty")
                if state.get("synced") and matches:
                    self.cached_state = state
                    return {"state": state, "ack_scope": "slave"}
                if time.monotonic() > end:
                    raise ApiError("sync_timeout", "主机命令已发送，但等待从机同步确认超时；没有伪造成功。")
                time.sleep(.1)
        finally:
            link.close()

    def worker(self, identifier):
        job = self.store.update(identifier, state="running", started_at=now())
        spec, directory = job["spec"], Path(job["directory"])
        try:
            self.emit(identifier, "validate", .02, "请求与镜像已校验，任务使用独立文件快照。" if spec["action"] == "flash" else "操作请求已校验。")
            if spec["action"] == "flash":
                # Re-check the route at execution time as well: an unsupported
                # interface must never fall through to a computer-side adapter.
                method = flash_method(spec)
                image = (directory/"firmware.bin").read_bytes()
                if hashlib.sha256(image).hexdigest() != spec["sha256"]:
                    raise ApiError("image_changed", "任务固件快照损坏，停止写入。")
                if method["id"] in ("wireless_uart","wireless_swd"):
                    result_path = directory/"transfer.json"
                    args = [sys.executable, str(TOOLS/"flash_stm32.py"), str(directory/"firmware.bin"),
                            "--part", spec["chip"], "--interface", spec["target_interface"], "--execute", "--journal", str(result_path)]
                    if spec["port"] != "auto": args += ["--port", spec["port"]]
                    if spec["run"]: args += ["--run"]
                    if spec.get("swd_legacy"): args += ["--swd-legacy"]
                    code = self.child(identifier, args)
                    result = json.loads(result_path.read_text(encoding="utf-8")) if result_path.exists() else {}
                    start_ok = result.get("start_ack") if spec["target_interface"]=="swd" else result.get("go_ack")
                    if code or not result.get("verified") or spec["run"] and not start_ok:
                        self.store.update(identifier, result=result)
                        raise ApiError("wireless_flash_failed", result.get("error", "无线写入/读回/启动未完成，详情见日志。"))
                else:
                    raise ApiError("interface_not_implemented", "从机目标接口没有可执行的烧录驱动。", 501)
                result.update(transport="esp_now", target_interface=method["target_interface"])
            elif spec["action"] == "echo":
                result_path = directory/"echo.json"
                args = [sys.executable, str(TOOLS/"echo_stm32.py"), "--journal", str(result_path)]
                if spec["port"] != "auto": args += ["--port", spec["port"]]
                if "text" in spec: args += ["--text", spec["text"]]
                elif "file" in spec: args += ["--file", str(directory/"input.dat")]
                else: args += ["--bytes", str(spec["bytes"])]
                code = self.child(identifier, args)
                result = json.loads(result_path.read_text(encoding="utf-8")) if result_path.exists() else {}
                if code or not result.get("passed"):
                    self.store.update(identifier, result=result)
                    raise ApiError("echo_failed", result.get("error", "未通过 STM32 全链路回传校验。"))
                returned = Path(result["returned_file"]).read_bytes()
                result["preview"] = returned[:8192].decode("utf-8", "replace")
            else:
                result = self.panel_command(spec)
            self.emit(identifier, "done", 1, "任务完成；验证结果已保存。")
            self.store.update(identifier, state="succeeded", progress=1, result=result, completed_at=now())
        except BaseException as error:
            self.emit(identifier, "failed", self.store.get(identifier)["progress"], str(error))
            self.store.update(identifier, state="failed", error={"code": getattr(error, "code", "operation_failed"), "message": str(error)}, completed_at=now())
        finally:
            self.gate.release()
