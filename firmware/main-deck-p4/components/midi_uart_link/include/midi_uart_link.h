/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Serial-MIDI transport adapter for an external controller MCU.
 *
 * Receives a raw MIDI 1.0 byte stream on a UART (default 19200 8N1, GPIO28 RX /
 * GPIO29 TX) and feeds assembled messages into controller_runtime_handle_midi(),
 * the same entry point the direct USB-MIDI path uses. The existing controller
 * runtime, profile mapping and CTRL-event pipeline are unchanged; this component
 * is only a transport.
 *
 * The controller is assumed to emit / accept DDJ-FLX4 compatible MIDI so the
 * built-in FLX4 map and LED encoder apply without an SD profile.
 *
 * TX: this component chains onto the control_link LED sink and mirrors every
 * LED command to the UART as a 3-byte FLX4-shaped note (header byte stripped),
 * forwarding to whatever sink was installed before it (the USB path).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bytes_received;
    uint32_t messages_parsed;
    uint32_t messages_mapped;   /* handle_midi() returned true */
    uint32_t semantic_events;   /* events delivered to the deck_core queue */
    uint32_t queue_failures;    /* deck_core queue full / not ready */
    uint32_t led_messages_sent; /* LED commands mirrored out the UART */
    uint32_t uart_errors;
    bool     runtime_bound;     /* controller runtime up, built-in FLX4 map on */
} midi_uart_link_diagnostics_t;

/* Configure the UART and start the RX task. Idempotent; safe to call once from
 * app_main after the controller runtime owner has been started. */
esp_err_t midi_uart_link_start(void);

void midi_uart_link_get_diagnostics(midi_uart_link_diagnostics_t *out);

#ifdef __cplusplus
}
#endif
