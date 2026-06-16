# dfukit — transport-agnostic OTA for ESP-IDF

`dfukit` is a self-contained ESP-IDF component (plus a host-side CLI) that does
over-the-air firmware updates over **any** transport. It has two core layers
and a set of ready-to-use, optional transport adapters:

- **dfu_core** — the OTA flash writer. Accepts a raw binary, UF2 image, or
  delta patch (auto-detected from the first block), writes it to the inactive
  OTA partition, verifies it, and arms a deferred reboot. One worker task owns
  all `esp_ota_*` state.
- **dfu_session** — the session protocol on top of the writer. Adds manifest +
  SHA-256 verification, sliding-window flow control, PROGRESS acks, and
  suspend/resume so a dropped link costs one reconnect rather than a full
  retransfer.
- **transports** — thin adapters that bridge a wire to `dfu_session`. BLE GATT,
  USB-HID, UDP, and USB-MSC (UF2 drag-drop) ship in `src/transports/`, each
  behind its own Kconfig flag and exposing a single init function. USB-DFU is
  a few callbacks you wire into TinyUSB yourself (see the example).

The matching host tool is `dfukit` (`pip install dfukit`); its source lives in
`dfukit-python-interface/` in this repo.

## AI notice

Parts of this project (code, documentation, and tests) were written with the
assistance of AI tools and subsequently reviewed by a human maintainer. Treat
it as you would any third-party code: review before relying on it in
production.

## Repository layout

```
CMakeLists.txt / Kconfig          the IDF component (registered name: dfukit)
src/
  dfu_core.c   dfu_core.h         OTA flash writer (raw / UF2 / delta)
  dfu_session.c                   session protocol
  dfu_session.h  dfu_session_wire.h
  dfu_core_uf2.c  dfu_core_uf2.h  UF2 parser (private)
  transports/
    dfu_ble.c     dfu_ble.h       BLE GATT  (CONFIG_DFU_TRANSPORT_BLE)
    dfu_hid.c     dfu_hid.h       USB-HID   (CONFIG_DFU_TRANSPORT_HID)
    dfu_udp.c     dfu_udp.h       UDP/IP    (CONFIG_DFU_TRANSPORT_UDP)
    dfu_msc_uf2.c dfu_msc_uf2.h   USB-MSC   (CONFIG_DFU_TRANSPORT_MSC)
examples/dfu_example_app/         standalone, fully-compilable example that
                                  exercises all four transports + USB-DFU
dfukit-python-interface/          the host-side `dfukit` CLI (PyPI package)
architecture.md                   how the three layers fit together
DFU-session.md                    full protocol specification
```

Headers and sources live together under `src/`, which is the component's
public include directory — consumers `#include "dfu_core.h"` etc. directly.

## Using it

Add the component to your project (via `idf_component.yml`, or as a submodule +
`EXTRA_COMPONENT_DIRS`), then in `app_main`:

```c
ESP_ERROR_CHECK(dfu_core_init());        // always

#if CONFIG_DFU_TRANSPORT_MSC
    dfu_msc_uf2_register();              // forward MSC sectors to it
#endif
#if CONFIG_DFU_TRANSPORT_HID
    ESP_ERROR_CHECK(dfu_hid_register()); // before the USB stack starts
#endif
#if CONFIG_DFU_TRANSPORT_BLE
    /* NimBLE host must already be up: dfu_ble_register() only adds the
       GATT service. */
    ESP_ERROR_CHECK(dfu_ble_register());
#endif
#if CONFIG_DFU_TRANSPORT_UDP
    /* IP stack must already be up (Wi-Fi/Ethernet); the adapter just binds
       a socket. */
    ESP_ERROR_CHECK(dfu_udp_start());
#endif
```

Each adapter assumes its substrate (NimBLE host, IP stack, TinyUSB) is already
running — it only adds the OTA-specific glue. They are deliberately small: if
the defaults don't fit, copy the adapter `.c`/`.h` into your project and edit
it. The protocol logic stays in the component; only the wire plumbing is yours.

The example in `examples/dfu_example_app/` is the canonical integration and
doubles as a regression target — it brings up all four transports plus USB-DFU
on the smallest firmware that compiles and runs.

## Public API

### dfu_core (flash writer)

