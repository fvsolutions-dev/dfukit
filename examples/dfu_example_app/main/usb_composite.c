/* usb_composite — TinyUSB composite device for the dfu_core example.
 *
 * CDC (log), MSC (UF2 drag-drop OTA), HID (OTA), DFU (dfu-util OTA). The
 * MSC/HID/DFU class callbacks forward into the dfu_core transport adapters
 * (dfu_msc_uf2_*, dfu_hid_*) and into dfu_core directly (USB-DFU). Kept in
 * its own translation unit so main.c stays focused on the boot sequence.
 */

#include "usb_composite.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "class/dfu/dfu.h"
#include "class/dfu/dfu_device.h"

#include "dfu_core.h"
#if CONFIG_DFU_TRANSPORT_HID
#include "dfu_hid.h"
#endif
#if CONFIG_DFU_TRANSPORT_MSC
#include "dfu_msc_uf2.h"
#endif

static const char *TAG = "usb_composite";

// ---------------------------------------------------------------------------
// usb_start — entry point (called from app_main)

static usb_phy_handle_t phy_hdl;

static void usb_task(void *arg)
{
    (void)arg;
    while (1) tud_task();
}

esp_err_t usb_start(void)
{
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_cfg, &phy_hdl), TAG, "usb_new_phy");
    if (!tusb_init()) {
        usb_del_phy(phy_hdl);
        return ESP_FAIL;
    }
    if (xTaskCreate(usb_task, "tusb", 4096, NULL, 5, NULL) != pdPASS) {
        usb_del_phy(phy_hdl);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TinyUSB up");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Composite descriptor — single source of truth for the interface layout

#define USB_VID 0x303A
#define USB_PID 0x4003
#define USB_BCD 0x0101

enum {
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MSC,
    ITF_NUM_HID,
    ITF_NUM_DFU,
    ITF_NUM_TOTAL
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MSC_OUT   0x03
#define EPNUM_MSC_IN    0x83
#define EPNUM_HID_IN    0x84

#define DFU_ALT_COUNT 1
#define DFU_ATTRS \
    (DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT | DFU_ATTR_WILL_DETACH)

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + \
     TUD_HID_DESC_LEN + TUD_DFU_DESC_LEN(DFU_ALT_COUNT))

static tusb_desc_device_t const device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const hid_report_desc[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(64)
};

static uint8_t const config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC,  4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC,  5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID,  6, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_desc), EPNUM_HID_IN, 64, 10),
    TUD_DFU_DESCRIPTOR(ITF_NUM_DFU,  DFU_ALT_COUNT, 7, DFU_ATTRS,
                       1000, CFG_TUD_DFU_XFER_BUFSIZE),
};

static char const *const string_descs[] = {
    (const char[]){0x09, 0x04},
    "dfu_core example",
    "ESP32-S3 OTA target",
    "000001",
    "CDC log",
    "OTA",
    "HID OTA",
    "DFU",
};
static int const string_desc_count =
    (int)(sizeof(string_descs) / sizeof(string_descs[0]));

// TinyUSB descriptor callbacks.
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return config_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[64];
    if (index >= (uint8_t)string_desc_count) return NULL;
    if (index == 0) {
        buf[1] = 0x0409;
        buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return buf;
    }
    const char *s = string_descs[index];
    size_t len = 0;
    while (s[len] && len < sizeof(buf) / sizeof(buf[0]) - 1) {
        buf[1 + len] = (uint8_t)s[len];
        len++;
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return buf;
}

// ---------------------------------------------------------------------------
// MSC callbacks — minimal virtual disk; drag-drop UF2 arrives via
// tud_msc_write10_cb which forwards to dfu_msc_uf2_write().

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB mounted");
#if CONFIG_DFU_TRANSPORT_MSC
    dfu_msc_uf2_reset();
#endif
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB unmounted");
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "ESP-IDF ", 8);
    memcpy(product_id,  "OTA             ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

// 512-byte virtual disk — one sector, so the FAT12 superblock fits.
#define DISK_BLOCK_COUNT 16u
#define DISK_BLOCK_SIZE  512u

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size)
{
    (void)lun;
    *block_count = DISK_BLOCK_COUNT;
    *block_size  = DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)load_eject;
    return start;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset;
    memset(buffer, 0, bufsize);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset;
    if (bufsize == 0 || (bufsize % 512) != 0) return (int32_t)bufsize;
