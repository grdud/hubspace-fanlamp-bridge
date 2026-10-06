"""USB BLE capture/decode tool. Python 3.12+. No transmission without --send.

Capture: tools/capture.py capture --port COM3 --seconds 30 --output capture.jsonl
Decode:  tools/capture.py decode capture.jsonl
Fresh repeat of one selected, verified command:
 tools/capture.py repeat capture.jsonl --record 12 --codec fanlamp_pro_v2
Add --send --port COM3 only during an observed fan test.
"""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).parent / "vendor"))
from ble_codecs import get_codecs
from ble_codecs.models import BleAdvAdvertisement, BleAdvCodec
import serial

CODECS = get_codecs()

def decode(raw):
    adv = BleAdvAdvertisement.FromRaw(bytes.fromhex(raw))
    matches = []
    for name, codec in CODECS.items():
        cmd, conf = codec.decode_adv(adv)
        if cmd is not None and conf is not None:
            entities = codec.enc_to_ent(cmd, BleAdvCodec.DEF_TRANS_NAME)
            matches.append({"codec": name, "config": vars(conf), "command": vars(cmd),
                            "entities": [vars(e) for e in entities]})
    return matches

def open_serial(port):
    # This CP210x board needs a deterministic reset after Windows opens the port.
    # Firmware is passive at boot, so reset cannot resend a previous command.
    conn = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
    conn.dtr = False
    conn.rts = False
    conn.port = port
    conn.open()
    conn.rts = True
    time.sleep(0.2)
    conn.rts = False
    time.sleep(1.0)
    conn.reset_input_buffer()
    return conn

def read_line(conn):
    # pyserial's default readline reads byte by byte on Windows; dense BLE
    # traffic can outrun that. Read available chunks and preserve line tails.
    if not hasattr(conn, "_bridge_buffer"):
        conn._bridge_buffer = b""
    deadline = time.monotonic() + 0.3
    while b"\n" not in conn._bridge_buffer:
        chunk = conn.read(max(1, min(conn.in_waiting, 8192)))
        conn._bridge_buffer += chunk
        if time.monotonic() >= deadline and b"\n" not in conn._bridge_buffer:
            return ""
    line, conn._bridge_buffer = conn._bridge_buffer.split(b"\n", 1)
    return line.decode(errors="replace").strip()

def request(conn, command, timeout=5):
    conn.write((command + "\n").encode())
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = read_line(conn)
        if line.startswith("ERR"):
            raise RuntimeError(line)
        if line.startswith("OK"):
            return line
    raise TimeoutError(f"No response to {command.split()[0]}")

def capture(args):
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    count = matched = 0
    # Exclusive creation prevents silently overwriting a previous capture.
    with path.open("x", encoding="utf-8") as out, open_serial(args.port) as conn:
        print(request(conn, "PING"), flush=True)
        request(conn, "SCAN")
        print(f"Listening for {args.seconds}s. Operate the working fan controller now.", flush=True)
        end = time.monotonic() + args.seconds
        seen = set()
        while time.monotonic() < end:
            line = read_line(conn)
            fields = line.split()
            if len(fields) != 5 or fields[0] != "ADV":
                continue
            _, uptime, rssi, addr, raw = fields
            # Keep each distinct packet once per capture, including duplicates' timing only in raw radio stream.
            if (addr, raw) in seen:
                continue
            seen.add((addr, raw))
            matches = decode(raw)
            item = {"record": count, "time": time.time(), "uptime_ms": int(uptime),
                    "rssi": int(rssi), "address": addr, "raw": raw,
                    "label": args.label, "matches": matches}
            out.write(json.dumps(item) + "\n")
            out.flush()
            if matches:
                matched += 1
                print(json.dumps({"record": count, "matches": matches}), flush=True)
            count += 1
        print(request(conn, "PING"), flush=True)
    print(f"Saved {count} distinct packets; {matched} protocol matches to {path}")

