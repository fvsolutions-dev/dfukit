# dfukit

Firmware updates over any link. dfukit is a small update protocol with a server for the device
and a client for the host, both in C, and a Python package and CLI that drive the C client.

- One protocol for every link: UART, UDP, TCP, BLE, USB, or a link of your own. dfukit sends
  frames; the link only has to carry them, and may lose some.
- A pipeline, not a request per frame: the host sends what the device's window allows, the
  device acknowledges in bulk, and a lost frame is sent again from where it went missing
  (go-back-N).
- Images raw or deflated, decoded on the device as they come in (miniz's inflater, built in).
- A 32-bit board ID per image target: it selects the backend that writes the image, and a
  device refuses IDs it does not know before it touches anything.
- Backends for what is behind a board ID: an ESP-IDF OTA slot, MCUboot's slot and RAM come with
  dfukit; write your own for another slot, or carry the session to a device further down a chain
  (pass-through).
- Resume after a lost link, with the device's state kept for a while.
- No threads, no heap and no platform code in the core: the application feeds it frames and
  time, and gets frames back through a callback.

The protocol and the internals are described in [DESIGN.md](DESIGN.md), and what changed in
[CHANGELOG.md](CHANGELOG.md).

## How an update goes

The host sends `START` with the image's board ID, size and CRC-32. The device prepares, then tells
the host how much data it may send ahead of the acknowledgements: the window. The host streams
`DATA` up to that limit and the device acknowledges as it goes. When a frame is lost, the host goes
back and sends again from the missing point (go-back-N); it does the same after a retransmission
timeout (RTO) without progress. `FINISH` checks the CRC and runs the backend's own checks, and
`ACTIVATE` switches to the new image.

## Adding it to a project

With CMake, from a copy or a submodule of this repository (`dfukit_ram` only if you use the RAM
backend):

```cmake
add_subdirectory(dfukit)
target_link_libraries(app PRIVATE dfukit dfukit_ram)
```

In an ESP-IDF project the repository is a component: add it to `EXTRA_COMPONENT_DIRS` (or
`components/`) and `REQUIRES` it. In a Zephyr build it is a module (`zephyr/module.yml`): add it
to `ZEPHYR_EXTRA_MODULES`, or to one image's `<image>_EXTRA_ZEPHYR_MODULES` with sysbuild, and
turn it on with `CONFIG_DFUKIT`.

## Building and testing

```bash
pip install pytest "nanobind>=2.5"
cmake -S . -B build && cmake --build build && ctest --test-dir build --no-tests=error
```

For the C libraries alone, add `-DDFUKIT_PYTHON=OFF` to the first `cmake`. That build has no tests,
so leave out `ctest`.

The tests run the server and the client against each other through a simulated link that drops,
duplicates and delays frames, and the Python package and CLI against a device in the test process.
They are in Python, on the C code through its bindings; `tests/README.md` has how to measure their
coverage.

## A device: the server

The server is a struct the application owns. Frames go in with `dfukit_server_feed()`, answers
come out through the `send` callback, and `dfukit_server_poll()` runs its timers. One task calls
into a server at a time.

```c
#include <string.h>

#include "dfukit_server.h"

static int send_frame(void *ctx, const dfukit_peer_t *to, const uint8_t *frame, size_t len)
{
	// To the peer `to` on your link (its address, as you gave it to dfukit_server_feed())
	return my_link_send(to, frame, len);
}

static dfukit_server_t server;
static dfukit_backend_t backend;

void update_init(void)
{
	const dfukit_server_config_t config = {
		.send = send_frame,
		.name = "my-device",
		.max_frame = 512, // the largest frame the link carries
	};

	dfukit_server_init(&server, &config);
	my_backend_init(&backend, 0x0000a001); // see "Backends"
	dfukit_server_register(&server, &backend);
}

// For each frame from the link, and every few milliseconds
void on_frame(const uint8_t *addr, size_t addr_len, const uint8_t *frame, size_t len)
{
	dfukit_peer_t from = {.len = (uint8_t)addr_len};

	if (addr_len > DFUKIT_PEER_MAX) {
		return; // longer than a peer: map it to 16 bytes of your own (an index, a hash)
	}
	memcpy(from.bytes, addr, addr_len);
	dfukit_server_feed(&server, &from, frame, len, now_ms());
}

void every_10_ms(void)
{
	dfukit_server_poll(&server, now_ms());
}
```

A peer is an opaque address of up to 16 bytes (`DFUKIT_PEER_MAX`): a socket address, a BLE
connection handle, a bus address; the server drops frames from a longer one. An IPv6 address
with its port is 18 bytes: give the server something shorter that names it (a slot in a table of
your own). Two peers are the same when their bytes are. The peer that starts a session owns it;
another one gets `BUSY` until it ends, unless it resumes the same image (see "Security").
`INFO` and `STATUS` are answered to anyone.

To take deflated images, give the server an inflater and a dictionary as large as the window the
backend announces (32 KiB takes any deflate stream):

```c
#include "dfukit_inflate.h"

static dfukit_inflate_t inflate;
static dfukit_inflater_t inflater;
static uint8_t dict[1u << 15];

dfukit_inflate_init(&inflate, &inflater, dict, sizeof(dict));
// and in the server's config: .inflater = &inflater
```

`on_status` in the config reports the session's state and progress, on every change and every
acknowledgement: for a light or a display. A pass-through session has no progress to report.

`INFO` lists as many boards as fit in one frame, after a name cut to fit. The C client does not send
or parse it: a C application that wants it sends the frame itself and reads the answer (`DESIGN.md`,
Messages). In Python it is `dfukit.info()`. It can warn early, but `START` is the real check.

## Backends

A backend puts an image in place for one board ID (`dfukit_backend_t`, `dfukit_server.h`). The server
calls it in order:

| Callback | Required | When | What it does |
|---|---|---|---|
| `begin` | yes | `START` | gets ready for an image of `start->image_size` bytes (may erase) |
| `write` | yes | data comes in | writes the decoded image at `offset`, in order |
| `window` | yes | every acknowledgement | how many more stream bytes it can take now (0: wait) |
| `finish` | yes | `FINISH` | its own checks of the whole image; `DFUKIT_PENDING` while still writing |
| `activate` | yes | `ACTIVATE` | makes the image the one that runs (restarts, or at the next boot) |
| `abort` | no | the session is broken off | undoes what it can |

The server does not check for missing callbacks: a required one left `NULL` crashes when it is
called. `backends/ram/dfukit_ram.c` is a complete backend to start from. Setting one up looks like
this:

```c
void my_backend_init(dfukit_backend_t *backend, uint32_t board_id)
{
	memset(backend, 0, sizeof(*backend));
	backend->board_id = board_id;
	backend->kind = DFUKIT_KIND_IMAGE;
	backend->max_size = MY_SLOT_SIZE;
	backend->encodings = DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_RAW); // and _DEFLATE, if it fits
	backend->begin = my_begin;
	backend->write = my_write;
	backend->window = my_window;
	backend->finish = my_finish;
	backend->activate = my_activate;
	backend->abort = my_abort;
	backend->ctx = &my_slot;
}
```

The application owns the backend, and it must outlive the server.

The server has checked the image's CRC-32 before it calls `finish`. The CRC catches accidents, not
attackers: checking signatures is up to the backend, which hands the image to whatever checks them
on its platform.

The CRC is a small module of its own (`lib/crc32`). Its `dfukit_crc32()` is weak, so an application
replaces it by defining one (a hardware engine, say), in an object file that gets linked. MSVC
has no weak symbols: there the static library gives way to the application's definition.

The ones that come with dfukit:

| Backend | Platform | Writes |
|---|---|---|
| `ram` | any | a buffer; for tests and for devices that handle the image themselves |
| `esp-idf-ota` | ESP-IDF | the next OTA slot; optionally erased ahead of time (`dfukit_esp_ota_preerase()`) |
| `mcuboot-recovery` | Zephyr, inside MCUboot | MCUboot's slot 0, from a recovery entered at reset |

A pass-through backend (`kind = DFUKIT_KIND_PASSTHROUGH`) does not write anything: `forward`
gets the session's frames as they are, for a device further down a chain, and that device's
answers go back with `dfukit_server_downstream()`. The server does not read the session: it ends
with the owner's `ABORT`, after the idle time, or when the backend calls
`dfukit_server_passthrough_end()`. `abort` is called however it ends. A pass-through backend needs
`forward`; its `abort` is optional, and the image callbacks are not used.

A backend usually ends the session once the device behind it has activated. Hand the answer on
first, because ending the session first drops it without an error:

```c
void on_frame_from_the_device_behind(const uint8_t *frame, size_t len)
{
	dfukit_server_downstream(&server, frame, len, now_ms());
	if (dfukit_frame_activated(frame, len)) {
		dfukit_server_passthrough_end(&server);
	}
}
```

### esp-idf-ota

Writes the next OTA slot, so the partition table needs two OTA slots. `FINISH` runs ESP-IDF's check
of the image, and `ACTIVATE` makes it the boot image and restarts.

```c
#include "dfukit_esp_ota.h"

static dfukit_esp_ota_t ota;

// window 0: 16 KiB. window_log2 12: takes deflate, with an inflater of that size in the server's
// config; 0 takes raw only
ESP_ERROR_CHECK(dfukit_esp_ota_init(&ota, &backend, 0x0000a001, 0, 12));
dfukit_server_register(&server, &backend);
```

Rollback is ESP-IDF's, and off unless `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on: without it a
bad image that boots stays. With it, the new image must call
`esp_ota_mark_app_valid_cancel_rollback()` once it is up, or the next reset goes back to the old
one. Until it does, updates are refused with `BACKEND`, and the device's log says why.

After that confirmation, `dfukit_esp_ota_preerase()` erases the other slot in the background, so the
next update writes without waiting for erases. Asked for while the slot holds an update's image, it
waits for the next update.

### mcuboot-recovery

An update inside MCUboot itself, for devices with a single application slot. MCUboot's own serial
recovery is off; dfukit's Zephyr module uses MCUboot's action hooks to enter its recovery when
the `mcuboot-button0` pin is active at reset or when there is no bootable image. It listens on
the UART chosen as `dfukit,uart`, receives by DMA, and takes its frames encoded with
[burst-link](https://github.com/fvsolutions-dev/burst-link-protocol). `FINISH` runs MCUboot's own
check of the image, so the host hears of an image that will not boot; MCUboot checks it again at
the next boot.

The image needs burst-link as a module next to dfukit's: both in `mcuboot_EXTRA_ZEPHYR_MODULES`
with sysbuild.

```
# MCUboot's configuration (sysbuild: sysbuild/mcuboot.conf)
CONFIG_DFUKIT=y
CONFIG_DFUKIT_MCUBOOT_RECOVERY=y
CONFIG_DFUKIT_MCUBOOT_BOARD_ID=0x0000b001
CONFIG_DMA=y
```

```dts
/* MCUboot's devicetree overlay */
/ {
	chosen {
		dfukit,uart = &lpuart1;
	};
};

&lpuart1 {
	current-speed = <3000000>;
	dmas = <&edma0 2 9>, <&edma0 3 8>;
	dma-names = "rx", "tx";
};
```

The receive buffers (`CONFIG_DFUKIT_MCUBOOT_RX_BUFFER`) must each take the window
(`CONFIG_DFUKIT_MCUBOOT_WINDOW`): the flash's erases stop the interrupts, and everything a host
may have in flight then lands in one buffer.

### Notes for integrators

- The window is a promise the backend makes to the host, not a limit the server enforces: a host
  that sends past it still gets its frames to `write`. A backend that buffers takes more, or fails.
- One inflater serves every backend: its dictionary must take the largest `window_log2` a
  backend announces, else a stream compressed to that window fails at its CRC.
- `begin` runs inside the `START` it answers: it must be done within the client's tries times
  its RTO (4 s by default), or open with a window of 0 and do the rest in the background.
- `activate` only schedules the switch (a restart after `delay_ms`, from a timer or a task):
  the server answers `ACTIVATE` with what it returns, so it must return before restarting.
- `finish` may return `DFUKIT_PENDING` while it still writes what it buffered; the host asks
  again.
- `mcuboot-recovery` writes the flash in whole write units (`flash_area_align()`); the last
  one is padded with the erased value.

## Security

The protocol has no authentication. dfukit leaves this to the link and the backend:

- Anyone who can reach the device can send `START`, and `begin` may erase flash before any
  signature has been checked. Restrict who can reach it, or authenticate the link.
- A peer that knows an image's CRC-32 and sizes can take over a session on that image by resuming
  it. There is no other credential.
- The CRC-32 only catches accidents. To refuse an image that is not yours, the backend or the
  bootloader checks a signature.

## A host: the client

The C client is driven like the server: `dfukit_client_feed()` with the device's frames,
`dfukit_client_poll()` every few milliseconds, frames out through `send`, and the image read
through `read` at any offset (a resend reads again), so it need not be in RAM.

```c
#include "dfukit_client.h"
#include "dfukit_crc32.h"

dfukit_client_t client;
const dfukit_client_config_t config = {
	.send = link_send,
	.read = image_read,
	.max_frame = 512,
};
dfukit_start_t start = {
	.board_id = 0x0000a001,
	.encoding = DFUKIT_ENCODING_RAW,
	.image_size = image_len,
	.stream_size = image_len,
};

start.crc32 = dfukit_crc32(0, image, image_len);
dfukit_client_init(&client, &config);
// true: ACTIVATE after FINISH. 0: activate now (DFUKIT_ACTIVATE_AT_NEXT_BOOT: at the next boot).
// 0: no delay before the switch, in milliseconds
dfukit_client_begin(&client, &start, true, 0, 0, now_ms());
while (dfukit_client_state(&client) < DFUKIT_CLIENT_DONE) {
	// feed what came in, then:
	dfukit_client_poll(&client, now_ms());
}
// DFUKIT_CLIENT_DONE, or DFUKIT_CLIENT_FAILED and dfukit_client_status() says why
```

After a lost link (or a `DFUKIT_TIMEOUT`), `dfukit_client_resume()` on the new one goes on where
the device stopped, or starts over when the device has no state left.

## A host: Python

```bash
pip install dfukit
pip install 'dfukit[serial]'   # with the serial transport (pyserial, burst-link)
```

```python
import dfukit

transport = dfukit.open_transport("udp:192.168.1.20:5000")
board = dfukit.info(transport).board(0x0000A001)  # None when it does not fit INFO's frame

# Compressed for the window the device has: a larger one fails at the end, with a CRC mismatch
update = dfukit.Update(
    transport, image, 0x0000A001, encoding=dfukit.DEFLATE, window_log2=board.window_log2
)
update.run(progress=lambda done, total: print(done, total))
```

`Update.run()` takes a `reconnect` callback that returns a new transport after a lost link; the
update then resumes on it, and gives up after 70 s without progress. Once `ACTIVATE` is sent it does
not reconnect, because a device that restarted would be flashed again: a lost answer raises
`dfukit.ActivationNotConfirmed`, and the device may well be running the new image.

The CLI:

```bash
dfukit info --via udp:192.168.1.20:5000
dfukit update --via serial:/dev/ttyUSB0@3000000 --board 0x0000B001 firmware.bin
```

`--encoding auto` (the default) deflates when the board takes it, with the board's window.

The transports that come with the package are `udp` (one frame per datagram) and `serial`
(burst-link frames). Others come as plugins: a package registers a `dfukit.Transport` subclass
under the entry point `dfukit.transports`, and the CLI finds it by its name.

```python
class MyLinkTransport(dfukit.Transport):
    max_frame = 509

    def open(self, address: str) -> None: ...
    def send(self, frame: bytes) -> None: ...  # hand each received frame to self.received()
    def close(self) -> None: ...
```

```toml
[project.entry-points."dfukit.transports"]
mylink = "my_link.transport:MyLinkTransport"
```

A transport raises `dfukit.LinkError` when its link is gone.

### The client and server directly

For a link and a clock of your own, without a transport: the same client and server as in C, driven
the same way.

The caller provides the link and the clock, in
milliseconds that fit 32 bits. Unlike the C client, `begin()` takes the whole image as bytes.

```python
import time

import dfukit


def now_ms() -> int:
    return int(time.monotonic() * 1000) & 0xFFFFFFFF


client = dfukit.Client(link_send, lambda offset, length: image[offset : offset + length], max_frame=512)
client.begin(0x0000A001, image, now_ms())
while client.state not in (dfukit.ClientState.DONE, dfukit.ClientState.FAILED):
    for frame in frames_from_the_link():
        client.feed(frame, now_ms())
    client.poll(now_ms())
if client.state == dfukit.ClientState.FAILED:
    raise RuntimeError(dfukit.status_name(client.status))
```

`dfukit.Server` is the device's end, with `add()` for a backend (`RamBackend`, `PassthroughBackend`),
`feed(peer, frame, now)` and `poll(now)`; its `send` callback gets `(peer, frame)`.

A deflated image goes the same way: the client streams the compressed bytes, and `begin()` still
takes the image, for its size and CRC. The stream is raw deflate, with a window the device announces:

```python
compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
stream = compressor.compress(image) + compressor.flush()
client = dfukit.Client(link_send, lambda offset, length: stream[offset : offset + length], max_frame=512)
client.begin(0x0000A001, image, now_ms(), stream_size=len(stream), encoding=1)
```

On the server, `server.set_inflater(dfukit.Inflate(15))` turns deflate on.

## License

Apache-2.0 ([LICENSE](LICENSE)). miniz's inflater in `lib/miniz` is MIT
([its LICENSE](lib/miniz/LICENSE)).
