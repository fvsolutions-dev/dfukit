// The Python module _dfukit (nanobind), which the package dfukit makes public: the C client and
// server, the ram backend and a pass-through, and their statuses and states as enums.
// Python's callbacks run from inside the C code, which an exception must not unwind:
// it is kept, the C code gets a failure, and it is raised again once the C call has returned

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "dfukit_client.h"
#include "dfukit_crc32.h"
#include "dfukit_inflate.h"
#include "dfukit_ram.h"
#include "dfukit_server.h"

namespace nb = nanobind;
using namespace nb::literals;

// The GIL is held all along: one at a time
static std::exception_ptr raised;

template <typename F> static int guarded(F call)
{
	try {
		return call();
	} catch (...) {
		raised = std::current_exception();
		return -1;
	}
}

static void raise_kept()
{
	if (raised) {
		std::exception_ptr kept = raised;

		raised = nullptr;
		std::rethrow_exception(kept);
	}
}

static nb::bytes as_bytes(const uint8_t *data, size_t len)
{
	return nb::bytes(reinterpret_cast<const char *>(data), len);
}

static const uint8_t *as_data(const nb::bytes &bytes)
{
	return reinterpret_cast<const uint8_t *>(bytes.c_str());
}

// send(frame) returns None, or -1 when it did not go; read(offset, length) the bytes, or None
struct Client {
	dfukit_client_t client;
	nb::object send, read;

	Client(nb::object send_fn, nb::object read_fn, uint16_t max_frame, uint32_t rto_ms,
	       uint8_t tries, uint16_t burst, uint32_t stall_ms)
	    : send(send_fn), read(read_fn)
	{
		dfukit_client_config_t config = {};

		config.send = on_send;
		config.read = on_read;
		config.ctx = this;
		config.max_frame = max_frame;
		config.rto_ms = rto_ms;
		config.tries = tries;
		config.burst = burst;
		config.stall_ms = stall_ms;
		dfukit_client_init(&client, &config);
	}

	static int on_send(void *ctx, const uint8_t *frame, size_t len)
	{
		return guarded([&] {
			nb::object result = static_cast<Client *>(ctx)->send(as_bytes(frame, len));

			return result.is_none() ? 0 : nb::cast<int>(result);
		});
	}

	static int on_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
	{
		return guarded([&] {
			nb::object result = static_cast<Client *>(ctx)->read(offset, len);

			if (result.is_none()) {
				return -1;
			}
			nb::bytes bytes = nb::cast<nb::bytes>(result);

			if (bytes.size() != len) {
				return -1;
			}
			std::memcpy(buf, bytes.c_str(), len);
			return 0;
		});
	}

	// An update of image to board_id: its size and CRC follow from it, unless given (a stream
	// of another size when it is encoded, or a test's wrong values)
	void begin(uint32_t board_id, const nb::bytes &image, uint32_t timestamp,
		   std::optional<uint32_t> stream_size, uint8_t encoding, uint8_t flags,
		   std::optional<uint32_t> image_size, std::optional<uint32_t> crc32, bool activate,
		   uint8_t activate_flags, uint16_t activate_delay_ms)
	{
		dfukit_start_t start = {};

		start.board_id = board_id;
		start.encoding = static_cast<dfukit_encoding_t>(encoding);
		start.flags = flags;
		start.image_size = image_size.value_or(static_cast<uint32_t>(image.size()));
		start.stream_size = stream_size.value_or(static_cast<uint32_t>(image.size()));
		start.crc32 = crc32.value_or(dfukit_crc32(0, as_data(image), image.size()));
		dfukit_client_begin(&client, &start, activate, activate_flags, activate_delay_ms,
				    timestamp);
		raise_kept();
	}

	void feed(const nb::bytes &frame, uint32_t timestamp)
	{
		dfukit_client_feed(&client, as_data(frame), frame.size(), timestamp);
		raise_kept();
	}

	void poll(uint32_t timestamp)
	{
		dfukit_client_poll(&client, timestamp);
		raise_kept();
	}

	void resume(uint32_t timestamp)
	{
		dfukit_client_resume(&client, timestamp);
		raise_kept();
	}
};

// What a server registers
struct Backend {
	dfukit_backend_t backend = {};
	std::string version;
};

struct Ram : Backend {
	dfukit_ram_t ram;
	std::vector<uint8_t> buffer;

	Ram(uint32_t board_id, size_t size, std::optional<std::string> version_text) : buffer(size)
	{
		dfukit_ram_init(&ram, &backend, board_id, buffer.data(), size);
		if (version_text) {
			version = *version_text;
			backend.version = version.c_str();
		}
	}

	// How it behaves; what is not given stays
	void behave(std::optional<uint32_t> window, std::optional<bool> busy,
		    std::optional<unsigned> pending, std::optional<bool> refuse_activate)
	{
		ram.window = window.value_or(ram.window);
		ram.busy = busy.value_or(ram.busy);
		ram.pending = pending.value_or(ram.pending);
		ram.refuse_activate = refuse_activate.value_or(ram.refuse_activate);
	}
};

