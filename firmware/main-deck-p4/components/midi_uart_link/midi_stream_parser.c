/* SPDX-License-Identifier: Apache-2.0 */
#include "midi_stream_parser.h"

#include <string.h>

#define MIDI_STATUS_SYSEX_START 0xF0u
#define MIDI_STATUS_SYSEX_END   0xF7u
#define MIDI_STATUS_RT_FIRST    0xF8u

static uint8_t channel_voice_data_len(uint8_t status)
{
    switch (status & 0xF0u) {
    case 0xC0u: /* Program Change */
    case 0xD0u: /* Channel Pressure */
        return 1u;
    default:
        return 2u;
    }
}

/* System-common messages 0xF1..0xF6: bytes of payload to discard. */
static uint8_t system_common_skip(uint8_t status)
{
    switch (status) {
    case 0xF1u: /* MTC quarter frame */
    case 0xF3u: /* Song Select */
        return 1u;
    case 0xF2u: /* Song Position Pointer */
        return 2u;
    default:    /* 0xF4..0xF6, undefined / tune request */
        return 0u;
    }
}

void midi_stream_parser_reset(midi_stream_parser_t *parser)
{
    if (parser) {
        memset(parser, 0, sizeof(*parser));
    }
}

static bool emit(const midi_stream_parser_t *parser, usb_midi_message_t *out)
{
    const uint8_t status = parser->running_status;
    const uint8_t len = (uint8_t)(parser->expected_data + 1u);
    *out = (usb_midi_message_t) {
        .cable = 0u,
        .cin = (uint8_t)(status >> 4u),
        .len = len,
        .status = status,
        .data1 = parser->data[0],
        .data2 = parser->expected_data > 1u ? parser->data[1] : 0u,
    };
    return true;
}

bool midi_stream_parser_push(midi_stream_parser_t *parser, uint8_t byte,
                             usb_midi_message_t *out)
{
    if (!parser || !out) {
        return false;
    }

    /* System real-time is interleaved anywhere and never disturbs state. */
    if (byte >= MIDI_STATUS_RT_FIRST) {
        return false;
    }

    if (parser->phase == MIDI_STREAM_SYSEX) {
        /* Any status byte < 0xF8 ends the dump (0xF7 explicitly, or an
         * interrupting new message). Non-status bytes are payload. */
        if (byte >= 0x80u) {
            parser->phase = MIDI_STREAM_IDLE;
            /* fall through to process this status byte */
        } else {
            return false;
        }
    }

    if (byte >= 0x80u) {
        /* Status byte. */
        if (byte == MIDI_STATUS_SYSEX_START) {
            parser->phase = MIDI_STREAM_SYSEX;
            parser->running_status = 0u;
            return false;
        }
        if (byte >= 0xF1u) {
            /* System common (0xF1..0xF7). Cancels running status. */
            parser->running_status = 0u;
            parser->sys_skip = system_common_skip(byte);
            parser->phase = parser->sys_skip > 0u ? MIDI_STREAM_SYS_SKIP
                                                  : MIDI_STREAM_IDLE;
            return false;
        }
        /* Channel-voice status: latch as running status. */
        parser->running_status = byte;
        parser->expected_data = channel_voice_data_len(byte);
        parser->data_count = 0u;
        parser->phase = MIDI_STREAM_DATA;
        return false;
    }

    /* Data byte. */
    if (parser->phase == MIDI_STREAM_SYS_SKIP) {
        if (--parser->sys_skip == 0u) {
            parser->phase = MIDI_STREAM_IDLE;
        }
        return false;
    }
    if (parser->running_status == 0u) {
        return false; /* orphan data byte */
    }

    parser->data[parser->data_count++] = byte;
    if (parser->data_count < parser->expected_data) {
        return false;
    }

    parser->data_count = 0u; /* keep running status for the next message */
    return emit(parser, out);
}
