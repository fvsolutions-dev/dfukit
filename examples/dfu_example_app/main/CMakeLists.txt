set(srcs
    "main.c"
    "usb_composite.c"   # TinyUSB CDC/MSC/HID/DFU composite
)

# ble_app.c only when the BLE transport is on — it pulls in NimBLE (bt).
if(CONFIG_DFU_TRANSPORT_BLE STREQUAL "y")
    list(APPEND srcs "ble_app.c")
endif()

# wifi_sta.c is only compiled when the UDP transport is on — it pulls in
# esp_wifi / esp_netif which aren't needed when UDP is disabled.
if(CONFIG_DFU_TRANSPORT_UDP STREQUAL "y")
    list(APPEND srcs "wifi_sta.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        app_update
        esp_timer
        log
        esp_hw_support
        dfukit
        bt
        esp_wifi
        esp_netif
        esp_event
        espressif__tinyusb
)