#if CONFIG_DFU_TRANSPORT_MSC
    const uint32_t sectors = bufsize / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        dfu_msc_uf2_write(buffer + i * 512, 512);
    }
#endif
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    return -1;
}

// ---------------------------------------------------------------------------
// HID callbacks — forward OUT reports to dfu_hid; echo IN reports back.

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf)
{
    (void)itf;
    return hid_report_desc;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf; (void)report_id; (void)report_type;
#if CONFIG_DFU_TRANSPORT_HID
    dfu_hid_on_out(buffer, bufsize);
#else
    // Echo back for non-DFU builds.
    if (tud_hid_ready()) tud_hid_report(0, buffer, bufsize);
#endif
}

void tud_hid_report_complete_cb(uint8_t itf, uint8_t const *report,
                                uint16_t len)
{
    (void)itf; (void)report; (void)len;
#if CONFIG_DFU_TRANSPORT_HID
    dfu_hid_on_in_complete();
#endif
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)itf; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// ---------------------------------------------------------------------------
// CDC callbacks — minimal: ignore input, output is via ESP_LOG.

void tud_cdc_rx_cb(uint8_t itf)
{
    // Drain the RX buffer so the host doesn't stall.
    uint8_t buf[64];
    while (tud_cdc_n_read(itf, buf, sizeof(buf)) > 0) {
        // discard
    }
}

// ---------------------------------------------------------------------------
// USB-DFU callbacks — thin adapter over dfu_core (same pattern as
// main/usb/usb_dfu.c in the product firmware).

static bool dfu_acquired;

static void dfu_event_cb(dfu_source_t src, dfu_event_t evt,
                         esp_err_t err, void *user)
{
    (void)src; (void)user;
    uint8_t status;
    switch (evt) {
    case DFU_EVT_FEED_OK:
        status = DFU_STATUS_OK;
        break;
    case DFU_EVT_COMPLETE_OK:
        dfu_acquired = false;
        status = DFU_STATUS_OK;
        break;
    case DFU_EVT_ERROR:
    default:
        ESP_LOGE(TAG, "DFU OTA error: %s", esp_err_to_name(err));
        dfu_acquired = false;
        status = DFU_STATUS_ERR_WRITE;
        break;
    }
    tud_dfu_finish_flashing(status);
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num,
                         uint8_t const *data, uint16_t length)
{
    (void)alt; (void)block_num;
    if (!dfu_acquired) {
        if (dfu_core_acquire(DFU_SRC_USB_DFU, dfu_event_cb, NULL) != ESP_OK) {
            tud_dfu_finish_flashing(DFU_STATUS_ERR_WRITE);
            return;
        }
        dfu_acquired = true;
    }
    if (dfu_core_feed(DFU_SRC_USB_DFU, data, length) != ESP_OK) {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_WRITE);
        dfu_core_release(DFU_SRC_USB_DFU);
        dfu_acquired = false;
    }
}

void tud_dfu_manifest_cb(uint8_t alt)
{
    (void)alt;
    if (!dfu_acquired) {
        tud_dfu_finish_flashing(DFU_STATUS_OK);
        return;
    }
    if (dfu_core_complete(DFU_SRC_USB_DFU,
                          CONFIG_DFU_CORE_REBOOT_DELAY_MS) != ESP_OK) {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_VERIFY);
        dfu_core_release(DFU_SRC_USB_DFU);
        dfu_acquired = false;
    }
}

void tud_dfu_abort_cb(uint8_t alt)
{
    (void)alt;
    if (dfu_acquired) {
        dfu_core_release(DFU_SRC_USB_DFU);
        dfu_acquired = false;
    }
}

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
    (void)alt; (void)state;
    return 100;
}

uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num,
                           uint8_t *data, uint16_t length)
{
    (void)alt; (void)block_num; (void)data; (void)length;
    return 0;
}

void tud_dfu_detach_cb(void) {}
