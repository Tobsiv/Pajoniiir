#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "midi_stream_parser.h"

static unsigned s_checks;
#define CHECK(expr) do { \
    s_checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

/* Feed a byte sequence; return count of completed messages, storing the last. */
static int feed(midi_stream_parser_t *p, const uint8_t *bytes, size_t n,
                usb_midi_message_t *last)
{
    int emitted = 0;
    for (size_t i = 0; i < n; ++i) {
        usb_midi_message_t msg;
        if (midi_stream_parser_push(p, bytes[i], &msg)) {
            emitted++;
            if (last) {
                *last = msg;
            }
        }
    }
    return emitted;
}

static void test_note_on(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    const uint8_t seq[] = { 0x90, 0x54, 0x7F };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.status == 0x90);
    CHECK(m.data1 == 0x54);
    CHECK(m.data2 == 0x7F);
    CHECK(m.len == 3);
    CHECK(m.cin == 0x9);
}

static void test_control_change_release(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    /* Button B PFL deck 2 press + release, FLX4-style note on ch2. */
    const uint8_t seq[] = { 0x91, 0x54, 0x7F, 0x91, 0x54, 0x00 };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 2);
    CHECK(m.status == 0x91 && m.data1 == 0x54 && m.data2 == 0x00);
}

static void test_running_status(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    /* One status byte, three note pairs. */
    const uint8_t seq[] = { 0x90, 0x40, 0x7F, 0x41, 0x7F, 0x40, 0x00 };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 3);
    CHECK(m.status == 0x90 && m.data1 == 0x40 && m.data2 == 0x00);
}

static void test_program_change_one_data(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    const uint8_t seq[] = { 0xC0, 0x05 };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.status == 0xC0 && m.data1 == 0x05 && m.data2 == 0x00);
    CHECK(m.len == 2);
}

static void test_realtime_interleaved(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    /* 0xF8 clock bytes sprinkled through a note must not disturb parsing. */
    const uint8_t seq[] = { 0x90, 0xF8, 0x3C, 0xF8, 0x64 };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.status == 0x90 && m.data1 == 0x3C && m.data2 == 0x64);
}

static void test_orphan_data_dropped(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    const uint8_t seq[] = { 0x11, 0x22, 0x33 };
    CHECK(feed(&p, seq, sizeof(seq), NULL) == 0);
}

static void test_sysex_skipped(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    /* SysEx dump, then a real note. Nothing from the dump is emitted, and the
     * note after 0xF7 parses cleanly. */
    const uint8_t seq[] = {
        0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7,
        0x90, 0x54, 0x7F
    };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.status == 0x90 && m.data1 == 0x54 && m.data2 == 0x7F);
}

static void test_sysex_cancels_running_status(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    const uint8_t seq[] = { 0x90, 0x40, 0x7F, 0xF0, 0xF7, 0x41, 0x7F };
    usb_midi_message_t m;
    /* First note emits; the trailing 0x41 0x7F are orphaned. */
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.data1 == 0x40);
}

static void test_song_position_skipped(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    /* 0xF2 + 2 data bytes, then a note. */
    const uint8_t seq[] = { 0xF2, 0x10, 0x20, 0x90, 0x54, 0x7F };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.status == 0x90 && m.data1 == 0x54);
}

static void test_status_interrupts_partial_message(void)
{
    midi_stream_parser_t p;
    midi_stream_parser_reset(&p);
    /* Note-on with only one data byte, then a fresh status byte. The partial
     * message is abandoned; the new one parses. */
    const uint8_t seq[] = { 0x90, 0x40, 0x91, 0x54, 0x7F };
    usb_midi_message_t m;
    CHECK(feed(&p, seq, sizeof(seq), &m) == 1);
    CHECK(m.status == 0x91 && m.data1 == 0x54 && m.data2 == 0x7F);
}

int main(void)
{
    test_note_on();
    test_control_change_release();
    test_running_status();
    test_program_change_one_data();
    test_realtime_interleaved();
    test_orphan_data_dropped();
    test_sysex_skipped();
    test_sysex_cancels_running_status();
    test_song_position_skipped();
    test_status_interrupts_partial_message();
    printf("ok - midi_stream_parser (%u checks)\n", s_checks);
    return 0;
}
