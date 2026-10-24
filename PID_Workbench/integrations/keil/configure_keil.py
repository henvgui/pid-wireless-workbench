"""Install a backed-up Keil Download-button configuration; no code/target changes."""
import argparse
import datetime
from pathlib import Path
import re
import sys
from xml.sax.saxutils import escape
import xml.etree.ElementTree as ET


def configure(project, target, chip, python, fromelf, launcher=None):
    project = Path(project).resolve(strict=True)
    original = project.read_bytes()
    text = original.decode("utf-8-sig")
    root = ET.fromstring(text)
    matches = [t for t in root.findall("./Targets/Target") if t.findtext("TargetName") == target]
    if len(matches) != 1:
        raise ValueError("Expected exactly one matching Keil target")
    device = matches[0].findtext("TargetOption/TargetCommonOption/Device", "")
    if not (device == chip or device.endswith("x") and chip.startswith(device[:-1])):
        raise ValueError(f"Project device {device} differs from selected {chip}; no settings changed")
    script = Path(__file__).with_name("pid_keil.py").resolve(strict=True)
    python = Path(python).resolve(strict=True)
    converter = Path(fromelf).resolve(strict=True)
    launcher = Path(launcher or script.with_name("PidKeilDownload.exe")).resolve(strict=True)
    args = f'"{python}" "{script}" --firmware "#L" --project "#P" --chip {chip} --interface swd --fromelf "{converter}"'
    command = f'"{launcher}" ({args})'
    changed = 0

    def update_target(match):
        nonlocal changed
        block = match.group()
        if ET.fromstring(block).findtext("TargetName") != target:
            return block
        utilities = re.search(r"<Utilities>.*?</Utilities>", block, re.S)
        if not utilities:
            raise ValueError("Project is missing Utilities settings")
        settings = utilities.group()
        for name, value in (("UseTargetDll", "0"), ("UseExternalTool", "1"), ("RunIndependent", "0"),
                            ("UpdateFlashBeforeDebugging", "0"), ("bUseTDR", "0"), ("Flash3", command)):
            pattern = r"<" + name + r"\b[^>]*(?:/>|>.*?</" + name + r">)"
            settings, count = re.subn(pattern, lambda m: f"<{name}>{escape(value)}</{name}>", settings, count=1, flags=re.S)
            if count != 1:
                raise ValueError("Missing project field " + name)
        changed += 1
        return block[:utilities.start()] + settings + block[utilities.end():]

    updated = re.sub(r"<Target>.*?</Target>", update_target, text, flags=re.S)
    assert changed == 1
    ET.fromstring(updated)
    # uVision 5 rejects these project files with a UTF-8 BOM.
    output = updated.encode("utf-8")
    if output != original:
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        backup = project.with_name(project.name + ".before_pid_" + stamp + ".bak")
        backup.write_bytes(original)
        # Refuse to overwrite a concurrent Keil/user save.
        if project.read_bytes() != original:
            raise ValueError("Project changed during configuration; retry after saving it")
        project.write_bytes(output)
        print("Backup:", backup)
    print("Configured:", project, "target=" + target)
    print("Command:", launcher)
    print("Arguments:", args)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--chip", default="STM32F407VET6", choices=["STM32F407VET6", "STM32F407VGT6"])
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--fromelf", type=Path, default=Path(r"C:\Keil_v5\ARM\ARMCC\bin\fromelf.exe"))
    args = parser.parse_args()
    configure(args.project, args.target, args.chip, args.python, args.fromelf)
