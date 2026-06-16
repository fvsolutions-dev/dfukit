# DFU Session Protocol & layering

The transport-agnostic OTA session layer that sits between `dfu_core` (the OTA writer) and the wire (BLE, CDC, UDP, …). The session logic lives in `dfu_session.{h,c}` inside this component; this document describes the protocol independent of any particular transport so that adding a new transport requires ~100 LOC of new code rather than a re-implementation.

The protocol is **DFU Session Protocol v1.1**; this document pins its wire format.

## Where it sits

```
+──────────────────────────────────────────────────────+
│ Wire transport: BLE GATT / USB CDC / UDP / ...       │
+──────────────────────────────────────────────────────+
                  ↑ ↓  framed bytes (one packet = one protocol message)
+──────────────────────────────────────────────────────+
│ dfu_session  (this document)                         │
│  - state machine                                     │
│  - manifest + SHA verify                             │
│  - sliding-window flow control                       │
│  - staging buffer + pending-feed ring                │
│  - idle watchdog                                     │
+──────────────────────────────────────────────────────+
                  ↑ ↓  dfu_core_acquire/feed/complete
+──────────────────────────────────────────────────────+
│ dfu_core     (esp_ota writer + UF2 parser)           │
+──────────────────────────────────────────────────────+
```

## Protocol — wire format

Each protocol message is one "frame" — a length-delimited byte sequence. How a frame is delimited on the wire is the transport's problem (§ "Per-transport framing" below); to `dfu_session` a frame is opaque between calls to `feed_frame()` / `send()`.

### Downstream (host → device) opcodes

| Op | Value | Payload | Notes |
|---|---|---|---|
| `START` | `0x01` | `proto u8 \| flags u8 \| size u32 \| sha256[32] \| fwver u32 \| hwid u16 \| minfw u32` (48 B) | Begins a session. Acquires dfu_core. |
| `DATA` | `0x10` | `offset u32 \| payload…` | One chunk. `offset` must equal the session's `next_off`; mismatch → `OFFSET_GAP`. |
| `FINISH` | `0x02` | — | All bytes sent; verify and stage. |
| `APPLY` | `0x03` | `delay_ms u16` (optional) | Reboot into the staged slot after `delay_ms` (clamped ≥ 200, default 500). |
| `CONFIRM` | `0x04` | — | After post-flash boot: commit the running slot (`dfu_core_mark_running_valid_after_ms(0)`). Idempotent. |
| `ABORT` | `0x05` | — | Release the session. |
| `GET_STATE` | `0x06` | — | Returns current state + acked + image_size. |

### Upstream (device → host) opcodes

| Op | Value | Frame layout | Notes |
|---|---|---|---|
| `RSP` | downstream-op `\| 0x80` | `status u8 \| extra…` | Reply to each command. |
| `PROGRESS` | `0x90` | `status u8 \| state u8 \| acked u32 \| crc u32` (11 B) | Asynchronous, emitted every `window / 2` bytes acked, also on errors during TRANSFER. |

### Status codes

`OK=0`, `INVALID_STATE=1`, `HW_MISMATCH=2`, `TOO_LARGE=3`, `OFFSET_GAP=4`, `FLASH_ERR=5`, `SHA_MISMATCH=6`, `SIG_INVALID=7`, `TIMEOUT=8`, `BUSY=9`, `DOWNGRADE=10`, `RESUME_INVALID=11`.

### Selected response payloads

- `START` reply (success): `resume_off u32 \| max_payload u16 \| window u32` (10 B). `resume_off` is currently always `0` (see § Limitations).
- `GET_STATE` reply: `state u8 \| acked u32 \| image_size u32` (9 B).
- `FINISH` reply (success): `crc32_acked u32` (4 B) — convenience for the host to verify locally.

## State machine

```
IDLE ──START──▶ TRANSFER ──last DATA + drained──▶ FINISHING ──verified──▶ READY ──APPLY──▶ reboot
   ▲             │                                                          │
   │             ├─ peer loss / idle timeout ──▶ SUSPENDED ──START(same)──▶ (resume)
   │             │                                   │
   │             │                              grace expires
   │             │                                   │
   └─ABORT / disconnect / TIMEOUT / FLASH_ERR ───────┘
```

