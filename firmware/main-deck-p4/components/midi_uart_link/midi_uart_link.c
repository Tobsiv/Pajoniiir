/* SPDX-License-Identifier: Apache-2.0 */
#include "midi_uart_link.h"

#include <stdatomic.h>

#include "sdkconfig.h"

#if CONFIG_MIDI_UART_LINK_ENABLED

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "control_link.h"
#include "controller_runtime.h"
#include "flx4_led_midi.h"
#include "flx4_map.h"
#include "midi_stream_parser.h"

#define LINK_PORT       ((uart_port_t)CONFIG_MIDI_UART_LINK_PORT)
#define LINK_RX_GPIO    CONFIG_MIDI_UART_LINK_RX_GPIO
#define LINK_TX_GPIO    CONFIG_MIDI_UART_LINK_TX_GPIO
#define LINK_BAUD       CONFIG_MIDI_UART_LINK_BAUD

#define LINK_RX_RING_BYTES   512
#define LINK_TX_RING_BYTES   256
#define LINK_READ_CHUNK      64
#define LINK_READ_TIMEOUT_MS 100
#define LINK_DISPATCH_BUDGET 64u

static const char *TAG = "midi_uart";

static TaskHandle_t s_rx_task;
static TaskHandle_t s_dispatch_task;
static atomic_bool s_started;
static atomic_bool s_runtime_ready;

static uint32_t s_bytes_received;
static uint32_t s_messages_parsed;
static uint32_t s_messages_mapped;
static uint32_t s_semantic_events;
static uint32_t s_queue_failures;
static uint32_t s_led_messages_sent;
static uint32_t s_uart_errors;

static inline void bump(uint32_t *counter)
{
    (void)__atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
}

/* Controller runtime -> transport-neutral semantic queue (deck_core's queue,
 * bound by control_link_init in app_main). */
static esp_err_t semantic_callback(const flx4_control_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t rc = control_link_inject_semantic(
        event->type, event->id, event->value);
    if (rc == ESP_OK) {
        bump(&s_semantic_events);
    } else {
        bump(&s_queue_failures);
    }
    return rc;
}

/*
 * control_link LED sink. Every LED command from deck_core / ui becomes a 3-byte
 * FLX4-shaped note out the UART (the USB-MIDI header byte from
 * flx4_led_midi_build_packet is dropped). This is the only LED sink now — the
 * USB controller LED path is gone.
 */
static esp_err_t uart_led_sink(uint8_t led, uint8_t state, uint8_t deck,
                               void *ctx)
{
    (void)ctx;
    uint8_t packet[4];
    if (flx4_led_midi_build_packet(led, state, deck, packet)) {
        const uint8_t midi[3] = { packet[1], packet[2], packet[3] };
        const int written = uart_write_bytes(LINK_PORT, midi, sizeof(midi));
        if (written == (int)sizeof(midi)) {
            bump(&s_led_messages_sent);
        } else {
            bump(&s_uart_errors);
        }
    }
    return ESP_OK;
}

static void dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        const size_t dispatched =
            controller_runtime_dispatch_pending(LINK_DISPATCH_BUDGET);
        if (dispatched == 0u) {
            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            taskYIELD();
        }
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    midi_stream_parser_t parser;
    midi_stream_parser_reset(&parser);
    uint8_t buf[LINK_READ_CHUNK];

    for (;;) {
        const int n = uart_read_bytes(LINK_PORT, buf, sizeof(buf),
                                      pdMS_TO_TICKS(LINK_READ_TIMEOUT_MS));
        if (n < 0) {
            bump(&s_uart_errors);
            continue;
        }
        if (n == 0) {
            continue;
        }
        (void)__atomic_add_fetch(&s_bytes_received, (uint32_t)n, __ATOMIC_RELAXED);

        for (int i = 0; i < n; ++i) {
            usb_midi_message_t msg;
            if (!midi_stream_parser_push(&parser, buf[i], &msg)) {
                continue;
            }
            bump(&s_messages_parsed);
            if (controller_runtime_handle_midi(&msg)) {
                bump(&s_messages_mapped);
            }
        }
    }
}

