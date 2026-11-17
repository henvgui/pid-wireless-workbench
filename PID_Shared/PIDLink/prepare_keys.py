"""Prepare local ESP-NOW keys and, optionally, a different ESP32-S3 pair."""
import argparse
from pathlib import Path
import re
import secrets


def mac(text):
    if not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", text, re.IGNORECASE):
        raise argparse.ArgumentTypeError("Use a MAC/USB serial such as AA:BB:CC:DD:EE:02")
    address = bytes.fromhex(text.replace(":", ""))
    if not any(address) or address[0] & 1:
        raise argparse.ArgumentTypeError("Expected a nonzero unicast address")
    return address


def declaration(name, value):
    return "static constexpr uint8_t %s[6]={%s};" % (name, ",".join("0x%02x" % b for b in value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=mac, help="Host's native USB serial / factory MAC")
    parser.add_argument("--slave", type=mac, help="First slave's native USB serial / factory MAC")
    args = parser.parse_args()
    if (args.host is None) != (args.slave is None) or (args.host is not None and args.host == args.slave):
        parser.error("Specify both --host and --slave with different addresses")

    private = Path(__file__).parent / "private"
    private.mkdir(exist_ok=True)
    if args.host is not None:
        devices = private / "pair_devices.h"
        contents = "\n".join(("#pragma once", "#include <stdint.h>", "namespace pidlink {",
                              declaration("kConfiguredHostMac", args.host),
                              declaration("kDefaultSlaveMac", args.slave), "}", ""))
        if devices.exists():
            if devices.read_text(encoding="ascii") != contents:
                parser.error("This checkout already has a different pair; use a fresh clone for a different kit")
        else:
            devices.write_text(contents, encoding="ascii")
        print("Configured local ESP32 addresses")

    target = private / "pair_keys.h"
    if target.exists():
        print("Existing pair keys retained")
    else:
        lines = ["#pragma once", "#include <stdint.h>", "namespace pidlink {"]
        for name in ("kPrimaryKey", "kPairKey"):
            values = ",".join("0x%02x" % b for b in secrets.token_bytes(16))
            lines.append("static const uint8_t %s[16]={%s};" % (name, values))
        lines.append("}")
        with target.open("x", encoding="ascii") as file:
            file.write("\n".join(lines) + "\n")
        print("Created local pair keys; values are not printed")


if __name__ == "__main__":
    main()
