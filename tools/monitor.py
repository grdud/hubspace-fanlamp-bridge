"""Optional USB diagnostics. Never sends fan commands or refreshes cloud tokens."""
import argparse
import time
from common import open_serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=int, default=120)
args = parser.parse_args()
with open_serial(args.port) as conn:
    conn.write(b"STATUS\n")
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        line = conn.readline().decode(errors="replace").strip()
        if line.startswith(("OK ", "STATUS ", "STATE ", "ERR ", "HTTP_ERROR ", "JSON_ERROR ", "EVENT ", "TX ", "FATAL ")):
            print(line, flush=True)
