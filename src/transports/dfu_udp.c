/* UDP transport adapter for dfu_session.

   Wire protocol: DFU Session Protocol v1.1, same as BLE/HID. UDP
   preserves message boundaries — each datagram is exactly one protocol
   frame. No length-prefix framing. Each received datagram is fed
   directly to dfu_session_feed_frame; the sender's address is stored
   as the current peer for outbound frames (send vtable).

   ASSUMES the IP stack is already up when dfu_udp_start() is called.
   The caller is responsible for bringing up Wi-Fi (or any other IP
   transport) before this call. */

#include "dfu_udp.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "dfu_session.h"

static const char *TAG = "dfu_udp";

/* Maximum datagram we accept. 1500 covers a standard Ethernet MTU; the
   session's DATA frames are capped at max_payload() = 1024 bytes so
   this buffer has comfortable headroom for protocol overhead. */
#define UDP_BUF_BYTES   1500

/* Safe UDP payload that stays under typical MTU after IP + UDP headers
   (20 + 8 = 28 bytes). dfu_session subtracts its own 5-byte DATA header
   before advertising this to the host. */
#define UDP_MAX_PAYLOAD 1024

static dfu_session_t      *session;
static SemaphoreHandle_t   peer_mtx;   /* guards peer_addr + peer_valid */
static struct sockaddr_in  peer_addr;
static bool                peer_valid;
static int                 udp_sock = -1;

/* --- transport vtable ------------------------------------------------ */

static int udp_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    xSemaphoreTake(peer_mtx, portMAX_DELAY);
    bool valid = peer_valid;
    struct sockaddr_in addr = peer_addr;
    xSemaphoreGive(peer_mtx);

    if (!valid || udp_sock < 0) return -1;
    ssize_t n = sendto(udp_sock, buf, len, 0,
                       (const struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) {
        ESP_LOGW(TAG, "sendto: errno %d", errno);
        return -2;
    }
    return 0;
}

static size_t udp_max_payload(void *ctx)
{
    (void)ctx;
    return UDP_MAX_PAYLOAD;
}

/* --- UDP server task ------------------------------------------------- */

static void udp_server_task(void *arg)
{
    (void)arg;

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "socket(): errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Enlarge the RX buffer to absorb a full session window burst. UDP
    // has no flow control: while this task blocks writing one 4 KB flush
    // to flash, the host's in-flight window keeps arriving. A buffer
    // smaller than the window overflows and silently drops datagrams,
    // and with enough loss the OFFSET_GAP recovery can't even fire (the
    // out-of-order datagram that would trigger it is dropped too). Size
    // it above CONFIG_DFU_SESSION_WINDOW_BYTES so a whole window fits.
    int rcvbuf = CONFIG_DFU_SESSION_WINDOW_BYTES * 2;
    if (setsockopt(udp_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        ESP_LOGW(TAG, "SO_RCVBUF %d: errno %d (continuing)", rcvbuf, errno);
    }

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_DFU_TRANSPORT_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(udp_sock, (const struct sockaddr *)&bind_addr,
             sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind(): errno %d", errno);
        close(udp_sock);
        udp_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP server bound on port %d", CONFIG_DFU_TRANSPORT_UDP_PORT);

    static uint8_t buf[UDP_BUF_BYTES];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            ESP_LOGW(TAG, "recvfrom: %d (errno %d)", (int)n, errno);
            continue;
        }

        /* Update the peer address under the mutex so udp_send always
           routes to whoever last sent us a frame. */
        xSemaphoreTake(peer_mtx, portMAX_DELAY);
        peer_addr  = from;
        peer_valid = true;
        xSemaphoreGive(peer_mtx);

        dfu_session_feed_frame(session, buf, (size_t)n);
    }
}

/* --- public API ------------------------------------------------------- */

esp_err_t dfu_udp_start(void)
{
    if (session) return ESP_OK;

    peer_mtx = xSemaphoreCreateMutex();
    if (!peer_mtx) return ESP_ERR_NO_MEM;

    /* Register the dfu_session. IP stack is assumed up by the caller. */
    const dfu_session_transport_t tx = {
        .send             = udp_send,
        .max_payload      = udp_max_payload,
        .on_session_start = NULL,
        .on_release       = NULL,
        .ctx              = NULL,
        .source_id        = DFU_SRC_UDP,
        .name             = "udp",
    };
    const dfu_session_config_t scfg = {
        .window_bytes    = CONFIG_DFU_SESSION_WINDOW_BYTES,
        .idle_timeout_ms = CONFIG_DFU_SESSION_IDLE_TIMEOUT_MS,
        .hw_id           = CONFIG_DFU_TRANSPORT_HW_ID,
    };
    esp_err_t err = dfu_session_init(&session, &tx, &scfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dfu_session_init: %s", esp_err_to_name(err));
        return err;
    }

    /* Start the UDP receive task after the session is ready so the first
       arriving datagram already has a valid session pointer. Stack is
       generous because lwip's socket path can be deep. */
    BaseType_t ok = xTaskCreate(udp_server_task, "udp_dfu",
                                4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started (port=%d)", CONFIG_DFU_TRANSPORT_UDP_PORT);
    return ESP_OK;
}
