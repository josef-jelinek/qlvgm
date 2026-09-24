#define QLVGM_TEST

#include "src/runtime.c"
#include "src/vgm.c"
#include "src/qlz.c"
#include "src/qlay.c"

static int test_failures;

static void check(bool condition, const char *name) {
    if (condition) {
        return;
    }
    printf("FAIL %s\n", name);
    test_failures += 1;
}

static void test_write_le32(uint8_t *data, uint32_t offset, uint32_t value) {
    data[offset] = (uint8_t)value;
    data[offset + 1] = (uint8_t)(value >> 8);
    data[offset + 2] = (uint8_t)(value >> 16);
    data[offset + 3] = (uint8_t)(value >> 24);
}

static uint16_t test_read_be16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] << 8 | data[1]);
}

static uint32_t test_read_be32(const uint8_t *data) {
    return (uint32_t)data[0] << 24
        | (uint32_t)data[1] << 16
        | (uint32_t)data[2] << 8
        | data[3];
}

static byte_buffer make_qlz_test_stream(const uint8_t *payload, uint32_t payload_size, uint32_t decoded_size) {
    byte_buffer stream = { 0 };
    buffer_append(&stream, "QLZ1", 4);
    qlz_append_be32(&stream, decoded_size);
    qlz_append_be32(&stream, payload_size);
    buffer_append(&stream, payload, payload_size);
    return stream;
}

static bool test_qlz_decode_stream(const byte_buffer *stream, byte_buffer *decoded, qlvgm_error *error) {
    memset(decoded, 0, sizeof(*decoded));
    memset(error, 0, sizeof(*error));
    return qlz_decode(stream->data, stream->size, decoded, error);
}

static void test_qlz_append_literals(byte_buffer *payload, byte_buffer *expected, uint32_t count) {
    while (count != 0) {
        uint32_t length = count;
        if (length > QLZ_LITERAL_MAX) {
            length = QLZ_LITERAL_MAX;
        }
        buffer_append_byte(payload, (uint8_t)(length - 1));
        for (uint32_t i = 0; i < length; i += 1) {
            uint8_t value = (uint8_t)(expected->size * 37u + 11u);
            buffer_append_byte(payload, value);
            buffer_append_byte(expected, value);
        }
        count -= length;
    }
}

static void test_qlz_append_expected_match(byte_buffer *expected, uint32_t distance, uint32_t length) {
    for (uint32_t i = 0; i < length; i += 1) {
        buffer_append_byte(expected, expected->data[expected->size - distance]);
    }
}

static file_data make_test_vgm(void) {
    file_data file;
    file.size = 0xA0;
    file.data = calloc(1, file.size);
    memcpy(file.data, "Vgm ", 4);
    test_write_le32(file.data, 0x08, 0x170);
    test_write_le32(file.data, 0x18, 2646);
    test_write_le32(file.data, 0x20, 1764);
    test_write_le32(file.data, 0x34, 0x80 - 0x34);
    test_write_le32(file.data, 0x44, VGM_FIXTURE_CLOCK);
    uint32_t position = 0x80;
    uint8_t intro[] = {
        0x55, 0x00, 0x34,
        0x55, 0x01, 0x02,
        0x55, 0xA4, 0x22,
        0x55, 0xA0, 0x34,
        0x55, 0x28, 0xF0,
        0x63
    };
    memcpy(file.data + position, intro, sizeof intro);
    position += sizeof intro;
    test_write_le32(file.data, 0x1C, position - 0x1C);
    uint8_t loop[] = {
        0x55, 0x00, 0x34,
        0x55, 0x08, 0x0F,
        0x63,
        0x55, 0x00, 0x56,
        0x55, 0x01, 0x03,
        0x63,
        0x66
    };
    memcpy(file.data + position, loop, sizeof loop);
    position += sizeof loop;
    file.size = position;
    test_write_le32(file.data, 0x04, file.size - 4);
    return file;
}