`CONFIRM` is a separate post-boot operation on the *running* image. It does not transition the state machine; it just commits the active slot.

## SUSPENDED state and resume

When the peer disappears during TRANSFER (BLE disconnect, idle-timeout, link wedge), the session transitions to SUSPENDED instead of aborting. The dfu_core acquire handle, SHA-256 context, CRC state, and acked offset are all preserved; the staging buffer is discarded and `next_off` is rewound to the last flush boundary (the last offset handed to `dfu_core_feed`).

The host may reconnect and re-issue START with the identical manifest (same `size` + `sha256`) within `CONFIG_DFU_SESSION_RESUME_GRACE_MS` (default 60 s). The device replies with `resume_off = next_off` in the START response; the host resumes streaming DATA from that offset. A re-START with a *different* manifest aborts the suspended session and starts a fresh transfer.

If the grace period expires without a matching re-START the partial image is abandoned (`dfu_core_release`), the session returns to IDLE, and the next re-START goes through the full acquire path.

Client recovery order before any START:
1. GET_STATE probe (2 retries). If READY with matching `image_size` → skip directly to APPLY. If FINISHING → poll GET_STATE up to 10 s until state changes. If IDLE or SUSPENDED → fall through to START.
2. Send START. `resume_off` in the response is authoritative; the client resumes DATA from that offset regardless of local bookkeeping.
3. On `OFFSET_GAP` status in a PROGRESS frame, rewind to the `acked` field and resume.

## Sliding-window flow control

- Client tracks `outstanding = bytes_sent − bytes_acked` and keeps it ≤ `window` (advertised in the `START` response, default `CONFIG_DFU_SESSION_WINDOW_BYTES = 16384`).
- Each `DATA` write into the session is forwarded to `dfu_core_feed()`. The session pre-registers the chunk in a pending-feed ring; the `dfu_core` `DFU_EVT_FEED_OK` callback advances `acked` and emits a `PROGRESS` notify every `window / 2` bytes.
- On `STS_OFFSET_GAP`, the client rewinds to `acked` and resumes.

Effective device buffering is `DFU_CORE_SCRATCH_SLOTS * DFU_CORE_SCRATCH_BYTES + staging_bytes`. The window must not exceed `pend_ring_size * staging_bytes` (default 16 × 4 KB = 64 KB), with a sensible default well below.

## Loss / drop handling

The protocol handles most-but-not-all drop cases. Honest enumeration, with the recovery path for each:

| What's dropped | Detected by | Recovery |
|---|---|---|
| `DATA` frame, then another `DATA` arrives | Device — `off != next_off` | Device emits `PROGRESS(STS_OFFSET_GAP)`. Host rewinds to `acked` and resumes. **Clean.** |
| `DATA` frame, and host stops sending (window-paced) | Nothing — device sits silently waiting | `idle_timeout_ms` fires → session aborts. **Not clean** unless host issues `GET_STATE` periodically or knows to keep pinging. |
| Duplicate / out-of-order `DATA` | Device — `off != next_off` | Same as gap: `OFFSET_GAP` → rewind. **Clean** (just bandwidth wasted). |
| `PROGRESS` notify | Nothing — host never knows it lost an ack | Host's `outstanding` calculation stays high, eventually exceeds `window`, host stops sending → idle timeout aborts. **Not clean** unless host polls `GET_STATE`. |
| `RSP` to `START` / `FINISH` / `APPLY` / `CONFIRM` | Nothing — host can't tell if command landed | Host re-sends. Device returns `STS_INVALID_STATE` on re-`START` mid-session, but the existing code treats re-`START` as "client restart" and aborts + re-acquires. Re-`FINISH` in READY returns INVALID_STATE; host should issue `GET_STATE` to see READY and then `APPLY`. **Workable** but the host has to be state-aware. |
| Bytes corrupted in transit | Transport's framing layer (CRC on CDC, controller-level on BLE, UDP checksum) | Frame is discarded by the transport before reaching `dfu_session`. From the session's POV, the frame was simply dropped — see above. |

