/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Byte-stream MIDI 1.0 parser.
 *
 * Turns a raw serial MIDI byte stream (as it arrives over UART from an external
 * controller MCU) into assembled channel-voice messages shaped like the USB-MIDI
 * events the controller runtime already consumes. The parser is transport
 * neutral and has no ESP-IDF dependency so it is covered by a host test.
 *
 * Handled:
 *   - Channel-voice status 0x80..0xEF, including running status.
 *   - System real-time bytes 0xF8..0xFF are transparent: consumed without
 *     disturbing running status or a message in progress.
 *   - System exclusive 0xF0..0xF7 and system-common 0xF1..0xF7 cancel running
 *     status; their payloads are skipped, not emitted.
 *
 * A data byte received with no running status is dropped.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usb_midi_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIDI_STREAM_IDLE = 0,   /* waiting for a status byte */
    MIDI_STREAM_DATA,       /* collecting data bytes for running_status */
    MIDI_STREAM_SYSEX,      /* inside 0xF0 .. 0xF7, discarding */
    MIDI_STREAM_SYS_SKIP,   /* system-common: discard a fixed number of bytes */
} midi_stream_phase_t;

typedef struct {
    midi_stream_phase_t phase;
    uint8_t running_status;  /* 0 = none */
    uint8_t expected_data;   /* data bytes required by running_status */
    uint8_t data_count;      /* data bytes collected so far */
    uint8_t data[2];
    uint8_t sys_skip;        /* remaining bytes to discard in MIDI_STREAM_SYS_SKIP */
} midi_stream_parser_t;

void midi_stream_parser_reset(midi_stream_parser_t *parser);

/*
 * Feed one received byte. Returns true and fills *out when the byte completed a
 * channel-voice message. cable/cin/len in *out are synthesized for compatibility
 * with usb_midi_message_t consumers; status/data1/data2 carry the message.
 */
bool midi_stream_parser_push(midi_stream_parser_t *parser, uint8_t byte,
                             usb_midi_message_t *out);

#ifdef __cplusplus
}
#endif