esp_err_t midi_uart_link_start(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_started, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* This component owns the controller runtime lifecycle now (there is no USB
     * controller owner). The external controller speaks DDJ-FLX4 compatible
     * MIDI, so the built-in FLX4 map applies without an SD profile. */
    const controller_runtime_config_t runtime_cfg = {
        .event_cb = semantic_callback,
        .callback_ctx = NULL,
        .publish_connection_events = true,
    };
    esp_err_t rc = controller_runtime_init(&runtime_cfg);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller_runtime_init: %s", esp_err_to_name(rc));
        atomic_store_explicit(&s_started, false, memory_order_release);
        return rc;
    }
    controller_runtime_set_builtin_flx4_enabled(true);
    controller_runtime_set_connected(true);
    atomic_store_explicit(&s_runtime_ready, true, memory_order_release);

    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    rc = uart_driver_install(LINK_PORT, LINK_RX_RING_BYTES,
                             LINK_TX_RING_BYTES, 0, NULL, 0);
    if (rc == ESP_OK) {
        rc = uart_param_config(LINK_PORT, &cfg);
    }
    if (rc == ESP_OK) {
        rc = uart_set_pin(LINK_PORT, LINK_TX_GPIO, LINK_RX_GPIO,
                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "UART%d init failed: %s", (int)LINK_PORT,
                 esp_err_to_name(rc));
        (void)uart_driver_delete(LINK_PORT);
        atomic_store_explicit(&s_started, false, memory_order_release);
        return rc;
    }

    if (xTaskCreate(dispatch_task, "midi_uart_disp", 4096, NULL, 4,
                    &s_dispatch_task) != pdPASS ||
        xTaskCreate(rx_task, "midi_uart_rx", 4096, NULL, 6, &s_rx_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        (void)uart_driver_delete(LINK_PORT);
        atomic_store_explicit(&s_started, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    control_link_set_led_sink(uart_led_sink, NULL);

    ESP_LOGI(TAG, "serial MIDI link up: UART%d RX=%d TX=%d @ %d 8N1",
             (int)LINK_PORT, LINK_RX_GPIO, LINK_TX_GPIO, LINK_BAUD);
    return ESP_OK;
}

void midi_uart_link_get_diagnostics(midi_uart_link_diagnostics_t *out)
{
    if (!out) {
        return;
    }
    *out = (midi_uart_link_diagnostics_t) {
        .bytes_received = __atomic_load_n(&s_bytes_received, __ATOMIC_ACQUIRE),
        .messages_parsed = __atomic_load_n(&s_messages_parsed, __ATOMIC_ACQUIRE),
        .messages_mapped = __atomic_load_n(&s_messages_mapped, __ATOMIC_ACQUIRE),
        .semantic_events = __atomic_load_n(&s_semantic_events, __ATOMIC_ACQUIRE),
        .queue_failures = __atomic_load_n(&s_queue_failures, __ATOMIC_ACQUIRE),
        .led_messages_sent =
            __atomic_load_n(&s_led_messages_sent, __ATOMIC_ACQUIRE),
        .uart_errors = __atomic_load_n(&s_uart_errors, __ATOMIC_ACQUIRE),
        .runtime_bound =
            atomic_load_explicit(&s_runtime_ready, memory_order_acquire),
    };
}

#else /* !CONFIG_MIDI_UART_LINK_ENABLED */

esp_err_t midi_uart_link_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void midi_uart_link_get_diagnostics(midi_uart_link_diagnostics_t *out)
{
    if (out) {
        *out = (midi_uart_link_diagnostics_t) {0};
    }
}

#endif /* CONFIG_MIDI_UART_LINK_ENABLED */
