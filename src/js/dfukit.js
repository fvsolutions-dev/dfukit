// dfukit's client in JavaScript, for a browser or Node: the C client's logic (src/dfukit_client.c)
// over any link that carries frames. Plain script: it defines globalThis.dfukit.
//
//   const update = new dfukit.Update({send: frame => link.send(frame), image, boardId: 0xa001});
//   link.onframe = frame => update.received(frame);
//   await update.run(); // rejects with a dfukit.UpdateError (its .status, its .message)
//
// The image raw, or deflated first (dfukit.deflate(), the browser's CompressionStream), for a board
// that takes a 32 KiB window.

"use strict";

(() => {
	const OP = {INFO: 0x01, START: 0x02, DATA: 0x03, FINISH: 0x04, ACTIVATE: 0x05, ABORT: 0x06,
		    ANSWER: 0x80};
	const ACK = OP.ANSWER | OP.DATA;
	const DATA_HEADER = 7;
	const STATUS = [
		"OK", "GAP", "BUSY", "UNKNOWN_BOARD", "TOO_LARGE", "NO_RESUME", "BAD_STREAM",
		"CRC_MISMATCH", "BACKEND", "NO_SESSION", "BAD_FRAME", "PENDING",
	];
	const OK = 0, GAP = 1, BUSY = 2, NO_RESUME = 5, PENDING = 11, TIMEOUT = 0x7f;
	const RAW = 0, DEFLATE = 1;
	const START_RESUME = 0x01;

	// A status's name, for messages
	function statusName(status) {
		return status === TIMEOUT ? "TIMEOUT" : (STATUS[status] ?? `status ${status}`);
	}

	// Why an update failed: .status, and .message with the device's reason if it gave one
	class UpdateError extends Error {
		constructor(status, reason = "") {
			super(statusName(status) + (reason ? `: ${reason}` : ""));
			this.status = status;
		}
	}

	// CRC-32 (IEEE 802.3, as zlib's) of bytes
	function crc32(bytes) {
		let crc = ~0;

		for (const byte of bytes) {
			crc ^= byte;
			for (let bit = 0; bit < 8; bit++) {
				crc = crc >>> 1 ^ (crc & 1 ? 0xedb88320 : 0);
			}
		}
		return ~crc >>> 0;
	}

	// Raw deflate (no header) of bytes, with a 32 KiB window: the browser's own (and Node's)
	async function deflate(bytes) {
		const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("deflate-raw"));
		return new Uint8Array(await new Response(stream).arrayBuffer());
	}

	// One update: an image to a board ID, through send(frame), its answers into received(frame)
	class Update {
		constructor({send, image, boardId, deflated = null, activate = true, activateFlags = 0,
			     activateDelayMs = 0, maxFrame = 509, rtoMs = 500, tries = 8,
			     stallMs = 5 * 60 * 1000, onProgress = null}) {
			this.sendFrame = send;
			this.image = image;
			this.boardId = boardId;
			// The stream: the image, or its deflated form
			this.stream = deflated ?? image;
			this.encoding = deflated ? DEFLATE : RAW;
			this.activate = activate;
			this.activateFlags = activateFlags;
			this.activateDelayMs = activateDelayMs;
			this.maxFrame = Math.min(maxFrame, 1024);
			this.rtoMs = rtoMs;
			this.tries = tries;
			this.stallMs = stallMs;
			this.onProgress = onProgress;
			this.state = "idle";
			this.resent = 0;
		}

		// Until the device has the image (activated, with activate): resolves, or rejects with an
		// UpdateError
		run() {
			const now = Date.now();

			this.crc = crc32(this.image);
			this.flags = 0;
			this.session = 0;
			this.acked = 0;
			this.next = 0;
			this.window = 0;
			this.triesLeft = this.tries;
			this.progressTime = now;
			this.movedTime = now;
			this.state = "starting";
			return new Promise((resolve, reject) => {
				this.resolve = resolve;
				this.reject = reject;
				this.sendStart(now);
				this.timer = setInterval(() => this.poll(Date.now()), 5);
			});
		}

		// The link was lost: on a new one, from where the device is. Not once ACTIVATE is sent: a
		// device that restarted has no session left, and would be flashed again
		resume() {
			if (this.state === "activating") {
				this.fail(TIMEOUT, "the image was sent, but its activation is not confirmed");
			} else if (["sending", "starting", "finishing"].includes(this.state)) {
				this.flags |= START_RESUME;
				this.triesLeft = this.tries;
				this.movedTime = Date.now();
				this.state = "starting";
				this.sendStart(Date.now());
			}
		}

		// A frame from the device
		received(frame) {
			const now = Date.now();
			const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);

			if (frame.length < 2) {
				return;
			}
			switch (frame[0]) {
			case OP.ANSWER | OP.START:
				this.onStartAnswer(frame, view, now);
				break;
			case ACK:
				this.onAck(frame, view, now);
				break;
			case OP.ANSWER | OP.FINISH:
				if (this.state !== "finishing") {
					break;
				}
				if (frame[1] === PENDING) {
					this.triesLeft = this.tries;
					this.requestTime = now;
				} else if (frame[1] !== OK) {
					const reason = frame.length > 3
						? new TextDecoder().decode(frame.subarray(3, 3 + frame[2])) : "";
					this.fail(frame[1], reason);
				} else if (this.activate) {
					this.state = "activating";
					this.movedTime = now;
					this.triesLeft = this.tries;
					this.sendSessionOp(OP.ACTIVATE, now);
				} else {
					this.done();
				}
				break;
			case OP.ANSWER | OP.ACTIVATE:
				if (this.state === "activating") {
					frame[1] === OK ? this.done() : this.fail(frame[1]);
				}
				break;
			}
			this.poll(now);
		}

		// START's answer: the session and where to go on from; without state, from the start
		onStartAnswer(frame, view, now) {
			if (this.state !== "starting" || frame.length < 14) {
				return;
			}
			const status = frame[1];

			// Nothing to resume from: from the start
			if (status === NO_RESUME && (this.flags & START_RESUME)) {
				this.flags &= ~START_RESUME;
				this.triesLeft = this.tries;
				this.sendStart(now);
				return;
			}
			if (status !== OK) {
				this.fail(status);
				return;
			}
			this.session = view.getUint16(2, true);
			this.maxData = Math.min(view.getUint16(12, true), this.maxFrame - DATA_HEADER);
			if (this.maxData <= 0) {
				this.fail(0x0a);
				return;
			}
			this.acked = view.getUint32(4, true);
			this.next = this.acked;
			this.window = view.getUint32(8, true);
			this.progressTime = now;
			this.movedTime = now;
			this.triesLeft = this.tries;
			this.state = "sending";
			this.finishIfDone(now);
		}

		// An ACK: how far the device is, how much more it allows, and whether a frame was lost
		onAck(frame, view, now) {
			if (this.state !== "sending" || frame.length < 12 ||
			    view.getUint16(1, true) !== this.session) {
				return;
			}
			const status = frame[3];
			const offset = view.getUint32(4, true);

			if (status !== OK && status !== GAP) {
				this.fail(status);
				return;
			}
			if (offset < this.acked) {
				return; // overtaken by a later one
			}
			this.triesLeft = this.tries;
			if (offset > this.acked) {
				this.acked = offset;
				this.progressTime = now;
				this.movedTime = now;
				this.onProgress?.(this.acked, this.stream.length);
			}
			this.window = view.getUint32(8, true);
			if (status === GAP &&
			    !(offset === this.rewoundTo && now - this.rewoundTime < this.rtoMs)) {
				// A frame was lost: everything after it again
				if (this.next > offset) {
					this.resent += this.next - offset;
				}
				this.next = offset;
				this.rewoundTo = offset;
				this.rewoundTime = now;
				this.progressTime = now;
			}
			this.next = Math.max(this.next, this.acked);
			this.finishIfDone(now);
		}

		// All of the stream acknowledged: FINISH
		finishIfDone(now) {
			if (this.state === "sending" && this.acked === this.stream.length) {
				this.state = "finishing";
				this.movedTime = now;
				this.triesLeft = this.tries;
				this.sendSessionOp(OP.FINISH, now);
			}
		}

		// Time passes: frames as the window allows, and resends
		poll(now) {
			if (["idle", "done", "failed"].includes(this.state)) {
				return;
			}
			if (now - this.movedTime >= this.stallMs) {
				this.fail(TIMEOUT);
				return;
			}
			if (this.state === "sending") {
				// No progress for the RTO: from the acked offset again, or a probe
				if (now - this.progressTime >= this.rtoMs) {
					if (this.triesLeft === 0) {
						this.fail(TIMEOUT);
						return;
					}
					this.triesLeft--;
					this.progressTime = now;
					if (this.next > this.acked) {
						this.resent += this.next - this.acked;
						this.next = this.acked;
					} else {
						this.sendData(this.acked, 0);
					}
				}
				const limit = Math.min(this.acked + this.window, this.stream.length);

				while (this.next < limit) {
					const length = Math.min(this.maxData, limit - this.next);

					this.sendData(this.next, length);
					this.next += length;
				}
			} else if (now - this.requestTime >= this.rtoMs) {
				// A request without an answer: again, until the tries are used up
				if (this.triesLeft === 0) {
					this.fail(TIMEOUT, this.state === "activating"
						? "the image was sent, but its activation is not confirmed" : "");
				} else if (this.state === "starting") {
					this.sendStart(now);
				} else {
					this.sendSessionOp(this.state === "finishing" ? OP.FINISH : OP.ACTIVATE, now);
				}
			}
		}

		// START: which image, for which board
		sendStart(now) {
			const frame = new Uint8Array(19);
			const view = new DataView(frame.buffer);

			frame[0] = OP.START;
			view.setUint32(1, this.boardId, true);
			frame[5] = this.encoding;
			frame[6] = this.flags;
			view.setUint32(7, this.image.length, true);
			view.setUint32(11, this.stream.length, true);
			view.setUint32(15, this.crc, true);
			this.send(frame);
			this.requestTime = now;
			this.triesLeft--;
		}

		// FINISH or ACTIVATE
		sendSessionOp(op, now) {
			const frame = new Uint8Array(op === OP.ACTIVATE ? 6 : 3);
			const view = new DataView(frame.buffer);

			frame[0] = op;
			view.setUint16(1, this.session, true);
			if (op === OP.ACTIVATE) {
				frame[3] = this.activateFlags;
				view.setUint16(4, this.activateDelayMs, true);
			}
			this.send(frame);
			this.requestTime = now;
			this.triesLeft--;
		}

		// A piece of the stream; length 0 is a probe
		sendData(offset, length) {
			const frame = new Uint8Array(DATA_HEADER + length);
			const view = new DataView(frame.buffer);

			frame[0] = OP.DATA;
			view.setUint16(1, this.session, true);
			view.setUint32(3, offset, true);
			frame.set(this.stream.subarray(offset, offset + length), DATA_HEADER);
			this.send(frame);
		}

		// To the link, which may not be able to send now
		send(frame) {
			try {
				this.sendFrame(frame);
			} catch {
				// A link that cannot send now: the resends cover it
			}
		}

		// The update is over, and it worked
		done() {
			this.state = "done";
			clearInterval(this.timer);
			this.resolve();
		}

		// The update is over, and it did not work
		fail(status, reason = "") {
			this.state = "failed";
			clearInterval(this.timer);
			// Over: the device lets go at once, without its idle time. Not when the link
			// may be back for a resume, and a session that is someone else's stays theirs
			if (status !== TIMEOUT && status !== BUSY) {
				const frame = new Uint8Array(3);

				frame[0] = OP.ABORT;
				new DataView(frame.buffer).setUint16(1, this.session, true);
				this.send(frame);
			}
			this.reject(new UpdateError(status, reason));
		}
	}

	globalThis.dfukit = {Update, UpdateError, crc32, deflate, statusName, RAW, DEFLATE};
})();
