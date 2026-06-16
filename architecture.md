# Architecture

dfukit does one thing: take firmware bytes from somewhere and land them safely
in the inactive OTA partition. "Somewhere" is deliberately abstract — BLE, USB,
UDP, a file on a drive — so the interesting bytes-to-flash machinery is written
once and every transport reuses it.

The design is three layers, each unaware of the one above it:

```
   transports        BLE GATT · USB-HID · UDP · USB-MSC · USB-DFU
        │            wire plumbing only; turn packets into frames/bytes
        ▼
   dfu_session       the streaming protocol: manifest + SHA, sliding-window
        │            flow control, PROGRESS acks, suspend/resume
        ▼
   dfu_core          the flash writer: raw/UF2/delta, esp_ota, rollback
```

A transport never touches `esp_ota`. The session never touches a socket. The
writer never knows whether the bytes arrived over Bluetooth or a USB drive.
That separation is the whole point: a new transport is ~100–200 lines that
bridge a wire to the session, and it inherits verification, flow control, and
crash recovery for free.

## The writer — `dfu_core`

`dfu_core` owns all `esp_ota_*` state behind a single worker task. Callers
`acquire()` it (one source at a time), `feed()` bytes, and `complete()`; a
callback reports progress and the terminal result from the worker.

What it handles so callers don't have to:

- **Format auto-detection.** The first bytes of a stream decide the path: a
  raw `.bin`, a UF2 image (`UF2\n` magic, address-dispatched and tolerant of
  out-of-order or duplicated blocks), or a delta patch (`esp_delta_ota`
  container). Callers feed bytes; the writer figures out the rest.
- **Delta patches** are applied against the *running* partition, and the
  patch's embedded base SHA-256 is checked against it before a single byte is
  written — a patch built for a different base is rejected, not half-applied.
- **Lazy erase.** Writing through `esp_partition_write` with per-sector erase
  tracking avoids `esp_ota_begin`'s blocking full-partition erase. An optional
  background pre-erase of the inactive slot after commit makes the next
  update's writes erase-free.
- **Rollback safety.** Rollback is expected to be on. The running image is only
  committed (`esp_ota_mark_app_valid_cancel_rollback`) a few seconds into a
  healthy boot, so an image that crashes early reverts on the next reset.

## The session — `dfu_session`

Streaming firmware over a lossy or droppable link needs more than "send bytes."
`dfu_session` sits on top of the writer and adds the protocol that makes a
transfer survive the real world:

- A **manifest** (size, SHA-256, hardware ID, version gates) sent up front, so
  a wrong-board or already-installed image is rejected before any flash.
- **Sliding-window flow control** with periodic PROGRESS acks, so a fast host
  can't outrun the device's buffering.
- **Suspend/resume.** A dropped link mid-transfer doesn't abandon the image:
  the session parks, keeps its SHA/CRC/offset state, and a reconnect with the
  same manifest resumes from the last flush boundary. A lost link costs one
  reconnect, not a full retransfer.

The session is transport-agnostic by construction: it speaks in opaque
*frames* and leaves framing to the transport. The complete wire format —
opcodes, packed layouts, state machine, loss handling — is specified in
[`DFU-session.md`](DFU-session.md). The host side mirrors the same constants in
`dfukit-python-interface/`, with size asserts that fire at import if the two
ever drift.

## The transports

Two kinds ride the writer:

**Session transports** carry the streaming protocol and inherit all of its
recovery. Four ship in `src/transports/`, each behind its own Kconfig flag and
exposing one init function:

| Transport | Kconfig | Init | Notes |
|-----------|---------|------|-------|
| BLE GATT | `DFU_TRANSPORT_BLE` | `dfu_ble_register()` | single write/notify characteristic |
| USB-HID | `DFU_TRANSPORT_HID` | `dfu_hid_register()` | driverless, 64-byte reports |
| UDP | `DFU_TRANSPORT_UDP` | `dfu_udp_start()` | one datagram = one frame |
| USB-MSC | `DFU_TRANSPORT_MSC` | `dfu_msc_uf2_register()` | UF2 drag-drop (full images only) |

**Direct transports** skip the session and feed the writer straight:

- **USB-MSC UF2 drag-drop** — the device serves a tiny virtual drive; dropping
  a `.uf2` onto it streams sectors into the writer. No session, no acks; UF2's
  own structure tolerates the host's write ordering. Full images only.
- **USB-DFU 1.1** — the standard DFU class, driven by `dfu-util`. A handful of
  TinyUSB callbacks forward download blocks into the writer.

Each adapter assumes its substrate is already running — the NimBLE host is up,
the IP stack has an address, TinyUSB is initialized. The adapter adds only the
OTA-specific glue, never the stack bring-up. They are intentionally small and
meant to be read: if a default doesn't fit your product, copy the adapter into
your project and edit it. The protocol stays in the component; only the wire
plumbing becomes yours.

## The host side

`dfukit` (in `dfukit-python-interface/`, published to PyPI) is the counterpart
tool. One command per mechanism — `ble`, `hid`, `udp`, `msc`, `dfu-util`,
`uart`, `scan` — over a shared session implementation. Delta encoding and a
firmware cache live in the tool, so any session transport can opt into delta
uploads without device-side changes.

## Notes worth keeping

- **One update at a time.** The writer is a singleton resource; a second source
  attempting to acquire mid-transfer is rejected. This keeps `esp_ota` state
  unambiguous and is surfaced to clients as a busy status.
- **Delta saves bytes, not necessarily time.** A delta is a few percent of the
  full image over the air, but applying it still writes the whole image to
  flash. The win is bandwidth and airtime on constrained links.
- **MSC and stale FAT.** After repeated drag-drops a host may cache the
  filesystem and report "no space." The device serves a fresh volume on each
  enumeration, so re-mounting clears it.
- **BLE link stability needs a good low-power clock.** Long-lived connections
  driven off an internal RC clock drift enough to break; battery designs want a
  32.768 kHz crystal or a main-XTAL low-power clock source.