static file_data make_volume_metadata_test_vgm(uint16_t gain) {
    file_data file = make_test_vgm();
    uint32_t command_size = file.size - 0x80;
    uint32_t old_loop = read_le32(file.data + 0x1C) + 0x1C;
    file.data = realloc(file.data, file.size + 0x60);
    memmove(file.data + 0xE0, file.data + 0x80, command_size);
    memset(file.data + 0x80, 0, 0x60);
    file.size += 0x60;
    test_write_le32(file.data, 0x04, file.size - 4);
    test_write_le32(file.data, 0x1C, old_loop + 0x60 - 0x1C);
    test_write_le32(file.data, 0x34, 0xE0 - 0x34);
    test_write_le32(file.data, 0xBC, 4);
    test_write_le32(file.data, 0xC0, 12);
    test_write_le32(file.data, 0xC8, 4);
    file.data[0xCC] = 1;
    file.data[0xCD] = 0x86;
    file.data[0xCE] = 0;
    file.data[0xCF] = (uint8_t)gain;
    file.data[0xD0] = (uint8_t)(gain >> 8);
    return file;
}

static void test_vgm_conversion(void) {
    file_data file = make_test_vgm();
    qlvgm_error error = { { 0 } };
    vgm_song song;
    bool parsed = vgm_parse(file.data, file.size, &song, &error);
    check(parsed, "valid YM2203 VGM parses");
    check(parsed && song.write_count == 9 && song.has_loop, "VGM writes and loop are collected");

    converted_song converted;
    bool converted_ok = parsed && vgm_convert(&song, 50, true, &converted, &error);
    check(converted_ok, "VGM converts to 50 Hz stream");
    check(
        converted_ok && converted.frame_count == 5 && converted.loop_frame_count == 2,
        "PAL conversion retains the first and steady-state loop frames"
    );
    check(
        converted_ok && converted.loop_offset > 6 && converted.loop_offset < converted.stream.size,
        "loop offset skips a distinct first iteration"
    );
    if (converted_ok) {
        uint32_t count = (uint32_t)converted.stream.data[0] << 8 | converted.stream.data[1];
        uint8_t registers[256] = { 0 };
        for (uint32_t i = 0; i < count; i += 1) {
            uint8_t reg = converted.stream.data[2 + i * 2];
            registers[reg] = converted.stream.data[3 + i * 2];
        }
        check(
            registers[0] == 0x1A && registers[1] == 0x01,
            "PSG tone period is scaled for the 2 MHz QSound2 clock"
        );
        uint32_t source_fm = 0x234u << ((0x22 >> 3) & 7);
        uint32_t target_fm = (
            ((uint32_t)registers[0xA0] | (registers[0xA4] & 7u) << 8) <<
            ((registers[0xA4] >> 3) & 7)
        );
        check(
            target_fm > source_fm && target_fm >= source_fm * 19 / 10,
            "FM block/F-number is scaled for the 2 MHz QSound2 clock"
        );
        uint32_t first_loop = 2 + count * 2;
        uint32_t first_loop_count = (
            (uint32_t)converted.stream.data[first_loop] << 8 |
            converted.stream.data[first_loop + 1]
        );
        bool first_loop_uses_intro_latch = false;
        for (uint32_t i = 0; i < first_loop_count; i += 1) {
            uint8_t reg = converted.stream.data[first_loop + 2 + i * 2];
            uint8_t value = converted.stream.data[first_loop + 3 + i * 2];
            if (reg == 0 && value == 0x1A) {
                first_loop_uses_intro_latch = true;
            }
        }
        check(first_loop_uses_intro_latch, "first loop iteration uses the intro frequency latch");
        uint32_t loop = converted.loop_offset;
        uint32_t loop_count = (
            (uint32_t)converted.stream.data[loop] << 8 |
            converted.stream.data[loop + 1]
        );
        bool loop_restores_frequency = false;
        for (uint32_t i = 0; i < loop_count; i += 1) {
            uint8_t reg = converted.stream.data[loop + 2 + i * 2];
            uint8_t value = converted.stream.data[loop + 3 + i * 2];
            if (reg == 0 && value == 0x9B) {
                loop_restores_frequency = true;
            }
        }
        check(loop_restores_frequency, "steady-state loop uses its ending frequency latch");
        converted_song_free(&converted);
    }

    uint8_t expected_raw[] = {
        0x00, 0x05,
        0x00, 0x34,
        0x01, 0x02,
        0xA4, 0x22,
        0xA0, 0x34,
        0x28, 0xF0,
        0x00, 0x02,
        0x00, 0x34,
        0x08, 0x0F,
        0x00, 0x02,
        0x00, 0x56,
        0x01, 0x03
    };
    converted_ok = parsed && vgm_convert(&song, 50, false, &converted, &error);
    check(converted_ok, "VGM converts with pitch conversion disabled");
    check(
        (
            converted_ok &&
            converted.frame_count == 3 &&
            converted.loop_frame_count == 2 &&
            converted.loop_offset == 12
        ),
        "raw-pitch loop needs no converted latch transition"
    );
    check(
        (
            converted_ok &&
            converted.stream.size == sizeof expected_raw &&
            memcmp(converted.stream.data, expected_raw, sizeof expected_raw) == 0
        ),
        "raw-pitch stream preserves source register writes and order"
    );
    check(converted_ok && converted.clipped_values == 0, "raw-pitch conversion reports no clipping");
    if (converted_ok) {
        converted_song_free(&converted);
    }
    vgm_song_free(&song);

    test_write_le32(file.data, 0x1C, 0);
    test_write_le32(file.data, 0x20, 0);
    memset(&error, 0, sizeof(error));
    parsed = vgm_parse(file.data, file.size, &song, &error);
    converted_ok = parsed && vgm_convert(&song, 60, true, &converted, &error);
    check(
        converted_ok && !song.has_loop && converted.loop_offset == VGM_NO_LOOP,
        "non-looping VGM converts with the end sentinel"
    );
    if (converted_ok) {
        converted_song_free(&converted);
    }
    vgm_song_free(&song);

    file.data[0x80] = 0x54;
    memset(&error, 0, sizeof(error));
    parsed = vgm_parse(file.data, file.size, &song, &error);
    check(!parsed && strstr(error.text, "unsupported VGM command") != NULL, "unsupported command is rejected");
    vgm_song_free(&song);
    free(file.data);
}