// forward(frame) returns None, or -1 when it did not go; end() when the session is over
struct Passthrough : Backend {
	nb::object forward, end;

	Passthrough(uint32_t board_id, uint32_t max_size, nb::object forward_fn, nb::object end_fn)
	    : forward(forward_fn), end(end_fn)
	{
		backend.board_id = board_id;
		backend.kind = DFUKIT_KIND_PASSTHROUGH;
		backend.max_size = max_size;
		backend.ctx = this;
		backend.forward = on_forward;
		backend.abort = on_end;
	}

	static int on_forward(void *ctx, const uint8_t *frame, size_t len)
	{
		return guarded([&] {
			nb::object result =
				static_cast<Passthrough *>(ctx)->forward(as_bytes(frame, len));

			return result.is_none() ? 0 : nb::cast<int>(result);
		});
	}

	static void on_end(void *ctx)
	{
		guarded([&] {
			static_cast<Passthrough *>(ctx)->end();
			return 0;
		});
	}
};

// The deflate decoder, for a server
struct Inflate {
	dfukit_inflate_t inflate;
	dfukit_inflater_t inflater;
	std::vector<uint8_t> dict;

	explicit Inflate(unsigned window_log2) : dict(size_t{1} << window_log2)
	{
		dfukit_inflate_init(&inflate, &inflater, dict.data(), dict.size());
	}
};

// send(peer, frame) returns None, or -1 when it did not go; status(state, done, total, activated)
struct Server {
	dfukit_server_t server;
	nb::object send, status;
	std::string name;

	Server(nb::object send_fn, nb::object status_fn, std::string name_text, uint16_t max_frame)
	    : send(send_fn), status(status_fn), name(std::move(name_text))
	{
		dfukit_server_config_t config = {};

		config.send = on_send;
		config.ctx = this;
		config.name = name.c_str();
		config.max_frame = max_frame;
		if (!status.is_none()) {
			config.on_status = on_status;
		}
		dfukit_server_init(&server, &config);
	}

	static int on_send(void *ctx, const dfukit_peer_t *to, const uint8_t *frame, size_t len)
	{
		return guarded([&] {
			nb::object result = static_cast<Server *>(ctx)->send(
				as_bytes(to->bytes, to->len), as_bytes(frame, len));

			return result.is_none() ? 0 : nb::cast<int>(result);
		});
	}

	static void on_status(void *ctx, const dfukit_server_status_t *current)
	{
		guarded([&] {
			static_cast<Server *>(ctx)->status(current->state, current->done,
							   current->total, current->activated);
			return 0;
		});
	}

	void set_inflater(Inflate &inflate)
	{
		server.config.inflater = &inflate.inflater;
	}

	void add(Backend &backend)
	{
		dfukit_server_register(&server, &backend.backend);
	}

	void feed(const nb::bytes &peer, const nb::bytes &frame, uint32_t timestamp)
	{
		dfukit_peer_t from = {};

		// As the C API's: a peer longer than its bytes is dropped
		if (peer.size() > DFUKIT_PEER_MAX) {
			return;
		}
		from.len = static_cast<uint8_t>(peer.size());
		std::memcpy(from.bytes, peer.c_str(), peer.size());
		dfukit_server_feed(&server, &from, as_data(frame), frame.size(), timestamp);
		raise_kept();
	}

	void poll(uint32_t timestamp)
	{
		dfukit_server_poll(&server, timestamp);
		raise_kept();
	}

	void downstream(const nb::bytes &frame, uint32_t timestamp)
	{
		dfukit_server_downstream(&server, as_data(frame), frame.size(), timestamp);
		raise_kept();
	}

	void passthrough_end()
	{
		dfukit_server_passthrough_end(&server);
		raise_kept();
	}
};

// A status as its enum; one this version does not know, from a newer device, as its number
static nb::object status_object(dfukit_status_t status)
{
	try {
		return nb::cast(status);
	} catch (const std::exception &) {
		return nb::cast(static_cast<int>(status));
	}
}

