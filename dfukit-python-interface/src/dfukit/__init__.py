# dfukit — one-stop firmware-update toolkit.
#
# Mechanisms:
#   - session/  DFU Session Protocol v1.1 (transports: BLE, HID, UDP, …)
#   - msc       USB-MSC UF2 drag-drop
#   - usbdfu    USB-DFU 1.1 class (dfu-util)
#   - rom       ESP ROM serial bootloader (esptool; optional [uart] extra)
#
# Wire-format source of truth for the session protocol is the device-side
# header dfu_session_wire.h shipped with the matching ESP-IDF component.