**The one real gap** in v1.1: a lost `PROGRESS` with the host window-paced. The session sits silent until idle timeout. This is fine for BLE (link-layer retransmits make PROGRESS loss rare) and CDC (BURST framing is byte-reliable as long as the UART doesn't overflow), but is a concern for UDP.

**Recovery: the host polls `GET_STATE`.** The protocol already exposes the necessary primitive — `GET_STATE` (opcode `0x06`) returns `state | acked | image_size`, which is everything the host needs to know whether it lost a PROGRESS or whether the device has genuinely stalled. The required host behaviour is:

- While a transfer is in progress, if the host's `outstanding` window is full *and* no PROGRESS has arrived for some interval (suggested ~1 s on BLE, ~250 ms on UDP), issue `GET_STATE`. The reply's `acked` updates the host's window accounting; the host keeps streaming. No protocol change.
- If `GET_STATE` shows `acked` is *not* advancing across successive polls, the device is genuinely stuck (worker queue stuck, flash error, etc.) — abort and restart.

This adds no device-side complexity (no timer, no seq counter, no coalescing logic). The bandwidth cost is a couple of small extra frames per stall recovery — negligible on any transport. For UDP specifically, the host can be aggressive about `GET_STATE` polling: the round-trip is one datagram each way, and `GET_STATE` itself is idempotent.

**Setting `idle_timeout_ms`** is the other lever — drop it for lossy transports so a genuinely-dead session fails fast. Default 10 s is BLE-tuned; UDP would want something like 3 s.

## Per-transport framing

Each transport delivers exactly one protocol frame per `feed_frame()` call and exactly one per `send()` call. How that mapping happens on the wire is transport-specific:

| Transport | Framing | Per-frame max | Notes |
|---|---|---|---|
| **BLE GATT** (`dfu_ble`) | One characteristic write (down) / notify (up). | ATT MTU − 3. Default 247 → 244 B. | Existing implementation. Subject to `CONFIG_DFU_BLE_REQUIRE_ENC`. |
| **USB CDC** (`dfu_cdc`, new) | COBS + CRC16 BURST envelope around each frame. | A few KB — limited by the staging buffer and CDC TX bufsize. | Reuse the same framing layer your application already uses for CDC streams. |
| **UDP** (`dfu_udp`, new) | One datagram per frame. | One MTU — advertise `~1472` B over Ethernet, `~512` B over LWiP defaults. | No retransmit needed; `STS_OFFSET_GAP` already handles loss/reorder at the application layer. |

A future TCP transport could exist too but is mostly redundant with CDC + UDP for our use cases.

## Layering — file structure

Current state:

```
dfu_core/  (this component)
  dfu_core.{h,c}        OTA writer (raw, UF2, delta auto-detected)
  dfu_core_uf2.{h,c}    UF2 parser (address-dispatched, out-of-order tolerant)
  dfu_session.{h,c}     Session protocol (state machine, SHA, flow control)
  dfu_session_wire.h    On-wire constants and packed structs (canonical)
  Kconfig               DFU_CORE_* and DFU_SESSION_* knobs
```

Each transport adapter is ~100–200 LOC of application code that you write on
top of this component. The adapter wires the transport's I/O to the
`dfu_session_transport_t` vtable and calls `dfu_session_feed_frame` /
`dfu_session_peer_gone`. A BLE adapter, for example, drives the GATT
characteristic write callback into `feed_frame` and the disconnect event into
`peer_gone`.

A future separate component for the UDP or CDC transport would look like:

```
components/dfu_udp/   (transport adapter — your application or a separate component)
  dfu_udp.{h,c}       ~80 LOC — UDP socket, one datagram per frame
  CMakeLists.txt
  Kconfig             PORT, SOCKET_BUFSIZE
```

## Interface contract (`dfu_session.h`)

The session API is documented in `dfu_session.h`. Summary:

```
typedef struct dfu_session dfu_session_t;

typedef struct {
    /* Write one frame to the active peer. Return 0 on success, ESP_FAIL
       on transport-level error. The session never calls this with a
       buffer larger than max_payload(ctx). */
    int      (*send)(void *ctx, const void *buf, size_t len);

    /* Current outbound max frame size. Queried once at START to fill the
       max_payload field of the START response. Transports that don't
       have a real MTU (CDC, UDP) return a sensible static cap. */
    size_t   (*max_payload)(void *ctx);

    /* Optional. Called when the session decides to tear down (timeout,
       abort, fatal error). Transport may drop the peer (BLE disconnect,
       UDP socket reset). May be NULL. */
    void     (*on_release)(void *ctx);

    void          *ctx;        /* transport-private */
    dfu_source_t   source_id;  /* DFU_SRC_BLE / _USB_CDC / _UDP / ... */
    const char    *name;       /* diagnostic label, e.g. "ble" */
} dfu_session_transport_t;

typedef struct {
    uint32_t window_bytes;
    uint32_t idle_timeout_ms;
    uint16_t hw_id;
    /* … room for: min_fwver, downgrade_policy, signed-image hooks … */
} dfu_session_config_t;

/* Lifecycle ----------------------------------------------------------- */
esp_err_t dfu_session_init(dfu_session_t *s,
                           const dfu_session_transport_t *tx,
                           const dfu_session_config_t *cfg);
void      dfu_session_deinit(dfu_session_t *s);

/* Transport → session. One call per received frame. The session does
   its own state-checking, opcode dispatch, dfu_core feeding, response
   generation. */
esp_err_t dfu_session_feed_frame(dfu_session_t *s,
                                 const void *buf, size_t len);

/* Transport tells session the peer disappeared. Idempotent. */
void      dfu_session_peer_gone(dfu_session_t *s);

bool      dfu_session_in_progress(const dfu_session_t *s);
```

The session owns: its mutex, the pending-feed ring, the staging buffer, the SHA context, the dfu_core source binding, the idle/finalize esp_timer handles, the boot counter coordination. The transport owns: its wire plumbing, MTU, and connection state.

## Concurrency contract

Three integration invariants that any session implementation must preserve:

1. **The session mutex MUST NOT be held across `dfu_core_feed()`** — `dfu_core_feed` can block on its own `slot_sem` while the dfu_core worker is blocked inside the session's `core_cb` waiting for the same mutex.
2. **Finalisation runs on the `esp_timer` task**, never inside `core_cb` — `dfu_core_complete()` posts to the worker queue with `portMAX_DELAY`; calling it from the callback risks a queue-full deadlock.
3. **Transport `send()` callbacks are called WITH the session mutex released**, so transports may take their own locks (NimBLE host lock, CDC TX mutex, …) without inversion.

## Adding a new transport adapter

A new transport (e.g. CDC, UDP) follows the same pattern:

1. Allocate a `dfu_session_transport_t` vtable with your `send`, `max_payload`, and optional `on_session_start` / `on_release` callbacks.
2. Call `dfu_session_init(&s, &tx, &cfg)` — the session is heap-allocated and all state is inside it.
3. On each received frame, call `dfu_session_feed_frame(s, buf, len)`.
4. On disconnect / line-down, call `dfu_session_peer_gone(s)`.
5. BLE-specific knobs (encryption, UUIDs) stay in the BLE adapter; only the Kconfig keys starting with `DFU_SESSION_*` belong here.

For CDC: wrap each frame in a COBS+CRC16 BURST envelope so the byte-stream transport delivers clean frame boundaries. For UDP: one datagram = one frame — no framing needed.

## Limitations / future work

- **No image signing.** `SECURE_SIGNED_ON_UPDATE` enforcement happens in `esp_ota_end` (inside dfu_core); the session passes through. For UDP especially, signed images are the practical defence.
- **The session is payload-agnostic.** `dfu_core` auto-detects raw `.bin`, UF2, and delta-patch streams from the first feed; the manifest SHA always covers the wire bytes (the patch, for deltas), while image validity is enforced by dfu_core at completion. Delta patches additionally carry the base image digest, verified against the running partition before any write.
- **UF2 over BLE / CDC / UDP works but is discouraged.** UF2 framing carries no useful information that the manifest doesn't also cover, and its self-completing behaviour interacts poorly with the FINISH/APPLY split: UF2 auto-fires dfu_core_complete internally, so FINISH arrives after the image is already staged — the session transitions to READY without going through FINISHING, and the finalize SHA check runs without the full ack-drain wait.
- **Single-session.** The session is a singleton per dfu_core; only one transport can be acquired at a time. `STS_BUSY` from `START` covers this.
