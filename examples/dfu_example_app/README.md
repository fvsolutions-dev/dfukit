# dfu_example_app

Standalone regression target for the `dfu_core` component. Exercises all four
OTA transports on the smallest firmware that compiles and runs:

| Transport | Wire | Tool |
|-----------|------|------|
| USB-DFU   | EP0  | `dfu-util -d 303a:4003 -D dfu_example_app.bin -R` |
| USB-MSC UF2 | USB MSC write | Drag-drop `dfu_example_app.uf2` onto the "OTA" drive |
| USB-HID   | 64-byte reports | `dfukit` / `pydfu` HID transport |
| BLE       | GATT notify/write | `dfukit` / `pydfu` BLE transport |
| UDP       | UDP datagrams | `dfukit` / `pydfu` UDP transport |

A persistent boot counter in NVS increments on every boot. Watching it go
N → N+1 across a transport confirms the new image actually ran.

## Build and flash

```
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Set your Wi-Fi credentials before building (for UDP transport):

```
idf.py menuconfig   # OTA target example → Wi-Fi SSID / password
```

Or set them in `sdkconfig.defaults`:

```
CONFIG_DFU_EXAMPLE_WIFI_SSID="yourssid"
CONFIG_DFU_EXAMPLE_WIFI_PASS="yourpassword"
```

## Component dependency

`dfukit` is resolved from `../..` (the component repo root, two levels above
this example) via `EXTRA_COMPONENT_DIRS` in the top-level `CMakeLists.txt`. `espressif/tinyusb`
is fetched by the IDF component manager on first configure (declared in
`main/idf_component.yml`).

## Transport notes

**USB-DFU** — `dfu-util` sends the raw `.bin`. The DFU class here is
app-side (not ROM DFU); the image lands in the inactive OTA slot.

**USB-MSC UF2** — drag-drop a `.uf2` converted from the `.bin`:
`uf2conv.py dfu_example_app.bin --family esp32s3 -o dfu_example_app.uf2`.
Non-UF2 sectors (FAT metadata) are silently ignored.

**BLE** — advertises as "dfu_example" on legacy 1M PHY. The OTA GATT
service UUID is `0x0000EEE1-0000-1000-8000-00805f9b34fb` (configurable via
`CONFIG_DFU_TRANSPORT_BLE_SVC_UUID16`).

**UDP** — device binds port `CONFIG_DFU_TRANSPORT_UDP_PORT` (default 3333)
after Wi-Fi STA connects. BT+Wi-Fi coexistence is enabled.
