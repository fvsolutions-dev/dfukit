# dfukit

Firmware updates over any link. dfukit is a small update protocol with a server for the device
and a client for the host, both in C.

- One protocol for every link: UART, UDP, TCP, BLE, USB, or a link of your own. dfukit sends
  frames; the link only has to carry them, and may lose some.
- A pipeline, not a request per frame: the host sends what the device's window allows, the
  device acknowledges in bulk, and a lost frame is sent again from where it went missing
  (go-back-N).
- A 32-bit board ID per image target: it selects the backend that writes the image, and a
  device refuses IDs it does not know before it touches anything.
- Backends for what is behind a board ID: your own for an OTA slot or a bootloader's slot, one
  for RAM that comes with dfukit, or a device further down a chain (pass-through).
- Resume after a lost link, with the device's state kept for a while.
- No threads, no heap and no platform code in the core: the application feeds it frames and
  time, and gets frames back through a callback.

The protocol and the internals are described in [DESIGN.md](DESIGN.md), and what changed in
[CHANGELOG.md](CHANGELOG.md).

## Building and testing

```bash
pip install pytest "nanobind>=2.5"
cmake -S . -B build && cmake --build build && ctest --test-dir build --no-tests=error
```

For the C libraries alone, without the Python module and the tests, add `-DDFUKIT_PYTHON=OFF`.

The tests run the server and the client against each other through a simulated link that drops,
duplicates and delays frames.
They are in Python, on the C code through its bindings; `tests/README.md` has how to measure their
coverage.

## A device: the server

The server is a struct the application owns. Frames go in with `dfukit_server_feed()`, answers
come out through the `send` callback, and `dfukit_server_poll()` runs its timers. One task calls
into a server at a time.

```c
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
another one gets `BUSY` until it ends. `INFO` and `STATUS` are answered to anyone.

`on_status` in the config reports the session's state and progress, on every change and every
acknowledgement: for a light or a display. A pass-through session has no progress to report.

`INFO` lists as many boards as fit in one frame, after a name cut to fit. A host can use it to warn
early, but `START` is the real check.

## Backends

A backend puts an image in place for one board ID (`dfukit_backend_t`, `dfukit_server.h`). The server
calls it in order:

| Callback | When | What it does |
|---|---|---|
| `begin` | `START` | gets ready for an image of `start->image_size` bytes (may erase) |
| `write` | data comes in | writes the image at `offset`, in order |
| `window` | every acknowledgement | how many more stream bytes it can take now (0: wait) |
| `finish` | `FINISH` | its own checks of the whole image; `DFUKIT_PENDING` while still writing |
| `activate` | `ACTIVATE` | makes the image the one that runs (restarts, or at the next boot) |
| `abort` | the session is broken off | undoes what it can |

The server has checked the image's CRC-32 before it calls `finish`. The CRC catches accidents,
not attackers: signatures are the backend's, which hands the image to whatever checks them on its
platform.

The CRC is a small module of its own (`lib/crc32`). Its `dfukit_crc32()` is weak, so an application
replaces it by defining one (a hardware engine, say), in an object file that gets linked. MSVC
has no weak symbols: there the static library gives way to the application's definition.

The ones that come with dfukit:

| Backend | Platform | Writes |
|---|---|---|
| `ram` | any | a buffer; for tests and for devices that handle the image themselves |

A pass-through backend (`kind = DFUKIT_KIND_PASSTHROUGH`) does not write anything: `forward`
gets the session's frames as they are, for a device further down a chain, and that device's
answers go back with `dfukit_server_downstream()`. The server does not read the session: it ends
with the owner's `ABORT`, after the idle time, or when the backend calls
`dfukit_server_passthrough_end()`. `abort` is called however it ends.

### Notes for integrators

- The window is the backend's promise to the host, not a limit the server enforces: a host that
  sends past it still gets its frames to `write`. A backend that buffers takes more, or fails.
- `begin` runs inside the `START` it answers: it must be done within the client's tries times
  its RTO (4 s by default), or open with a window of 0 and do the rest in the background.
- `activate` only schedules the switch (a restart after `delay_ms`, from a timer or a task):
  the server answers `ACTIVATE` with what it returns, so it must return before restarting.
- `finish` may return `DFUKIT_PENDING` while it still writes what it buffered; the host asks
  again.

## A host: the client

The C client is driven like the server: `dfukit_client_feed()` with the device's frames,
`dfukit_client_poll()` every few milliseconds, frames out through `send`, and the image read
through `read` at any offset (a resend reads again), so it need not be in RAM.

```c
#include "dfukit_client.h"

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
dfukit_client_begin(&client, &start, true, 0, 0, now_ms()); // true: ACTIVATE after FINISH
while (dfukit_client_state(&client) < DFUKIT_CLIENT_DONE) {
	// feed what came in, then:
	dfukit_client_poll(&client, now_ms());
}
// DFUKIT_CLIENT_DONE, or DFUKIT_CLIENT_FAILED and dfukit_client_status() says why
```

After a lost link (or a `DFUKIT_TIMEOUT`), `dfukit_client_resume()` on the new one goes on where
the device stopped, or starts over when the device has no state left.

## From Python

```bash
pip install dfukit
```

The same client and server, driven the same way. The link and the clock are the caller's, and the
clock is passed in as milliseconds that fit 32 bits.

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

## License

Apache-2.0 ([LICENSE](LICENSE)).
