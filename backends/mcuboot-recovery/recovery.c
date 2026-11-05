// dfukit's recovery in MCUboot (Kconfig DFUKIT_MCUBOOT_RECOVERY): MCUboot's action hook enters it
// at startup (the button) or when there is no bootable image; it never returns. A dfukit server
// on the UART chosen as dfukit,uart, its frames burst-link encoded, received by DMA:
// the flash's erases and writes stop the CPU's interrupts, never the reception

#include <burst/burst_decoder.h>
#include <burst/burst_encoder.h>
#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/ring_buffer.h>

#include "bootutil/mcuboot_status.h"
#include "mcuboot_config/mcuboot_config.h"
#include "dfukit_inflate.h"
#include "dfukit_server.h"
#include "dfukit_mcuboot.h"

#define RX_BUFFER CONFIG_DFUKIT_MCUBOOT_RX_BUFFER
// An erase holds the interrupts, so the DMA cannot change buffers meanwhile: one takes all a host
// may have in flight. A window as it is on the line: each frame's header, CRC, COBS and zeros (a
// 64th covers them), and the frame the buffer may already be in the middle of
BUILD_ASSERT(CONFIG_DFUKIT_MCUBOOT_RX_BUFFER >= CONFIG_DFUKIT_MCUBOOT_WINDOW +
							CONFIG_DFUKIT_MCUBOOT_WINDOW / 64 +
							BURST_MAX_ENCODED(DFUKIT_MAX_FRAME),
	     "DFUKIT_MCUBOOT_RX_BUFFER must take a window on the line (DFUKIT_MCUBOOT_WINDOW)");
// A receive buffer handed over after this long without a byte
#define RX_IDLE_US 500
// MCUboot's slot 0; PARTITION_ID() from Zephyr 4.4 on
#ifdef PARTITION_ID
#define SLOT0 PARTITION_ID(slot0_partition)
#else
#define SLOT0 FIXED_PARTITION_ID(slot0_partition)
#endif

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(dfukit_uart));

// Written by the DMA, past the CPU's data cache: whole cache lines, invalidated before each read
static uint8_t rx_buffers[2][RX_BUFFER] __aligned(64);
static uint8_t next_rx;
RING_BUF_DECLARE(rx_ring, 2 * RX_BUFFER);
static uint8_t tx_buffer[BURST_MAX_ENCODED(DFUKIT_MAX_FRAME)];

static dfukit_server_t server;
static dfukit_backend_t backend;
static dfukit_mcuboot_t mcuboot;
static burst_managed_decoder_t decoder;
static uint8_t decoded[DFUKIT_MAX_FRAME + CRC_SIZE];
#if CONFIG_DFUKIT_MCUBOOT_WINDOW_LOG2 > 0
static dfukit_inflate_t inflate;
static dfukit_inflater_t inflater;
static uint8_t dict[1u << CONFIG_DFUKIT_MCUBOOT_WINDOW_LOG2];
#endif

// The one host on the UART
static const dfukit_peer_t host = {.len = 1, .bytes = {0}};

#if DT_NODE_EXISTS(DT_ALIAS(mcuboot_led0))
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_led0), gpios);
#endif

static FUNC_NORETURN void recover(void);
static void on_uart(const struct device *device, struct uart_event *event, void *user_data);
static int send_frame(void *ctx, const dfukit_peer_t *to, const uint8_t *frame, size_t len);
static int on_frame(const uint8_t *frame, size_t len, void *ctx);
static void on_status(void *ctx, const dfukit_server_status_t *status);
static void draw_led(void);

/** @brief MCUboot's action hook: enters the recovery, at the button or without an image. */
void mcuboot_status_change(mcuboot_status_type_t status)
{
#if defined(CONFIG_DFUKIT_MCUBOOT_ON_BUTTON) && DT_NODE_EXISTS(DT_ALIAS(mcuboot_button0))
	if (status == MCUBOOT_STATUS_STARTUP) {
		static const struct gpio_dt_spec button =
			GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_button0), gpios);

		// Active at reset, still after a moment: not a glitch
		if (gpio_pin_configure_dt(&button, GPIO_INPUT) == 0 &&
		    gpio_pin_get_dt(&button) == 1) {
			k_msleep(5);
			if (gpio_pin_get_dt(&button) == 1) {
				recover();
			}
		}
	}
#endif
#ifdef CONFIG_DFUKIT_MCUBOOT_ON_NO_IMAGE
	if (status == MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND) {
		recover();
	}
#endif
}