static void test_fm_latch_conversion(void) {
    clock_converter baseline_converter;
    memset(&baseline_converter, 0, sizeof(baseline_converter));
    baseline_converter.source_clock = VGM_FIXTURE_CLOCK;
    baseline_converter.pitch_conversion = true;
    byte_buffer baseline = { 0 };
    bool baseline_ok = (
        converter_write(&baseline_converter, &baseline, 0xA4, 0x22) &&
        baseline.size == 0 &&
        converter_write(&baseline_converter, &baseline, 0xA0, 0x34)
    );
    check(
        (
            baseline_ok &&
            baseline.size == 4 &&
            baseline.data[0] == 0xA4 &&
            baseline.data[2] == 0xA0
        ),
        "FM conversion emits a committing high-then-low pair"
    );

    clock_converter interleaved_converter;
    memset(&interleaved_converter, 0, sizeof(interleaved_converter));
    interleaved_converter.source_clock = VGM_FIXTURE_CLOCK;
    interleaved_converter.pitch_conversion = true;
    byte_buffer interleaved = { 0 };
    bool interleaved_ok = (
        converter_write(&interleaved_converter, &interleaved, 0xA4, 0x22) &&
        converter_write(&interleaved_converter, &interleaved, 0xAC, 0x2A) &&
        interleaved.size == 0 &&
        converter_write(&interleaved_converter, &interleaved, 0xA0, 0x34) &&
        converter_write(&interleaved_converter, &interleaved, 0xA8, 0x42)
    );
    check(
        (
            interleaved_ok &&
            interleaved.size == 8 &&
            baseline.size == 4 &&
            memcmp(interleaved.data, baseline.data, baseline.size) == 0 &&
            interleaved.data[4] == 0xAC &&
            interleaved.data[6] == 0xA8
        ),
        "main and channel-3 FM frequency latches remain independent"
    );

    clock_converter shared_converter;
    memset(&shared_converter, 0, sizeof(shared_converter));
    shared_converter.source_clock = VGM_FIXTURE_CLOCK;
    shared_converter.pitch_conversion = true;
    byte_buffer shared = { 0 };
    bool shared_ok = (
        converter_write(&shared_converter, &shared, 0xA4, 0x22) &&
        converter_write(&shared_converter, &shared, 0xA0, 0x34) &&
        converter_write(&shared_converter, &shared, 0xA5, 0x1A) &&
        converter_write(&shared_converter, &shared, 0xA1, 0x56) &&
        converter_write(&shared_converter, &shared, 0xA4, 0x22) &&
        converter_write(&shared_converter, &shared, 0xA0, 0x78)
    );
    uint8_t expected_registers[] = {
        0xA4, 0xA0,
        0xA5, 0xA1,
        0xA4, 0xA0
    };
    bool register_order_ok = shared_ok && shared.size == 12;
    for (uint32_t i = 0; register_order_ok && i < sizeof expected_registers; i += 1) {
        register_order_ok = shared.data[i * 2] == expected_registers[i];
    }
    check(register_order_ok, "FM conversion restores the shared high latch before every commit");

    buffer_free(&shared);
    buffer_free(&interleaved);
    buffer_free(&baseline);
}