NB_MODULE(_dfukit, module)
{
	nb::set_leak_warnings(false);

	nb::enum_<dfukit_status_t>(module, "Status", nb::is_arithmetic())
		.value("OK", DFUKIT_OK)
		.value("GAP", DFUKIT_GAP)
		.value("BUSY", DFUKIT_BUSY)
		.value("UNKNOWN_BOARD", DFUKIT_UNKNOWN_BOARD)
		.value("TOO_LARGE", DFUKIT_TOO_LARGE)
		.value("NO_RESUME", DFUKIT_NO_RESUME)
		.value("BAD_STREAM", DFUKIT_BAD_STREAM)
		.value("CRC_MISMATCH", DFUKIT_CRC_MISMATCH)
		.value("BACKEND", DFUKIT_BACKEND)
		.value("NO_SESSION", DFUKIT_NO_SESSION)
		.value("BAD_FRAME", DFUKIT_BAD_FRAME)
		.value("PENDING", DFUKIT_PENDING)
		.value("TIMEOUT", DFUKIT_TIMEOUT);
	nb::enum_<dfukit_client_state_t>(module, "ClientState", nb::is_arithmetic())
		.value("IDLE", DFUKIT_CLIENT_IDLE)
		.value("STARTING", DFUKIT_CLIENT_STARTING)
		.value("SENDING", DFUKIT_CLIENT_SENDING)
		.value("FINISHING", DFUKIT_CLIENT_FINISHING)
		.value("ACTIVATING", DFUKIT_CLIENT_ACTIVATING)
		.value("DONE", DFUKIT_CLIENT_DONE)
		.value("FAILED", DFUKIT_CLIENT_FAILED);
	nb::enum_<dfukit_session_state_t>(module, "SessionState", nb::is_arithmetic())
		.value("IDLE", DFUKIT_SESSION_IDLE)
		.value("TRANSFER", DFUKIT_SESSION_TRANSFER)
		.value("SUSPENDED", DFUKIT_SESSION_SUSPENDED)
		.value("READY", DFUKIT_SESSION_READY)
		.value("PASSTHROUGH", DFUKIT_SESSION_PASSTHROUGH);

	module.def(
		"crc32",
		[](const nb::bytes &data, uint32_t crc) {
			return dfukit_crc32(crc, as_data(data), data.size());
		},
		"data"_a, "crc"_a = 0);
	module.def("status_name", [](int status) {
		return std::string(dfukit_status_name(static_cast<dfukit_status_t>(status)));
	});
	module.def("frame_activated", [](const nb::bytes &frame) {
		return dfukit_frame_activated(as_data(frame), frame.size());
	});

	nb::class_<Client>(module, "Client")
		.def(nb::init<nb::object, nb::object, uint16_t, uint32_t, uint8_t, uint16_t,
			      uint32_t>(),
		     "send"_a, "read"_a, "max_frame"_a = 0, "rto_ms"_a = 0, "tries"_a = 0,
		     "burst"_a = 0, "stall_ms"_a = 0)
		.def("begin", &Client::begin, "board_id"_a, "image"_a, "timestamp"_a,
		     "stream_size"_a = nb::none(), "encoding"_a = 0, "flags"_a = 0,
		     "image_size"_a = nb::none(), "crc32"_a = nb::none(), "activate"_a = true,
		     "activate_flags"_a = 0, "activate_delay_ms"_a = 0)
		.def("feed", &Client::feed)
		.def("poll", &Client::poll)
		.def("resume", &Client::resume)
		.def_prop_ro("state",
			     [](Client &self) { return dfukit_client_state(&self.client); })
		.def_prop_ro("status",
			     [](Client &self) {
				     return status_object(dfukit_client_status(&self.client));
			     })
		.def_prop_ro("acked",
			     [](Client &self) { return dfukit_client_acked(&self.client); })
		.def_prop_ro("resent", [](Client &self) { return self.client.resent; })
		.def_prop_ro("max_data", [](Client &self) { return self.client.max_data; });

	nb::class_<Backend>(module, "Backend");

	nb::class_<Ram, Backend>(module, "RamBackend")
		.def(nb::init<uint32_t, size_t, std::optional<std::string>>(), "board_id"_a,
		     "size"_a, "version"_a = nb::none())
		.def("behave", &Ram::behave, "window"_a = nb::none(), "busy"_a = nb::none(),
		     "pending"_a = nb::none(), "refuse_activate"_a = nb::none())
		.def_prop_ro(
			"image",
			[](Ram &self) { return as_bytes(self.buffer.data(), self.ram.written); })
		.def_prop_ro("written", [](Ram &self) { return self.ram.written; })
		.def_prop_ro("finished", [](Ram &self) { return self.ram.finished; })
		.def_prop_ro("activated", [](Ram &self) { return self.ram.activated; })
		.def_prop_ro("begins", [](Ram &self) { return self.ram.begins; })
		.def_prop_ro("aborts", [](Ram &self) { return self.ram.aborts; })
		.def_prop_ro("pending", [](Ram &self) { return self.ram.pending; });

	nb::class_<Passthrough, Backend>(module, "PassthroughBackend")
		.def(nb::init<uint32_t, uint32_t, nb::object, nb::object>(), "board_id"_a,
		     "max_size"_a, "forward"_a, "end"_a);

	nb::class_<Inflate>(module, "Inflate").def(nb::init<unsigned>(), "window_log2"_a);

	nb::class_<Server>(module, "Server")
		.def(nb::init<nb::object, nb::object, std::string, uint16_t>(), "send"_a,
		     "on_status"_a = nb::none(), "name"_a = "", "max_frame"_a = 512)
		.def("add", &Server::add, nb::keep_alive<1, 2>())
		.def("set_inflater", &Server::set_inflater, nb::keep_alive<1, 2>())
		.def("feed", &Server::feed)
		.def("poll", &Server::poll)
		.def("downstream", &Server::downstream)
		.def("passthrough_end", &Server::passthrough_end)
		.def_prop_ro("state",
			     [](Server &self) { return dfukit_server_state(&self.server); })
		.def_prop_ro("session", [](Server &self) { return self.server.session; })
		.def_prop_ro("taken", [](Server &self) { return self.server.expected; });
}