def repeat(args):
    record = next((json.loads(line) for line in Path(args.file).read_text().splitlines()
                   if json.loads(line)["record"] == args.record), None)
    if record is None:
        raise ValueError("Record not found")
    codec = CODECS[args.codec]
    cmd, conf = codec.decode_adv(BleAdvAdvertisement.FromRaw(bytes.fromhex(record["raw"])))
    if cmd is None or conf is None:
        raise ValueError("Selected codec does not decode this record")
    entities = codec.enc_to_ent(cmd, BleAdvCodec.DEF_TRANS_NAME)
    # A deliberate diagnostic may repeat exactly one recognized main-light
    # toggle. Production callers must keep the default idempotent-only policy.
    diagnostic_toggle = (getattr(args, "allow_light_toggle", False) and len(entities) == 1 and
                         entities[0].base_type == "light" and entities[0].index == 0 and
                         entities[0].chg_attrs == ["cmd"] and entities[0].attrs == {"cmd": "toggle"})
    diagnostic_brightness = (getattr(args, "allow_brightness_step", False) and len(entities) == 1 and
                             entities[0].base_type == "light" and entities[0].index == 0 and
                             entities[0].chg_attrs == ["cmd"] and
                             entities[0].attrs.get("cmd") in ("B-", "B+") and
                             set(entities[0].attrs).issubset({"cmd", "sub_type", "step"}))
    if not (diagnostic_toggle or diagnostic_brightness) and (not entities or any(e.base_type not in ("fan", "light") or
                           not set(e.chg_attrs).issubset({"on", "speed"}) or
                           any(e.attrs.get(k) == "toggle" for k in e.chg_attrs) for e in entities)):
        raise ValueError("Initial repeat tool allows explicit fan/light power and fan speed only")
    state_path = Path(args.state)
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    key = json.dumps([args.codec, conf.id, conf.index, conf.codec_params])
    if key in state:
        conf.tx_count = state[key]["tx_count"]
        conf.app_restart_count = state[key]["app_restart_count"]
    conf.seed = 0  # Upstream generates a fresh seed where this protocol requires it.
    payloads = [a.to_raw().hex() for a in codec.encode_advs(cmd, conf)]
    if any(len(raw) > 62 for raw in payloads):
        raise ValueError("Advertisement exceeds legacy BLE transport limit")
    print(json.dumps({"entities": [vars(e) for e in entities], "config": vars(conf),
                      "payloads": payloads, "send": args.send}, indent=2))
    if args.send:
        with open_serial(args.port) as conn:
            print(request(conn, "PING"))
            # Persist counter before sending; skip a counter rather than reuse it after a partial send.
            state[key] = {"tx_count": conf.tx_count, "app_restart_count": conf.app_restart_count}
            state_path.parent.mkdir(parents=True, exist_ok=True)
            temp = state_path.with_suffix(".tmp")
            temp.write_text(json.dumps(state, indent=2))
            temp.replace(state_path)
            for raw in payloads:
                print(request(conn, "TX " + raw))
        print("Radio transmission completed. User must verify the fan response.")

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    cap = sub.add_parser("capture")
    cap.add_argument("--port", default="COM3")
    cap.add_argument("--seconds", type=int, default=30)
    cap.add_argument("--output", required=True)
    cap.add_argument("--label", default="unlabeled")
    dec = sub.add_parser("decode")
    dec.add_argument("file")
    rep = sub.add_parser("repeat")
    rep.add_argument("file")
    rep.add_argument("--record", type=int, required=True)
    rep.add_argument("--codec", choices=sorted(CODECS), required=True)
    rep.add_argument("--send", action="store_true")
    rep.add_argument("--allow-light-toggle", action="store_true",
                     help="Diagnostic only: permit a single recognized main-light toggle; never auto-retry")
    rep.add_argument("--allow-brightness-step", action="store_true",
                     help="Diagnostic only: permit one recognized relative brightness step; never auto-retry")
    rep.add_argument("--port", default="COM3")
    rep.add_argument("--state", default=str(Path(__file__).resolve().parents[1] / ".local" / "tx-counters.json"))
    args = parser.parse_args()
    if args.action == "capture":
        if not 1 <= args.seconds <= 3600:
            parser.error("seconds must be 1..3600")
        capture(args)
    elif args.action == "decode":
        for line in Path(args.file).read_text().splitlines():
            item = json.loads(line)
            matches = decode(item["raw"])
            if matches:
                print(json.dumps({"record": item["record"], "label": item.get("label"), "matches": matches}))
    else:
        repeat(args)

if __name__ == "__main__":
    main()
