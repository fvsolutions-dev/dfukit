# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[2.0.0]: https://github.com/fvsolutions-dev/dfukit/releases/tag/v2.0.0
