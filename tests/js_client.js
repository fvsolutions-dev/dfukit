// src/js/dfukit.js in Node, for test_js_client.py: its frames on stdin and stdout, each a u16 length and
// the bytes. Its options are a JSON argument, the image is a file, and the last line on stderr says
// how it ended

"use strict";

const fs = require("node:fs");
const path = require("node:path");

require(path.join(__dirname, "..", "src", "js", "dfukit.js"));
const {dfukit} = globalThis;

const options = JSON.parse(process.argv[2]);
const image = new Uint8Array(fs.readFileSync(options.image));
let seed = 7;

// The same losses on every run
function dropped() {
	seed = (seed * 1103515245 + 12345) >>> 0;
	return seed / 2 ** 32 < (options.drop ?? 0);
}

function send(frame) {
	if (!dropped()) {
		const header = Buffer.alloc(2);

		header.writeUInt16LE(frame.length);
		process.stdout.write(Buffer.concat([header, Buffer.from(frame)]));
	}
}

async function main() {
	console.error(`crc32 ${dfukit.crc32(image)}`);

	const update = new dfukit.Update({
		send,
		image,
		boardId: options.board,
		deflated: options.deflate ? await dfukit.deflate(image) : null,
		activate: options.activate ?? true,
		rtoMs: options.rtoMs ?? 100,
		tries: options.tries ?? 8,
	});
	let pending = Buffer.alloc(0);

	process.stdin.on("data", chunk => {
		pending = Buffer.concat([pending, chunk]);
		while (pending.length >= 2 && pending.length >= 2 + pending.readUInt16LE(0)) {
			const length = pending.readUInt16LE(0);

			update.received(new Uint8Array(pending.subarray(2, 2 + length)));
			pending = pending.subarray(2 + length);
		}
	});
	try {
		await update.run();
		console.error(`done ${update.stream.length} ${update.resent}`);
	} catch (error) {
		console.error(`failed ${error.message}`);
	}
	process.exit(0);
}

main();
