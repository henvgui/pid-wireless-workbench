"""Keil Flash/Download adapter: AXF/HEX/BIN -> existing wireless job API."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
from urllib.parse import quote

APP = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(APP / "backend"))
from api_client import request
from workbench_core import ApiError, CHIPS, prepare

BASE = 0x08000000


def hex_image(text, capacity):
    """Validate every record/address; gaps are erased Flash, never relocated."""
    data = {}
    upper = 0
    eof = False
    for line_number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if eof or not line.startswith(":"):
            raise ValueError(f"Invalid Intel HEX record at line {line_number}")
        raw = bytes.fromhex(line[1:])
        if len(raw) < 5 or len(raw) != raw[0] + 5 or sum(raw) & 255:
            raise ValueError(f"Intel HEX length/checksum error at line {line_number}")
        size, kind = raw[0], raw[3]
        offset = int.from_bytes(raw[1:3], "big")
        payload = raw[4:-1]
        if kind == 0:
            address = upper + offset
            if offset + size > 0x10000 or address < BASE or address + size > BASE + capacity:
                raise ValueError(f"HEX address outside selected Flash: 0x{address:08X}")
            for index, value in enumerate(payload, address):
                if index in data and data[index] != value:
                    raise ValueError(f"Conflicting HEX bytes at 0x{index:08X}")
                data[index] = value
        elif kind == 1 and size == 0 and offset == 0:
            eof = True
        elif kind in (2, 4) and size == 2 and offset == 0:
            upper = int.from_bytes(payload, "big") << (4 if kind == 2 else 16)
        elif kind in (3, 5) and size == 4 and offset == 0:
            pass  # Entry metadata: actual reset vector is validated below.
        else:
            raise ValueError(f"Unsupported/malformed HEX record type {kind}")
    if not eof or not data or min(data) != BASE:
        raise ValueError("HEX must end with EOF and start at 0x08000000; relocation is not supported")
    image = bytearray(b"\xff" * (max(data) - BASE + 1))
    for address, value in data.items():
        image[address - BASE] = value
    return bytes(image)


def convert(source, directory, chip, fromelf=None):
    source = Path(source).resolve(strict=True)
    profile = next((item for item in CHIPS if item["id"] == chip), None)
    if profile is None:
        raise ValueError("Unsupported target chip")
    snapshot = directory / ("source" + source.suffix.lower())
    snapshot.write_bytes(source.read_bytes())
    extension = source.suffix.lower()
    if extension in (".axf", ".elf"):
        converter = Path(fromelf) if fromelf else Path(r"C:\Keil_v5\ARM\ARMCC\bin\fromelf.exe")
        if not converter.is_file():
            raise ValueError("fromelf.exe not found; pass --fromelf with your compiler's path")
        exported = directory / "source.hex"
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        result = subprocess.run([str(converter), "--i32combined", "--output=" + str(exported), str(snapshot)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90,
                                creationflags=flags)
        (directory / "fromelf.log").write_bytes(result.stdout)
        if result.returncode or not exported.is_file():
            raise ValueError("fromelf failed; see fromelf.log")
        image = hex_image(exported.read_text(encoding="ascii"), profile["flash_kb"] * 1024)
    elif extension == ".hex":
        image = hex_image(snapshot.read_text(encoding="utf-8-sig"), profile["flash_kb"] * 1024)
    elif extension == ".bin":
        image = snapshot.read_bytes()
    else:
        raise ValueError("Expected .axf, .elf, .hex or .bin")
    output = directory / "firmware.bin"
    output.write_bytes(image)
    # Existing validation checks SP, Thumb reset vector, image length and capacity.
    plan, _ = prepare({"action": "flash", "chip": chip, "firmware": str(output), "port": "auto"})
    return output, plan


def save(path, record):
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temp.replace(path)


def emit(directory, message):
    print("[PID] " + message, flush=True)
    with (directory / "events.log").open("a", encoding="utf-8") as handle:
        handle.write(datetime.datetime.now(datetime.timezone.utc).isoformat() + " " + message + "\n")


def preflight(spec):
    caps = request("/v1/capabilities")["data"]
    chip = next((c for c in caps["chips"] if c["id"] == spec["chip"]), None)
    if not chip or spec["target_interface"] not in chip["interfaces"]:
        raise ValueError("Chip/interface not supported by the running backend")
    if not any(m["available"] and m["target_interface"] == spec["target_interface"] for m in caps["methods"]):
        raise ValueError("Target interface is unavailable")
    request("/v1/ports")
    active = [j for j in request("/v1/jobs")["data"] if j["state"] in ("queued", "running")]
    if active:
        raise ValueError("Hardware busy; wait for existing job " + active[0]["id"])
    # A short GUI status query can momentarily hold the same hardware lock.
    for _ in range(12):
        device = request("/v1/device?port=" + quote(spec["port"]))["data"]
        if not device.get("busy"):
            break
        time.sleep(.25)
    else:
        raise ValueError("Device busy; no programming job submitted")
    state = device.get("state") or {}
    if not state.get("link_online") or not state.get("synced"):
        raise ValueError("Host/slave wireless link is not online and synchronized")
    return request("/v1/plan", spec)["data"]


def check_success(job, spec):
    result = job.get("result") or {}
    if job.get("state") != "succeeded" or result.get("verified") is not True:
        raise ValueError("Programming failed or readback not verified: " + json.dumps(job.get("error"), ensure_ascii=True))
    if spec["run"] and result.get("start_ack" if spec["target_interface"] == "swd" else "go_ack") is not True:
        raise ValueError("Target start was not acknowledged")
    if job.get("spec", {}).get("sha256") != spec["expected_sha256"]:
        raise ValueError("Completed job image SHA256 does not match this request")

def progress_line(job):
    line=f"{job.get('phase')} {round(100*job.get('progress',0))}%"
    done,total=job.get('work_done'),job.get('work_total')
    if type(done) is int and type(total) is int and 0<=done<=total and total>0:
        speed=0
        try:
            elapsed=(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(job['phase_started_at'])).total_seconds()
            first=job.get('phase_first_done',done)
            if elapsed>=.1 and type(first) is int and first<=done:speed=round((done-first)/elapsed)
        except (KeyError,TypeError,ValueError):pass
        line+=f" bytes={done}/{total} speed={speed}"
    return line


def execute_record(path, timeout=1800, resume=False):
    record = json.loads(path.read_text(encoding="utf-8"))
    directory, spec = path.parent, record["request"]
    image = Path(spec["firmware"]).read_bytes()
    if hashlib.sha256(image).hexdigest() != spec["expected_sha256"]:
        raise ValueError("Recorded image changed; refusing to submit or resume")
    try:
        if not resume:
            record["plan"] = preflight(spec)
            save(path, record)
        if not record.get("job_id"):
            # Persist the intent/key BEFORE POST. An uncertain response is resumed
            # with this same key; the server never creates a second erase task.
            record["status"] = "submitting"
            save(path, record)
            for attempt in range(12):
                try:
                    job = request("/v1/jobs", spec)["data"]
                    break
                except ApiError as error:
                    if error.code != "device_busy" or attempt == 11:
                        raise
                    time.sleep(.25)
            record["job_id"] = job["id"]
            record["status"] = "running"
            save(path, record)
        emit(directory, "Job " + record["job_id"])
        deadline = time.monotonic() + timeout
        last = None
        while True:
            job = request("/v1/jobs/" + record["job_id"])["data"]
            save(directory / "job.json", job)
            progress = (job.get("phase"), round(100 * job.get("progress", 0)),job.get('work_done'))
            if progress != last:
                emit(directory, progress_line(job))
                last = progress
            if job["state"] not in ("queued", "running"):
                break
            if time.monotonic() >= deadline:
                raise TimeoutError("Waiting timed out; existing job continues. Resume this record, do not create another task")
            time.sleep(.15)
        check_success(job, spec)
        record.update(status="succeeded", result=job["result"])
        record.pop("error", None)
        save(path, record)
        emit(directory, "Programming Done. Verify OK. " + ("Application running." if spec["run"] else "Target left halted."))
        return 0
    except Exception as error:
        record.update(status="attention_required", error=str(error))
        save(path, record)
        raise


def retry_record(path, timeout=1800):
    """An explicit user retry: reconcile the old intent before another erase."""
    path = path.resolve(strict=True)
    previous = json.loads(path.read_text(encoding="utf-8"))
    spec = previous["request"]
    image = Path(spec["firmware"]).read_bytes()
    if hashlib.sha256(image).hexdigest() != spec["expected_sha256"]:
        raise ValueError("Recorded image changed; refusing to retry")
    if previous.get("status") in ("succeeded", "checked"):
        raise ValueError("This attempt already completed; retry is for failed downloads")
    emit(path.parent, "Record " + str(path))
    emit(path.parent, "Checking the previous task before retrying.")
    # Resolve a lost POST response by its persisted key. Looking up the list is
    # read-only; never manufacture a fresh key while its outcome is unknown.
    jobs = request("/v1/jobs")["data"]
    matches = [job for job in jobs if job.get("idempotency_key") == spec["idempotency_key"]
               or previous.get("job_id") and job.get("id") == previous["job_id"]]
    if len(matches) > 1:
        raise ValueError("Previous task identity is ambiguous; no new download submitted")
    prior = matches[0] if matches else None
    if prior:
        if (prior.get("spec") or {}).get("sha256") != spec["expected_sha256"]:
            raise ValueError("Previous task image differs; no new download submitted")
        if prior["state"] in ("queued", "running", "succeeded"):
            previous["job_id"] = prior["id"]
            save(path, previous)
            emit(path.parent, "Continuing the existing task; no second erase requested.")
            return execute_record(path, timeout, resume=True)
        if prior["state"] not in ("failed", "interrupted"):
            raise ValueError("Previous task is not in a retryable state")
    elif previous.get("job_id"):
        raise ValueError("Previous task cannot be found; refusing to duplicate an uncertain download")
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    directory = path.parent.parent / (stamp + "_retry_" + secrets.token_hex(4))
    directory.mkdir(parents=True, exist_ok=False)
    snapshot = directory / "firmware.bin"
    snapshot.write_bytes(image)
    retry = dict(spec, port="auto")  # Rescan host USB identity, including changed COM numbers.
    if prior:
        # A terminal failed task cannot be rerun with its old key. This click is
        # the new intent; keep that key fixed for all responses/retries below.
        retry.update(firmware=str(snapshot), idempotency_key="keil-" + secrets.token_hex(16))
    # If no job exists, preserve the original key and snapshot path so a racing
    # or lost original submission is resolved by the API's idempotency check.
    record = {"version": 1, "source": previous.get("source"), "project": previous.get("project"),
              "request": retry, "status": "prepared", "previous_attempt": str(path),
              "retry_mode": "new_after_failure" if prior else "resolve_original_intent"}
    destination = directory / "attempt.json"
    save(destination, record)
    emit(directory, "Record " + str(destination))
    emit(directory, "Retry requested: rescan host USB; use the recorded firmware SHA256 " + spec["expected_sha256"])
    return execute_record(destination, timeout)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--chip", choices=[c["id"] for c in CHIPS], default="STM32F407VET6")
    parser.add_argument("--interface", choices=["uart", "swd"])
    parser.add_argument("--port", default="auto")
    parser.add_argument("--project", type=Path)
    parser.add_argument("--fromelf", type=Path)
    parser.add_argument("--records-dir", type=Path)
    recovery = parser.add_mutually_exclusive_group()
    recovery.add_argument("--resume", type=Path)
    recovery.add_argument("--retry-record", type=Path, help="Explicit user retry of a failed attempt; reconcile prior job, rescan USB, preserve image")
    parser.add_argument("--check", action="store_true", help="Conversion/validation only, no API or target writes")
    parser.add_argument("--no-run", action="store_true")
    parser.add_argument("--timeout", type=float, default=1800)
    args = parser.parse_args(argv)
    if not args.timeout > 0 or args.timeout == float("inf"):
        parser.error("timeout must be finite and positive")
    if args.resume or args.retry_record:
        if args.firmware or args.check:
            parser.error("--resume/--retry-record cannot be combined with --firmware/--check")
        if args.retry_record:
            return retry_record(args.retry_record, args.timeout)
        return execute_record(args.resume.resolve(strict=True), args.timeout, resume=True)
    if not args.firmware:
        parser.error("--firmware is required unless resuming")
    source = args.firmware.resolve(strict=True)
    root = args.project.resolve(strict=True).parent.parent if args.project else source.parent
    records = args.records_dir or root / "flash_records" / "keil"
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    directory = (records / (stamp + "_" + secrets.token_hex(4))).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    emit(directory, "Record " + str(directory / "attempt.json"))
    output, plan = convert(source, directory, args.chip, args.fromelf)
    interface = args.interface or next(c["default_interface"] for c in CHIPS if c["id"] == args.chip)
    spec = {"action": "flash", "chip": args.chip, "transport": "esp_now", "target_interface": interface,
            "port": args.port, "firmware": str(output), "expected_sha256": plan["sha256"],
            "run": not args.no_run, "idempotency_key": "keil-" + secrets.token_hex(16)}
    # Revalidate the explicit interface/port as well as the converted vectors.
    plan, _ = prepare(spec)
    record = {"version": 1, "source": str(source), "project": str(args.project) if args.project else None,
              "request": spec, "plan": plan, "status": "checked" if args.check else "prepared"}
    path = directory / "attempt.json"
    save(path, record)
    emit(directory, f"{args.chip} / {interface.upper()} / {plan['size']} bytes / SHA256 {plan['sha256']}")
    if args.check:
        emit(directory, "Check OK. No programming job submitted.")
        return 0
    return execute_record(path, args.timeout)


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace", line_buffering=True)
    try:
        sys.exit(main())
    except Exception as error:
        print("[PID] ERROR: " + str(error), file=sys.stderr, flush=True)
        sys.exit(1)