static void test_chip_volume_metadata_ignored(void) {
    const uint16_t gains[] = { 0x8200, 0x819A };
    const char *descriptions[] = {
        "relative unity chip-volume metadata is ignored",
        "relative fractional chip-volume metadata is ignored"
    };
    for (uint32_t i = 0; i < sizeof gains / sizeof gains[0]; i += 1) {
        file_data plain_file = make_test_vgm();
        file_data metadata_file = make_volume_metadata_test_vgm(gains[i]);
        qlvgm_error plain_error = { { 0 } };
        qlvgm_error metadata_error = { { 0 } };
        vgm_song plain_song;
        vgm_song metadata_song;
        bool plain_parsed = vgm_parse(plain_file.data, plain_file.size, &plain_song, &plain_error);
        bool metadata_parsed = vgm_parse(
            metadata_file.data,
            metadata_file.size,
            &metadata_song,
            &metadata_error
        );
        converted_song plain_converted;
        converted_song metadata_converted;
        bool plain_ok = plain_parsed && vgm_convert(
            &plain_song,
            50,
            true,
            &plain_converted,
            &plain_error
        );
        bool metadata_ok = metadata_parsed && vgm_convert(
            &metadata_song,
            50,
            true,
            &metadata_converted,
            &metadata_error
        );
        bool same = plain_ok && metadata_ok &&
            plain_converted.loop_offset == metadata_converted.loop_offset &&
            plain_converted.frame_count == metadata_converted.frame_count &&
            plain_converted.loop_frame_count == metadata_converted.loop_frame_count &&
            plain_converted.clipped_values == metadata_converted.clipped_values &&
            plain_converted.stream.size == metadata_converted.stream.size &&
            memcmp(
                plain_converted.stream.data,
                metadata_converted.stream.data,
                plain_converted.stream.size
            ) == 0;
        check(same, descriptions[i]);
        if (plain_ok) {
            converted_song_free(&plain_converted);
        }
        if (metadata_ok) {
            converted_song_free(&metadata_converted);
        }
        vgm_song_free(&plain_song);
        vgm_song_free(&metadata_song);
        free(plain_file.data);
        free(metadata_file.data);
    }
}

