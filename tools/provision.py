"""Enter your own Wi-Fi/Hubspace credentials and send them directly over USB."""
import argparse
import getpass
import json
from pathlib import Path
import sys
import time

from common import login, open_serial, fail


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    if not (Path(__file__).resolve().parents[1] / "include/bridge_config.h").exists():
        raise ValueError("Run configure.py, then build/upload the main firmware first")
    ssid = input("2.4 GHz Wi-Fi name: ")
    password = getpass.getpass("Wi-Fi password (hidden; blank for an open network): ")
    if not 1 <= len(ssid.encode()) <= 32 or len(password.encode()) > 63:
        raise ValueError("Invalid Wi-Fi name/password length")
    account, token, _ = login()
    if not 20 <= len(token) <= 7000:
        raise ValueError("Unsupported refresh-token size")
    payload = {"ssid": ssid, "password": password, "refresh_token": token, "account": account}
    with open_serial(args.port) as conn:
        conn.write(("CONFIG " + json.dumps(payload, separators=(",", ":")) + "\n").encode())
        del payload, token, password
        end = time.monotonic() + 45
        while time.monotonic() < end:
            line = conn.readline().decode(errors="replace").strip()
            if line == "OK CONFIG SAVED":
                print("Configuration saved on ESP32. It will reboot and connect automatically.")
                return
            if line.startswith("ERR CONFIG"):
                raise RuntimeError("Firmware rejected configuration")
        raise TimeoutError("No provisioning acknowledgement; check the selected port and firmware")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        fail(error)
        sys.exit(1)
