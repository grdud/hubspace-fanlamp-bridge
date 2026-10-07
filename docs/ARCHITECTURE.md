# Architecture and diagnostics

## Control path

REST discovery selects switch meta-devices by configured physical ID and class `switch`. One switch is required; a second is optional. The bridge needs no ceiling-fan parent or receiver child devices. It reads initial switch power, brightness, timestamps and availability, then subscribes to the account's TLS Conclave push stream. Reconnection repeats discovery and seeds state without replaying light commands.

The tested dimmers use raw attribute 10 for power and 20 for brightness. Other products need verification. `light_router.h` compares cloud revisions across two sources; stale reports and repeated fields cannot undo a newer action. The configured logical minimum maps to 1%, and 100 maps to 100%; lower values clamp to 1%. Hardware trim is not necessarily the logical API minimum.

The runtime sends no device-state PUTs. It does not synchronize a separate Hubspace light tile or control the motor. App changes to the selected dimmers arrive through the same push path as physical changes.

## Bluetooth

The signed `fanlamp_pro_v2/r` identity comes from a working remote capture. The sender saves its rolling counter before transmission and uses a fresh seed, signature, whitening and checksum.

| Light action | Command | Arguments |
|---|---|---|
| On | `0x10` | zeros |
| Off | `0x11` | zeros |
| Warm brightness | `0x21` | `0, 0, round(percent * 255 / 100)` |

Each advertisement burst lasts 350 ms. Equivalent power and warm-channel values are deduplicated. Unknown brightness after startup is explicitly set on the first ON. There is no acknowledgement or observation of independent remote changes.

## Responsiveness and memory

There is no routine REST polling. Authentication and initial HTTP work run in a worker task; the main task continues servicing live control and heartbeats. JSON filtering and delayed token-buffer allocation limit TLS memory pressure. HTTPS jobs remain serialized. Keep TLS validation enabled and measure both free heap and largest free blocks when diagnosing allocation failures.

## Diagnostics

Run `python tools/monitor.py --port COM3 --seconds 120`.

- `DISCOVERED_SWITCH_ONLY`: at least one configured switch was found.
- `READY_SWITCH_ONLY`: push connected and initial baseline established without replay.
- `EVENT switch=... ms=...`: switch event processed on the ESP clock.
- `EVENT light_request`: routed intent and cloud revision.
- `EVENT TX_BEGIN` / `TX`: radio start/end, not physical acknowledgement.
- `TOKEN_REFRESHED`: authentication renewed while preserving push.
- `CLOUD_JOB duration_ms`: HTTP duration.
- `PUSH_HELLO`, `PUSH_TIMEOUT`, `RETRY`: subscription recovery.

Subtract switch-event time from TX_BEGIN for local routing-to-radio delay. This excludes physical button-to-cloud delivery, and events queued during a BLE burst may be processed later. Do not interpret it as end-to-end latency.

Logs contain household activity and device identifiers; keep them local and redact before sharing. Never publish discovery exports, tokens or NVS dumps.

## Validation boundary

Switch-only behavior was physically confirmed in the original installation. The public tree uses ignored local identity configuration, synthetic codec vectors and explicit credential prompts. It is not byte-for-byte identical to the installed binary. Automated builds and tests do not establish compatibility with a new fan or switch.
