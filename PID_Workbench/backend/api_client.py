import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

from workbench_core import ApiError, DATA_DIR

BASE = "http://127.0.0.1:18765"
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def health():
    try:
        with OPENER.open(BASE+"/health", timeout=.8) as response:
            value = json.load(response)
        if value.get("app") != "pid-workbench":
            raise ApiError("port_in_use", "本机 18765 端口已被其他程序使用。")
        return value
    except (OSError, urllib.error.URLError):
        return None


def ensure_server():
    current = health()
    if current:
        return current
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    with (DATA_DIR/"server.log").open("ab") as log:
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        subprocess.Popen([sys.executable, str(Path(__file__).with_name("server.py"))],
                         stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                         close_fds=True, creationflags=flags)
    for _ in range(40):
        time.sleep(.15)
        current = health()
        if current:
            return current
    raise ApiError("service_start_failed", "后台服务启动失败，查看 %LOCALAPPDATA%/PIDWorkbench/server.log。")


def token_paths():
    """An unpackaged Keil process must also reach a Codex-packaged service.

    MSIX can redirect the service's apparent LocalAppData into its LocalCache.
    Do not copy/create a second token: use the existing service's credential.
    An explicit data-directory override remains authoritative.
    """
    paths = [DATA_DIR / "api_token.txt"]
    if os.environ.get("PID_WORKBENCH_DATA"):
        return paths
    roots = [Path(os.environ["LOCALAPPDATA"])] if os.environ.get("LOCALAPPDATA") else []
    if os.environ.get("USERPROFILE"):
        roots.append(Path(os.environ["USERPROFILE"]) / "AppData" / "Local")
    for root in roots:
        for package in sorted((root / "Packages").glob("OpenAI.Codex_*")):
            candidate = package / "LocalCache" / "Local" / "PIDWorkbench" / "api_token.txt"
            if candidate not in paths:
                paths.append(candidate)
    return paths


def request(path, value=None, start=True, credential_paths=None):
    if start:
        ensure_server()
    data = None if value is None else json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
    unauthorized = None
    for token_path in token_paths():
        try:
            api_token = token_path.read_text(encoding="ascii").strip()
        except (OSError, UnicodeError):
            continue
        if not re.fullmatch(r"[0-9a-f]{64}", api_token):
            continue
        headers = {"Authorization": "Bearer "+api_token, "Content-Type": "application/json"}
        req = urllib.request.Request(BASE+path, data=data, headers=headers, method="GET" if data is None else "POST")
        try:
            with OPENER.open(req, timeout=20) as response:
                result = json.load(response)
                if credential_paths is not None:
                    credential_paths.append(str(token_path))
                return result
        except urllib.error.HTTPError as error:
            with error:
                body = json.load(error)
            detail = body.get("error", {})
            failure = ApiError(detail.get("code", "http_error"), detail.get("message", str(error)), error.code)
            if "remote_error" in detail: failure.remote_error = detail["remote_error"]
            if error.code == 401:
                # Authentication is checked BEFORE any action by our server.
                # Only an explicit 401 permits trying another credential. Never
                # replay a POST after a timeout, disconnect, or other status.
                unauthorized = failure
                continue
            raise failure from error
    if unauthorized:
        raise unauthorized
    raise ApiError("api_token_missing", "本机烧录服务的凭据文件不存在或无效；已检查普通用户目录和 Codex 应用数据目录。", 503)