/** @brief The recovery: sets up the server and the UART, then takes frames for ever. */
static FUNC_NORETURN void recover(void)
{
	const dfukit_server_config_t config = {
		.send = send_frame,
		.name = CONFIG_DFUKIT_MCUBOOT_NAME,
		.max_frame = DFUKIT_MAX_FRAME,
		.on_status = on_status,
#if CONFIG_DFUKIT_MCUBOOT_WINDOW_LOG2 > 0
		.inflater = &inflater,
#endif
	};

#if CONFIG_DFUKIT_MCUBOOT_WINDOW_LOG2 > 0
	dfukit_inflate_init(&inflate, &inflater, dict, sizeof(dict));
#endif
	dfukit_server_init(&server, &config);
	dfukit_mcuboot_init(&mcuboot, &backend, CONFIG_DFUKIT_MCUBOOT_BOARD_ID, SLOT0,
			    CONFIG_DFUKIT_MCUBOOT_WINDOW, CONFIG_DFUKIT_MCUBOOT_WINDOW_LOG2);
	dfukit_server_register(&server, &backend);
	burst_managed_decoder_init(&decoder, decoded, sizeof(decoded), on_frame, NULL);
#if DT_NODE_EXISTS(DT_ALIAS(mcuboot_led0))
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
#endif
	uart_callback_set(uart, on_uart, NULL);
	next_rx = 1;
	uart_rx_enable(uart, rx_buffers[0], RX_BUFFER, RX_IDLE_US);

	for (;;) {
		static uint8_t data[512];
		// The interrupt fills the ring: taken out with it locked, briefly
		unsigned int key = irq_lock();
		uint32_t received = ring_buf_get(&rx_ring, data, sizeof(data));

		irq_unlock(key);
		if (received > 0) {
			burst_managed_decoder_handle_data(&decoder, data, received);
		} else {
			// Nothing came: the erase goes on, a sector at a time
			dfukit_mcuboot_step(&mcuboot);
			k_msleep(1);
		}
		dfukit_server_poll(&server, k_uptime_get_32());
		draw_led();
		// As MCUboot's own loops (a no-op without its watchdog)
		MCUBOOT_WATCHDOG_FEED();
	}
}

/** @brief The UART's events: what the DMA received to the ring, and its buffers. */
static void on_uart(const struct device *device, struct uart_event *event, void *user_data)
{
	switch (event->type) {
	// Bytes in: what does not fit is lost, as a frame lost on the line, and the protocol
	// resends
	case UART_RX_RDY:
		sys_cache_data_invd_range(event->data.rx.buf + event->data.rx.offset,
					  event->data.rx.len);
		ring_buf_put(&rx_ring, event->data.rx.buf + event->data.rx.offset,
			     event->data.rx.len);
		break;
	// The DMA asks for its next buffer
	case UART_RX_BUF_REQUEST:
		uart_rx_buf_rsp(device, rx_buffers[next_rx], RX_BUFFER);
		next_rx ^= 1;
		break;
	// After a line error: on again
	case UART_RX_DISABLED:
		next_rx = 1;
		uart_rx_enable(device, rx_buffers[0], RX_BUFFER, RX_IDLE_US);
		break;
	default:
		break;
	}
}

/** @brief The device's frames are short (an ACK is 12 bytes): sent byte by byte, done on return. */
static int send_frame(void *ctx, const dfukit_peer_t *to, const uint8_t *frame, size_t len)
{
	burst_encoder_t encoder;

	burst_encoder_init(&encoder, tx_buffer, sizeof(tx_buffer));
	if (burst_encoder_add_packet(&encoder, frame, len) != BURST_PACKET_READY) {
		return -1;
	}
	burst_packet_t packet = burst_encoder_flush(&encoder);

	for (size_t index = 0; index < packet.size; index++) {
		uart_poll_out(uart, packet.data[index]);
	}
	return 0;
}

/** @brief A whole frame from the decoder, to the server. */
static int on_frame(const uint8_t *frame, size_t len, void *ctx)
{
	dfukit_server_feed(&server, &host, frame, len, k_uptime_get_32());
	return 0;
}

// The mcuboot-led0 alias, where there is one: a slow blink while waiting in recovery, a fast one
// during an update, on once the image is written
#if DT_NODE_EXISTS(DT_ALIAS(mcuboot_led0))
static dfukit_session_state_t led_state;

/** @brief Notes the session's state for the LED. */
static void on_status(void *ctx, const dfukit_server_status_t *status)
{
	led_state = status->state;
}

/** @brief Sets the LED for the session's state. */
static void draw_led(void)
{
	uint32_t now = k_uptime_get_32();
	bool on = led_state == DFUKIT_SESSION_READY ||
		  (led_state == DFUKIT_SESSION_IDLE ? now % 1000 < 500 : now % 250 < 125);

	gpio_pin_set_dt(&led, on);
}
#else
/** @brief No LED. */
static void on_status(void *ctx, const dfukit_server_status_t *status)
{
}

/** @brief No LED. */
static void draw_led(void)
{
}
#endif
