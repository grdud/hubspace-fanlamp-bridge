# Architecture and debugging notes

## Data path

The receiver provides the genuine Hubspace identity. REST discovery finds its parent, fan child, and light child by physical device ID and device class. Dynamic meta-device IDs are never hard-coded. Initial state is read through REST; changes arrive through a TLS Conclave push stream. Reconnection repeats discovery and seeds state without replaying commands.

Receiver attributes currently interpreted:

| Raw attribute | Meaning |
|---|---|
| 1 | Primary power |
| 2 | Light power |
| 3 | Fan power |
| 4 | Brightness, 0–100 |
| 6 | Fan speed, 0/25/50/75/100 |
| 8 | Comfort Breeze, 0/1 |

Optional dimmers use attribute 10 for power and 20 for brightness. The same numbers are not assumed to work on arbitrary Hubspace devices.

The narrow FanLamp V2 sender emits nonconnectable legacy BLE advertisements. The controller identity comes from the user's paired remote capture. It saves the rolling counter before transmission, uses a fresh seed, signs the payload, and performs the protocol's whitening/checksum operations. Configuration supports only the signed `fanlamp_pro_v2/r` profile used in the original project.

| Action | Command | Arguments |
|---|---|---|
| Light on | `0x10` | zeros |
| Light off | `0x11` | zeros |
| Warm brightness | `0x21` | `0, 0, round(percent * 255 / 100)` |
| Fan off/speed | `0x31` | `32, speed 0..6, 0` |
| Native breeze | `0x33` | `2, 0, 0` |

Turning breeze off sends a fixed-speed command. While breeze stays enabled, receiver speed fluctuations do not cancel the native mode. Fan power OFF wins over any breeze setting.

## Why there is acknowledgement tracking

A light state write is not atomic on the event stream. Brightness, primary power, and light power can arrive separately and out of order. Combining one fresh field with stale fields caused an OFF command to be followed by an unintended ON.

`receiver_light.h` tracks expected acknowledgements per field, using a bounded eight-entry journal and a 15-second window. Genuine updates are applied as field changes against the current commanded light state. `light_router.h` arbitrates among the receiver and optional switches using their cloud revisions. Stale snapshots and repeated fields must not take control back from a newer source.

Switch commands immediately reach BLE. A separate light-child PUT updates the app tile. Its completion carries a generation number, so an older request cannot clear a newer pending update. Duplicate switch reports neither resend identical BLE commands nor restart the app-sync deadline. OFF updates omit brightness because changing brightness can turn some receivers on.

## Responsiveness and memory

The first standalone version responded well, but adding five blocking REST polls plus synchronous synchronization requests introduced multi-second stalls. Routine live polling was removed. Blocking HTTPS work now runs in a worker task while the main task services push events, routing, BLE, and heartbeats.

The ESP32-C3 also encountered TLS allocation failures despite apparently adequate total free heap. Smaller filtered push documents and allocating the token-response document after the handshake reduced peak pressure. Total free heap and the largest available block both matter. Normal tests subsequently completed repeated token renewals and cloud writes without those failures; this is not a long-term reliability guarantee.

Do not replace NimBLE with a larger Bluetooth stack without measuring memory usage. Do not disable TLS validation to make memory errors disappear. Avoid concurrent HTTP jobs; current cloud work is serialized, with live control serviced during each job.

## Diagnostics

- `READY_BASELINE`: initial state loaded; no physical command was replayed.
- `TOKEN_REFRESHED`: REST authentication renewed without intentionally tearing down push.
- `EVENT light_request` / `EVENT fan_request`: routed intent.
- `EVENT TX_BEGIN` / `TX`: radio start/end; not a fan acknowledgement.
- `APP_SYNC http=200`: cloud accepted a light-tile update.
- `CLOUD_JOB duration_ms`: HTTP work duration, separate from local control responsiveness.
- `HTTP_ERROR ... tls=-32512`: TLS allocation failure in the pinned stack.
- `PUSH_HELLO`, `PUSH_TIMEOUT`, `RETRY`: subscription recovery; intermittent hello retries have been observed.

Logs contain device identifiers and may contain information about household activity. Keep logs local and redact them before sharing an issue. Never post full Hubspace discovery, account tokens, or NVS dumps.

## Reproducibility limits

The public tree keeps the runtime behavior but replaces the original physical/controller IDs with an ignored local header, uses synthetic codec vectors, defaults dimmer mapping to logical 1–100%, and replaces installation-specific provisioning with explicit prompts. It is not a byte-for-byte copy of the original flashed binary. Public-source compilation and automated tests do not substitute for commissioning on a new fan.