static void test_qlz_round_trip(void) {
    byte_buffer stream = { 0 };
    for (uint32_t i = 0; i < 40000; i += 1) {
        buffer_append_be16(&stream, 0);
    }
    qlvgm_error error = { { 0 } };
    qlz_stream compressed;
    bool packed = qlz_stream_create(&stream, &compressed, &error);
    check(packed && compressed.raw_size > UINT16_MAX, "QLZ supports one stream larger than 64K");
    qlz_stream second;
    bool packed_again = packed && qlz_stream_create(&stream, &second, &error);
    check(
        (
            packed_again &&
            second.data.size == compressed.data.size &&
            memcmp(second.data.data, compressed.data.data, compressed.data.size) == 0
        ),
        "QLZ compression is deterministic"
    );
    byte_buffer decoded = { 0 };
    bool decoded_ok = packed && qlz_decode(compressed.data.data, compressed.data.size, &decoded, &error);
    bool matches = (
        decoded_ok &&
        decoded.size == stream.size &&
        memcmp(decoded.data, stream.data, stream.size) == 0
    );
    if (!matches) {
        printf(
            "QLZ round-trip diagnostic: ok=%d decoded=%" PRIu32 "/%" PRIu32
            " packed=%" PRIu32 " error=%s\n",
            decoded_ok,
            decoded.size,
            stream.size,
            compressed.data.size,
            error.text
        );
    }
    check(matches, "large QLZ stream round trips");
    buffer_free(&decoded);
    if (packed_again) {
        qlz_stream_free(&second);
    }
    qlz_stream_free(&compressed);
    buffer_free(&stream);
}

static void test_qlz_format(void) {
    byte_buffer payload = { 0 };
    byte_buffer expected = { 0 };
    buffer_append_byte(&payload, 0x3F);
    for (uint32_t i = 0; i < 64; i += 1) {
        buffer_append_byte(&payload, (uint8_t)i);
        buffer_append_byte(&expected, (uint8_t)i);
    }
    buffer_append_byte(&payload, 0x00);
    buffer_append_byte(&payload, 0xA5);
    buffer_append_byte(&expected, 0xA5);
    buffer_append_byte(&payload, 0x40);
    buffer_append_byte(&payload, 0);
    for (uint32_t i = 0; i < 3; i += 1) {
        buffer_append_byte(&expected, 0xA5);
    }
    buffer_append_byte(&payload, 0xFF);
    for (uint32_t i = 0; i < 66; i += 1) {
        buffer_append_byte(&expected, 0xA5);
    }
    byte_buffer encoded = make_qlz_test_stream(payload.data, payload.size, expected.size);
    byte_buffer decoded;
    qlvgm_error error;
    bool ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(
        ok && decoded.size == expected.size && memcmp(decoded.data, expected.data, expected.size) == 0,
        "QLZ literal, short-match, and continuation boundaries decode"
    );
    buffer_free(&decoded);
    buffer_free(&encoded);
    buffer_free(&expected);
    buffer_free(&payload);

    memset(&payload, 0, sizeof(payload));
    memset(&expected, 0, sizeof(expected));
    test_qlz_append_literals(&payload, &expected, 257);
    buffer_append_byte(&payload, 0x40);
    buffer_append_byte(&payload, 255);
    test_qlz_append_expected_match(&expected, 256, 3);
    buffer_append_byte(&payload, 0x80);
    buffer_append_be16(&payload, 257);
    test_qlz_append_expected_match(&expected, 257, 3);
    encoded = make_qlz_test_stream(payload.data, payload.size, expected.size);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(
        ok && decoded.size == expected.size && memcmp(decoded.data, expected.data, expected.size) == 0,
        "QLZ distinguishes 256-byte and 257-byte match distances"
    );
    buffer_free(&decoded);
    buffer_free(&encoded);
    buffer_free(&expected);
    buffer_free(&payload);

    memset(&payload, 0, sizeof(payload));
    memset(&expected, 0, sizeof(expected));
    test_qlz_append_literals(&payload, &expected, QLZ_MAX_DISTANCE);
    buffer_append_byte(&payload, 0x80);
    buffer_append_be16(&payload, QLZ_MAX_DISTANCE);
    test_qlz_append_expected_match(&expected, QLZ_MAX_DISTANCE, 3);
    encoded = make_qlz_test_stream(payload.data, payload.size, expected.size);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(
        ok && decoded.size == expected.size && memcmp(decoded.data, expected.data, expected.size) == 0,
        "QLZ decodes the maximum 65,535-byte match distance"
    );
    buffer_free(&decoded);
    buffer_free(&encoded);
    buffer_free(&expected);
    buffer_free(&payload);

    uint8_t bad_continuation[] = { 0xC0 };
    encoded = make_qlz_test_stream(bad_continuation, sizeof bad_continuation, 3);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "continuation") != NULL, "QLZ continuation requires a preceding match");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t continuation_after_literal[] = { 0x00, 0x11, 0xC0 };
    encoded = make_qlz_test_stream(continuation_after_literal, sizeof continuation_after_literal, 4);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "continuation") != NULL, "QLZ literals clear continuation state");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t zero_distance[] = { 0x00, 0x11, 0x80, 0, 0 };
    encoded = make_qlz_test_stream(zero_distance, sizeof zero_distance, 4);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "zero") != NULL, "QLZ rejects a zero long-match distance");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t too_far[] = { 0x00, 0x11, 0x40, 1 };
    encoded = make_qlz_test_stream(too_far, sizeof too_far, 4);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "match") != NULL, "QLZ rejects a match before its history");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t truncated_literal[] = { 0x03, 1 };
    encoded = make_qlz_test_stream(truncated_literal, sizeof truncated_literal, 4);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "literal") != NULL, "QLZ rejects a truncated literal");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t truncated_short_match[] = { 0x40 };
    encoded = make_qlz_test_stream(truncated_short_match, sizeof truncated_short_match, 3);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "short match") != NULL, "QLZ rejects a truncated short match");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t truncated_long_match[] = { 0x80, 1 };
    encoded = make_qlz_test_stream(truncated_long_match, sizeof truncated_long_match, 3);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "long match") != NULL, "QLZ rejects a truncated long match");
    buffer_free(&decoded);
    buffer_free(&encoded);

    uint8_t trailing_payload[] = { 0 };
    encoded = make_qlz_test_stream(trailing_payload, sizeof trailing_payload, 0);
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "trailing") != NULL, "QLZ rejects trailing payload");
    buffer_free(&decoded);
    buffer_free(&encoded);

    encoded = make_qlz_test_stream(NULL, 0, 0);
    encoded.data[0] = 'X';
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "header") != NULL, "QLZ rejects an unknown format version");
    buffer_free(&decoded);
    buffer_free(&encoded);

    encoded = make_qlz_test_stream(NULL, 0, 0);
    encoded.data[11] = 1;
    ok = test_qlz_decode_stream(&encoded, &decoded, &error);
    check(!ok && strstr(error.text, "sizes") != NULL, "QLZ rejects a mismatched payload size");
    buffer_free(&decoded);
    buffer_free(&encoded);

    byte_buffer empty = { 0 };
    qlz_stream compressed;
    memset(&error, 0, sizeof(error));
    ok = qlz_stream_create(&empty, &compressed, &error);
    if (ok) {
        ok = test_qlz_decode_stream(&compressed.data, &decoded, &error);
    }
    check(ok && decoded.size == 0, "empty QLZ stream round trips");
    buffer_free(&decoded);
    qlz_stream_free(&compressed);
}

