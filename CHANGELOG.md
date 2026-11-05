# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.2.0] - 2026-11-05

### Added

- `dfukit.Update`, which sends an image through a transport, and `dfukit.info`
- The `dfukit` command, with `info` and `update`
- UDP and serial transports, and other links as plugins through the `dfukit.transports` entry point
- `esp-idf-ota`, a backend for the next OTA slot on ESP-IDF
- `mcuboot-recovery`, an update inside MCUboot on a UART, and a Zephyr module
- A client in JavaScript for web pages (`src/js/dfukit.js`)

## [2.1.0] - 2026-11-04

### Added

- Add support for compressed (Deflated) images, decoded on the device as they come in, with miniz's inflater built in

### Changed

- Wheels are built and tested up to Python 3.15, with cibuildwheel 4. Linux wheels now need glibc
  2.28 or later, and there are no more 32-bit Linux wheels

## [2.0.0] - 2026-11-04

A new protocol and library. Not compatible with dfukit 1.

### Added

- A firmware update protocol for any link: a credit window, go-back-N, resume, a 32-bit board ID
  per image target, and a CRC-32 of each image
- A server and a client in C, without threads, heap or platform code
- A RAM backend, and a pass-through that carries a session to a device further down a chain
- The CRC-32 as a small module (`lib/crc32`) that an application can replace
- A Python interface to the server and the client (`dfukit.Server`, `dfukit.Client`)
- Wheels for Linux, macOS and Windows

[Unreleased]: https://github.com/fvsolutions-dev/dfukit/compare/v2.2.0...HEAD
[2.2.0]: https://github.com/fvsolutions-dev/dfukit/releases/tag/v2.2.0
[2.1.0]: https://github.com/fvsolutions-dev/dfukit/releases/tag/v2.1.0
[2.0.0]: https://github.com/fvsolutions-dev/dfukit/releases/tag/v2.0.0