```c
esp_err_t dfu_core_init(void);
esp_err_t dfu_core_acquire(dfu_source_t src, dfu_core_cb_t cb, void *user);
esp_err_t dfu_core_feed(dfu_source_t src, const void *data, size_t len);
esp_err_t dfu_core_complete(dfu_source_t src, uint32_t reboot_delay_ms);
void      dfu_core_release(dfu_source_t src);
bool      dfu_core_in_progress(void);
void      dfu_core_report_total(uint32_t total_bytes);
void      dfu_core_mark_running_valid_after_ms(uint32_t delay_ms);
```

Call `dfu_core_init()` once from `app_main`. The `cb` registered at
`acquire()` fires from the worker task with `DFU_EVT_FEED_OK`,
`DFU_EVT_COMPLETE_OK`, or `DFU_EVT_ERROR` after each chunk is processed.

### dfu_session (protocol)

```c
esp_err_t dfu_session_init(dfu_session_t **out,
                           const dfu_session_transport_t *tx,
                           const dfu_session_config_t *cfg);
void      dfu_session_deinit(dfu_session_t *s);
esp_err_t dfu_session_feed_frame(dfu_session_t *s,
                                 const void *buf, size_t len);
void      dfu_session_peer_gone(dfu_session_t *s);
bool      dfu_session_in_progress(const dfu_session_t *s);
```

This is the layer the bundled transports are built on. To write your own
transport: fill in a `dfu_session_transport_t` vtable (`send`, `max_payload`,
optional `on_session_start` / `on_release`), call `dfu_session_init`, forward
each received frame to `dfu_session_feed_frame`, and call
`dfu_session_peer_gone` on disconnect. The four adapters in `src/transports/`
are worked examples of exactly this — typically ~100–200 LOC each.

## Payload formats

- **Raw `.bin`** — the OTA app image directly. Auto-detected if it doesn't
  match UF2 or delta magic.
- **UF2** — address-dispatched, out-of-order/duplicate tolerant. Suitable for
  USB-MSC drag-drop. Auto-detected by the `UF2\n` magic.
- **Delta patch** — detools heatshrink-compressed patch in the `esp_delta_ota`
  container (magic `0xfccdde10`). The base image SHA-256 is verified against
  the running partition before any write. Requires
  `CONFIG_DFU_CORE_DELTA_ENABLE=y` and the `espressif/esp_delta_ota`
  dependency (pulled automatically by the component manager).

## Key Kconfig knobs

| Symbol | Default | Description |
|--------|---------|-------------|
| `DFU_TRANSPORT_BLE` | n | Build the BLE GATT transport adapter |
| `DFU_TRANSPORT_HID` | n | Build the USB-HID transport adapter |
| `DFU_TRANSPORT_UDP` | n | Build the UDP transport adapter |
| `DFU_TRANSPORT_MSC` | n | Build the USB-MSC UF2 transport adapter |
| `DFU_TRANSPORT_HW_ID` | 0x0001 | Hardware ID matched against START manifests |
| `DFU_CORE_DELTA_ENABLE` | y | Accept delta-patch streams |
| `DFU_CORE_PREERASE_INACTIVE` | y | Background-erase inactive slot after commit |
| `DFU_CORE_SCRATCH_SLOTS` | 2 | In-flight scratch buffers (2 × 4 KB) |
| `DFU_CORE_WORKER_STACK_SIZE` | 4096 | OTA worker task stack |
| `DFU_CORE_STATUS_LOG_PERIOD_MS` | 1000 | Periodic progress log period (0 = off) |
| `DFU_SESSION_WINDOW_BYTES` | 16384 | Sliding-window byte budget advertised in START |
| `DFU_SESSION_IDLE_TIMEOUT_MS` | 10000 | Suspend a stalled transfer after N ms |
| `DFU_SESSION_RESUME_GRACE_MS` | 60000 | Grace period for a suspended session to resume |

The BLE adapter has its own sub-options under `DFU_TRANSPORT_BLE`
(service/characteristic UUIDs, `REQUIRE_ENC`); the UDP adapter has
`DFU_TRANSPORT_UDP_PORT`.

## Wire format

The on-wire constants, opcodes, status codes, and packed struct layouts are
the canonical source of truth in `src/dfu_session_wire.h`. The host-side
Python mirror is `session/wire.py` in `dfukit-python-interface/`. If the two
diverge, the size asserts at import time catch it.

See `DFU-session.md` for the full protocol specification: state machine, flow
control, loss/drop handling, and per-transport framing notes.



## License

Apache-2.0.