static void test_clock_and_rate_validation(void) {
    file_data file = make_test_vgm();
    qlvgm_error error = { { 0 } };
    vgm_song song;
    test_write_le32(file.data, 0x44, 3579545);
    bool parsed = vgm_parse(file.data, file.size, &song, &error);
    check(!parsed && strstr(error.text, "unsupported YM2203 clock") != NULL, "unknown YM2203 clock is rejected");
    vgm_song_free(&song);

    test_write_le32(file.data, 0x44, VGM_FIXTURE_CLOCK);
    file.data[0x81] = 0x24;
    memset(&error, 0, sizeof(error));
    parsed = vgm_parse(file.data, file.size, &song, &error);
    check(
        !parsed && strstr(error.text, "clock-dependent YM2203 register") != NULL,
        "timer programming is rejected when the master clock changes"
    );
    vgm_song_free(&song);

    file.data[0x81] = 0x00;
    test_write_le32(file.data, 0x18, 2645);
    memset(&error, 0, sizeof(error));
    parsed = vgm_parse(file.data, file.size, &song, &error);
    check(!parsed && strstr(error.text, "sample count mismatch") != NULL, "bad total sample count is rejected");
    vgm_song_free(&song);
    free(file.data);
}

static void test_qlay_output(void) {
    char directory[] = "/tmp/qlvgm-qlay-test-XXXXXX";
    if (mkdtemp(directory) == NULL) {
        check(false, "QLAY test directory is created");
        return;
    }
    char path[QLVGM_PATH_BYTES];
    int length = snprintf(path, sizeof path, "%s/image.mdv", directory);
    if (length < 0 || (uint32_t)length >= sizeof path) {
        check(false, "QLAY test path is created");
        (void)rmdir(directory);
        return;
    }

    uint8_t medium_name[QLAY_MEDIUM_NAME_SIZE];
    qlay_medium_name("/tmp/Castle.MDV", NULL, medium_name);
    check(memcmp(medium_name, "Castle    ", sizeof medium_name) == 0, "QLAY default medium name is derived");
    qlay_medium_name(path, "hosted", medium_name);

    uint8_t boot[] = { 1, 2, 3 };
    uint8_t player[] = { 4, 5, 6, 7 };
    uint8_t screen[] = { 8, 9, 10, 11, 12 };
    qlay_file files[] = {
        { .name = "BOOT", .data = boot, .size = sizeof boot },
        { .name = "qlvgm", .data = player, .size = sizeof player },
        { .name = "screen", .data = screen, .size = sizeof screen }
    };
    qlvgm_error error = { { 0 } };
    bool created = qlay_write_image(
        path,
        false,
        QLAY_MIN_SECTORS,
        medium_name,
        0x004D,
        files,
        sizeof files / sizeof files[0],
        &error
    );
    check(created, "QLAY image is created");

    uint32_t image_size = QLAY_MIN_SECTORS * QLAY_SECTOR_SIZE;
    uint8_t *image = malloc(image_size);
    FILE *file = NULL;
    if (created && image != NULL) {
        file = fopen(path, "rb");
    }
    bool loaded = file != NULL && fread(image, 1, image_size, file) == image_size && fgetc(file) == EOF;
    if (file != NULL) {
        loaded = fclose(file) == 0 && loaded;
    }
    check(loaded, "QLAY image has the selected geometry");
    if (loaded) {
        uint8_t *map_sector = image;
        uint8_t *map = map_sector + QLAY_DATA_OFFSET;
        uint8_t *directory_sector = image + (QLAY_MIN_SECTORS - 1) * QLAY_SECTOR_SIZE;
        uint8_t *directory_data = directory_sector + QLAY_DATA_OFFSET;
        uint8_t *boot_sector = image + (QLAY_MIN_SECTORS - 2) * QLAY_SECTOR_SIZE;
        uint8_t *player_sector = image + (QLAY_MIN_SECTORS - 3) * QLAY_SECTOR_SIZE;
        uint8_t *screen_sector = image + (QLAY_MIN_SECTORS - 4) * QLAY_SECTOR_SIZE;
        check(
            map_sector[QLAY_SECTOR_HEADER_OFFSET + 1] == 0 &&
                directory_sector[QLAY_SECTOR_HEADER_OFFSET + 1] == 1 &&
                boot_sector[QLAY_SECTOR_HEADER_OFFSET + 1] == 2 &&
                player_sector[QLAY_SECTOR_HEADER_OFFSET + 1] == 3 &&
                screen_sector[QLAY_SECTOR_HEADER_OFFSET + 1] == 4,
            "QLAY physical ordering is map-first descending"
        );
        check(
            map[0] == QLAY_MAP_FILE_ID && map[2] == 0 && map[4] == 1 && map[6] == 2 &&
                map[8] == 3 && map[QLAY_DATA_SIZE - 1] == 4,
            "QLAY allocation map contains the directory and files"
        );
        check(
            test_read_be32(directory_data) == 4 * QLAY_QDOS_HEADER_SIZE &&
                test_read_be32(directory_data + QLAY_QDOS_HEADER_SIZE) == sizeof boot + QLAY_QDOS_HEADER_SIZE &&
                test_read_be16(
                    directory_data + QLAY_QDOS_HEADER_SIZE + QLAY_QDOS_NAME_LENGTH_OFFSET
                ) == 4 &&
                memcmp(
                    directory_data + QLAY_QDOS_HEADER_SIZE + QLAY_QDOS_NAME_OFFSET,
                    "BOOT",
                    4
                ) == 0 &&
                test_read_be32(
                    directory_data + QLAY_QDOS_HEADER_SIZE + QLAY_QDOS_UPDATE_OFFSET
                ) == QLAY_FIXED_UPDATE_TIME,
            "QLAY directory contains deterministic BOOT metadata"
        );
        check(
            memcmp(boot_sector + QLAY_DATA_OFFSET + QLAY_QDOS_HEADER_SIZE, boot, sizeof boot) == 0 &&
                memcmp(player_sector + QLAY_DATA_OFFSET + QLAY_QDOS_HEADER_SIZE, player, sizeof player) == 0 &&
                memcmp(screen_sector + QLAY_DATA_OFFSET + QLAY_QDOS_HEADER_SIZE, screen, sizeof screen) == 0,
            "QLAY file blocks contain their payloads"
        );
        uint16_t sector_checksum = (uint16_t)(
            map_sector[QLAY_SECTOR_HEADER_CHECKSUM_OFFSET] |
            (uint16_t)map_sector[QLAY_SECTOR_HEADER_CHECKSUM_OFFSET + 1] << 8
        );
        uint16_t block_checksum = (uint16_t)(
            map_sector[QLAY_BLOCK_HEADER_CHECKSUM_OFFSET] |
            (uint16_t)map_sector[QLAY_BLOCK_HEADER_CHECKSUM_OFFSET + 1] << 8
        );
        uint16_t data_checksum = (uint16_t)(
            map_sector[QLAY_DATA_CHECKSUM_OFFSET] |
            (uint16_t)map_sector[QLAY_DATA_CHECKSUM_OFFSET + 1] << 8
        );
        check(
            sector_checksum == qlay_checksum(
                map_sector + QLAY_SECTOR_HEADER_OFFSET,
                QLAY_SECTOR_HEADER_SIZE
            ) &&
                block_checksum == qlay_checksum(
                    map_sector + QLAY_BLOCK_HEADER_OFFSET,
                    QLAY_BLOCK_HEADER_SIZE
                ) &&
                data_checksum == qlay_checksum(map, QLAY_DATA_SIZE),
            "QLAY sector checksums are valid"
        );
    }
    free(image);

    memset(&error, 0, sizeof(error));
    check(
        !qlay_write_image(
            path,
            false,
            QLAY_MIN_SECTORS,
            medium_name,
            0x004D,
            files,
            sizeof files / sizeof files[0],
            &error
        ) && strstr(error.text, "already exists") != NULL,
        "QLAY existing output requires force"
    );
    memset(&error, 0, sizeof(error));
    check(
        qlay_write_image(
            path,
            true,
            QLAY_MIN_SECTORS,
            medium_name,
            0x004D,
            files,
            2,
            &error
        ),
        "QLAY two-file forced output replacement succeeds"
    );
    qlay_file oversized = { .name = "large", .data = player, .size = QLAY_DATA_SIZE * QLAY_MAX_SECTORS };
    memset(&error, 0, sizeof(error));
    check(
        !qlay_write_image(
            path,
            true,
            QLAY_MIN_SECTORS,
            medium_name,
            0x004D,
            &oversized,
            1,
            &error
        ) && strstr(error.text, "capacity") != NULL,
        "QLAY capacity is checked before rendering"
    );
    (void)qlay_random_id();
    (void)unlink(path);
    (void)rmdir(directory);
}

int main(void) {
    test_vgm_conversion();
    test_fm_latch_conversion();
    test_chip_volume_metadata_ignored();
    test_qlz_round_trip();
    test_qlz_format();
    test_clock_and_rate_validation();
    test_qlay_output();
    if (test_failures != 0) {
        printf("%d test(s) failed\n", test_failures);
        return 1;
    }
    puts("all qlvgm tests passed");
    return 0;
}
