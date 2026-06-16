# dfukit

One-stop firmware-update toolkit for ESP devices. A single CLI (and Python
library) that pushes firmware over whichever link you have — BLE, USB-HID,
USB-MSC, USB-DFU, UDP, or the UART ROM bootloader — with optional **delta
updates** that ship only the difference between the running image and the new
one.

It is the host side of a small device-side component (the `dfu_core` /
`dfu_session` ESP-IDF component). The device writes the inactive OTA slot and
reboots into it; dfukit speaks the matching protocol on the host.

## Install

```sh
pip install dfukit            # BLE, HID, MSC, USB-DFU, UDP
pip install dfukit[uart]      # + ESP ROM serial bootloader (pulls in esptool)
```

## Mechanisms & transports

dfukit groups updates into three mechanisms; only the **session** mechanism has
multiple transports (it carries one wire protocol over different links):

| Command | Mechanism | Link | Driver needed | Delta |
|---------|-----------|------|---------------|-------|
| `dfukit ble <img>`      | session  | BLE GATT            | no  | yes |
| `dfukit hid <img>`      | session  | USB-HID (vendor)    | no  | yes |
| `dfukit udp <img>`      | session  | UDP/IP              | no  | yes |
| `dfukit msc <app.uf2>`  | MSC      | USB mass storage    | no  | —   |
| `dfukit dfu-util <img>` | USB-DFU  | USB DFU 1.1 class   | no* | —   |
| `dfukit uart <img>`     | ROM      | UART / RFC 2217     | no  | —   |

\* needs the `dfu-util` binary on PATH.

The session transports share manifest+SHA verification, sliding-window flow
control, and **suspend/resume** — a dropped link costs one reconnect, not the
whole transfer.

## Examples

```sh
# BLE OTA, delta against what the device last received from us, then reboot
dfukit ble firmware.bin --name my-device --delta --apply

# Driverless USB-HID OTA (needs hidraw access — root or a udev rule)
sudo dfukit hid firmware.bin

# USB mass-storage drag-drop (app-only UF2 onto a mounted drive)
dfukit msc firmware-app.uf2 --mount /media/$USER/FWUPDATE

# USB DFU class
dfukit dfu-util firmware.bin

# UART ROM bootloader, local port or serial-over-IP
dfukit uart firmware.bin --port /dev/ttyUSB0
dfukit uart firmware.bin --port rfc2217://gateway:4000

# UDP (device must have an IP)
dfukit udp firmware.bin --host 192.168.1.50

# List BLE devices advertising the OTA service
dfukit scan
```

## Delta updates

`--delta` (session transports) sends only a patch between the image the device
is recorded as running and the new one — typically a few percent of the full
image. dfukit keeps a content-addressed firmware cache (`~/.cache/dfukit`,
override with `DFUKIT_CACHE`) and records what each device runs. The device
verifies the patch's base hash against its own running partition before
applying, and dfukit falls back to a full upload on any mismatch — so delta is
always safe to leave on.

Delta is a **bandwidth** optimisation: it shrinks what crosses the link, which
is decisive on slow or metered connections. On a fast local link the device's
patch-reconstruction work can make a full upload finish sooner — measure for
your case.

## Notes

- **UDP is experimental**: transfers currently reach ~88% then can wedge on a lost-PROGRESS deadlock over a lossy Wi-Fi link (recovery tuning in progress). BLE/HID/MSC/USB-DFU/UART are solid.

- **HID / hidraw permissions**: HID OTA reads/writes `/dev/hidrawN`. Run as root
  or install a udev rule granting your user access to the device's VID/PID.
- **UART over RFC 2217**: the ROM bootloader's auto-reset/strap relies on
  DTR/RTS round-trips; some serial-over-IP bridges drop or reorder these during
  the sync handshake. If sync fails, put the device in download mode manually
  and/or pass `--no-stub`.
- **Identity defaults** (VID/PID, BLE name/UUIDs, hardware ID, flash offset) are
  CLI options — set them for your product; the defaults are placeholders.

## Library use

Every transport implements a small `Transport` interface and the session
mechanism exposes `upload_with_reconnect(...)`, so you can drive updates from
Python instead of the CLI. See `dfukit/session/transports/` for reference
implementations (BLE, HID, UDP).

## License

Apache-2.0.
