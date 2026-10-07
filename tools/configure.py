"""Select one or two dimmer switches and generate an ignored, non-secret configuration."""
import argparse
import json
from pathlib import Path
import re
import sys

from common import login, fail

ROOT = Path(__file__).resolve().parents[1]


def remote_config(path, record_number):
    record = next((json.loads(line) for line in Path(path).read_text().splitlines()
                   if json.loads(line).get("record") == record_number), None)
    if record is None:
        raise ValueError("Record not found")
    matches = [m for m in record.get("matches", []) if m.get("codec") == "fanlamp_pro_v2/r"]
    if len(matches) != 1:
        raise ValueError("This firmware only supports a confirmed fanlamp_pro_v2/r capture")
    config = matches[0]["config"]
    if config.get("codec_params") != [True, [16, 0, 86]]:
        raise ValueError("Unsupported FanLamp protocol parameters")
    for name, maximum in (("id", 0xffffffff), ("index", 255), ("tx_count", 255)):
        value = config.get(name)
        if type(value) is not int or not 0 <= value <= maximum:
            raise ValueError("Invalid capture configuration")
    if config["id"] == 0:
        raise ValueError("A paired controller identity is required")
    return config


def candidates(devices, device_class):
    result = []
    seen = set()
    for d in devices:
        physical = d.get("deviceId", "")
        cls = d.get("description", {}).get("device", {}).get("deviceClass")
        if cls == device_class and re.fullmatch(r"[0-9a-fA-F]{16}", physical) and physical not in seen:
            seen.add(physical)
            result.append((str(d.get("friendlyName", device_class)), physical.lower()))
    return result


def choose(items, title, optional=False):
    print(title)
    if optional:
        print("0. None")
    for n, (name, _) in enumerate(items, 1):
        print(f"{n}. {name}")
    if not items and not optional:
        raise ValueError("No compatible switch was discovered")
    selection = int(input("Number: ").strip() or ("0" if optional else "-1"))
    if optional and selection == 0:
        return ""
    if not 1 <= selection <= len(items):
        raise ValueError("Invalid selection")
    return items[selection - 1][1]


def render(config, switch1, switch2, minimum1=1, minimum2=1):
    for identity in (switch1, switch2):
        if identity and not re.fullmatch(r"[0-9a-fA-F]{16}", identity):
            raise ValueError("Invalid physical ID")
    if not switch1 or switch1 == switch2:
        raise ValueError("Switch 1 required; switches must be distinct")
    if not all(1 <= minimum <= 99 for minimum in (minimum1, minimum2)):
        raise ValueError("Logical brightness minimum must be 1..99")
    return f'''#pragma once
// Generated locally. Ignored by Git. Contains identities, not account credentials.
#define BRIDGE_SWITCH_1_ID "{switch1}"
#define BRIDGE_SWITCH_2_ID "{switch2}"
#define BRIDGE_BLE_ID {config['id']}u
#define BRIDGE_BLE_INDEX {config['index']}
#define BRIDGE_BLE_PREFIX_0 0x10
#define BRIDGE_BLE_PREFIX_1 0x00
#define BRIDGE_BLE_PREFIX_2 0x56
#define BRIDGE_INITIAL_COUNTER {config['tx_count'] % 125}
#define BRIDGE_SWITCH_1_MIN {minimum1}
#define BRIDGE_SWITCH_2_MIN {minimum2}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--record", type=int, required=True)
    parser.add_argument("--switch-min1", type=int, default=1)
    parser.add_argument("--switch-min2", type=int, default=1)
    args = parser.parse_args()
    config = remote_config(args.capture, args.record)
    _, _, devices = login(discover=True)
    items = candidates(devices, "switch")
    switch1 = choose(items, "Select switch 1:")
    switch2 = choose([x for x in items if x[1] != switch1], "Optional switch 2:", True)
    text = render(config, switch1, switch2, args.switch_min1, args.switch_min2)
    target = ROOT / "include/bridge_config.h"
    if target.exists() and input("Replace existing local configuration? [y/N] ").strip().lower() != "y":
        print("Left existing configuration unchanged.")
        return
    target.write_text(text, encoding="utf-8")
    print("Saved include/bridge_config.h. Build/upload the main firmware next.")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        fail(error)
        sys.exit(1)
