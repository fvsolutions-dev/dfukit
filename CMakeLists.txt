set(srcs
    "src/dfu_core.c"
    "src/dfu_core_uf2.c"
    "src/dfu_session.c"
)

if(CONFIG_DFU_TRANSPORT_BLE STREQUAL "y")
    list(APPEND srcs "src/transports/dfu_ble.c")
endif()

if(CONFIG_DFU_TRANSPORT_HID STREQUAL "y")
    list(APPEND srcs "src/transports/dfu_hid.c")
endif()

if(CONFIG_DFU_TRANSPORT_UDP STREQUAL "y")
    list(APPEND srcs "src/transports/dfu_udp.c")
endif()

if(CONFIG_DFU_TRANSPORT_MSC STREQUAL "y")
    list(APPEND srcs "src/transports/dfu_msc_uf2.c")
endif()

# Core requirements always needed.
# Transport dependencies (bt, espressif__tinyusb, lwip, esp_netif) are
# listed unconditionally in REQUIRES so the IDF component scanner resolves
# their include paths when the matching transport source is compiled.
# They are harmless when the corresponding CONFIG symbol is off (no source
# file pulls their headers, so the linker never references them).
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "src"
    REQUIRES
        app_update esp_partition esp_timer esp_system log mbedtls
        bt espressif__tinyusb lwip esp_netif
)
