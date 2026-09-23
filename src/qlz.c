// QLZ1 uses a 12-byte big-endian size header and four byte-oriented packet
// classes so an original MC68000 can decode unaligned payloads efficiently.
enum {
    QLZ_HEADER_BYTES = 12,
    QLZ_HASH_COUNT = 65536,
    QLZ_CHAIN_LIMIT = 256,
    QLZ_MAX_DISTANCE = 65535,
    QLZ_LITERAL_MAX = 64,
    QLZ_MATCH_MIN = 3,
    QLZ_MATCH_MAX = 66
};

typedef struct {
    byte_buffer data;
    uint32_t raw_size;
} qlz_stream;

typedef struct {
    uint32_t distance;
    uint32_t length;
} qlz_match;

static uint32_t qlz_hash(const uint8_t *data) {
    uint32_t value = (uint32_t)data[0] * 251u + data[1];
    return (value * 251u + data[2]) & (QLZ_HASH_COUNT - 1);
}

static bool qlz_append_be32(byte_buffer *buffer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value
    };
    return buffer_append(buffer, bytes, sizeof bytes);
}

static void qlz_insert_position(
    const uint8_t *input,
    uint32_t input_size,
    int32_t heads[QLZ_HASH_COUNT],
    int32_t *previous,
    uint32_t position
) {
    if (position + 2 >= input_size) {
        return;
    }
    uint32_t hash = qlz_hash(input + position);
    previous[position] = heads[hash];
    heads[hash] = (int32_t)position;
}

static void qlz_find_matches(
    const uint8_t *input,
    uint32_t input_size,
    const int32_t heads[QLZ_HASH_COUNT],
    const int32_t *previous,
    uint32_t position,
    qlz_match *short_match,
    qlz_match *long_match
) {
    memset(short_match, 0, sizeof(*short_match));
    memset(long_match, 0, sizeof(*long_match));
    if (position + QLZ_MATCH_MIN > input_size) {
        return;
    }
    uint32_t hash = qlz_hash(input + position);
    int32_t candidate = heads[hash];
    uint32_t visits = 0;
    uint32_t maximum = input_size - position;
    while (candidate >= 0 && visits < QLZ_CHAIN_LIMIT) {
        uint32_t distance = position - (uint32_t)candidate;
        if (distance > QLZ_MAX_DISTANCE) {
            break;
        }
        uint32_t length = 0;
        while (length < maximum && input[(uint32_t)candidate + length] == input[position + length]) {
            length += 1;
        }
        qlz_match *match = long_match;
        if (distance <= 256) {
            match = short_match;
        }
        if (length > match->length || (length == match->length && distance < match->distance)) {
            match->distance = distance;
            match->length = length;
        }
        candidate = previous[candidate];
        visits += 1;
    }
}

static uint32_t qlz_match_packets(uint32_t length) {
    return (length + QLZ_MATCH_MAX - 1) / QLZ_MATCH_MAX;
}

static int64_t qlz_match_saving(qlz_match match) {
    if (match.length < QLZ_MATCH_MIN) {
        return INT64_MIN;
    }
    uint32_t offset_bytes = 2;
    if (match.distance <= 256) {
        offset_bytes = 1;
    }
    uint32_t encoded = offset_bytes + qlz_match_packets(match.length);
    return (int64_t)match.length - encoded;
}

static qlz_match qlz_choose_match(qlz_match short_match, qlz_match long_match) {
    int64_t short_saving = qlz_match_saving(short_match);
    int64_t long_saving = qlz_match_saving(long_match);
    qlz_match match = short_match;
    if (
        long_saving > short_saving ||
        (long_saving == short_saving && long_match.length > short_match.length)
    ) {
        match = long_match;
    }
    uint32_t minimum = QLZ_MATCH_MIN;
    if (match.distance > 256) {
        minimum = QLZ_MATCH_MIN + 1;
    }
    if (match.length < minimum || qlz_match_saving(match) <= 0) {
        memset(&match, 0, sizeof(match));
    }
    return match;
}

static bool qlz_write_literals(
    byte_buffer *payload,
    const uint8_t *input,
    uint32_t start,
    uint32_t length
) {
    while (length != 0) {
        uint32_t count = length;
        if (count > QLZ_LITERAL_MAX) {
            count = QLZ_LITERAL_MAX;
        }
        if (
            !buffer_append_byte(payload, (uint8_t)(count - 1)) ||
            !buffer_append(payload, input + start, count)
        ) {
            return false;
        }
        start += count;
        length -= count;
    }
    return true;
}

static uint32_t qlz_match_part(uint32_t remaining) {
    uint32_t count = remaining;
    if (count > QLZ_MATCH_MAX) {
        count = QLZ_MATCH_MAX;
    }
    uint32_t tail = remaining - count;
    if (tail != 0 && tail < QLZ_MATCH_MIN) {
        count -= QLZ_MATCH_MIN - tail;
    }
    return count;
}

static bool qlz_write_match(byte_buffer *payload, qlz_match match) {
    uint32_t count = qlz_match_part(match.length);
    uint8_t length_code = (uint8_t)(count - QLZ_MATCH_MIN);
    if (match.distance <= 256) {
        if (
            !buffer_append_byte(payload, 0x40 | length_code) ||
            !buffer_append_byte(payload, (uint8_t)(match.distance - 1))
        ) {
            return false;
        }
    } else if (
        !buffer_append_byte(payload, 0x80 | length_code) ||
        !buffer_append_be16(payload, (uint16_t)match.distance)
    ) {
        return false;
    }
    uint32_t remaining = match.length - count;
    while (remaining != 0) {
        count = qlz_match_part(remaining);
        if (!buffer_append_byte(payload, 0xC0 | (uint8_t)(count - QLZ_MATCH_MIN))) {
            return false;
        }
        remaining -= count;
    }
    return true;
}

static bool qlz_compress_payload(
    const uint8_t *input,
    uint32_t input_size,
    byte_buffer *payload,
    qlvgm_error *error
) {
    if (input_size == 0) {
        return true;
    }
    int32_t *heads = malloc(sizeof(*heads) * QLZ_HASH_COUNT);
    int32_t *previous = malloc(sizeof(*previous) * input_size);
    if (heads == NULL || previous == NULL) {
        free(heads);
        free(previous);
        error_set(error, "out of memory finding QLZ matches");
        return false;
    }
    for (uint32_t i = 0; i < QLZ_HASH_COUNT; i += 1) {
        heads[i] = -1;
    }
    for (uint32_t i = 0; i < input_size; i += 1) {
        previous[i] = -1;
    }

    uint32_t literal_start = 0;
    uint32_t position = 0;
    while (position < input_size && !payload->failed) {
        qlz_match short_match;
        qlz_match long_match;
        qlz_find_matches(
            input,
            input_size,
            heads,
            previous,
            position,
            &short_match,
            &long_match
        );
        qlz_match match = qlz_choose_match(short_match, long_match);
        bool position_inserted = false;
        if (match.length != 0 && position + 1 < input_size) {
            qlz_insert_position(input, input_size, heads, previous, position);
            position_inserted = true;
            qlz_match next_short;
            qlz_match next_long;
            qlz_find_matches(
                input,
                input_size,
                heads,
                previous,
                position + 1,
                &next_short,
                &next_long
            );
            qlz_match next = qlz_choose_match(next_short, next_long);
            if (qlz_match_saving(next) > qlz_match_saving(match) + 1) {
                position += 1;
                continue;
            }
        }
        if (match.length == 0) {
            if (!position_inserted) {
                qlz_insert_position(input, input_size, heads, previous, position);
            }
            position += 1;
            continue;
        }
        if (!qlz_write_literals(payload, input, literal_start, position - literal_start)
            || !qlz_write_match(payload, match)) {
            break;
        }
        uint32_t end = position + match.length;
        uint32_t insert_start = position;
        if (position_inserted) {
            insert_start += 1;
        }
        for (uint32_t i = insert_start; i < end; i += 1) {
            qlz_insert_position(input, input_size, heads, previous, i);
        }
        position = end;
        literal_start = position;
    }
    if (!payload->failed && literal_start < input_size) {
        qlz_write_literals(payload, input, literal_start, input_size - literal_start);
    }
    free(heads);
    free(previous);
    if (payload->failed) {
        error_set(error, "out of memory writing QLZ stream");
        return false;
    }
    return true;
}

static bool qlz_stream_create(
    const byte_buffer *input,
    qlz_stream *stream,
    qlvgm_error *error
) {
    memset(stream, 0, sizeof(*stream));
    stream->raw_size = input->size;
    byte_buffer payload = { 0 };
    if (!qlz_compress_payload(input->data, input->size, &payload, error)) {
        buffer_free(&payload);
        return false;
    }
    bool ok = (
        buffer_append(&stream->data, "QLZ1", 4) &&
        qlz_append_be32(&stream->data, input->size) &&
        qlz_append_be32(&stream->data, payload.size) &&
        buffer_append(&stream->data, payload.data, payload.size)
    );
    buffer_free(&payload);
    if (!ok) {
        error_set(error, "out of memory collecting QLZ stream");
        buffer_free(&stream->data);
        return false;
    }
    return true;
}

static void qlz_stream_free(qlz_stream *stream) {
    buffer_free(&stream->data);
    memset(stream, 0, sizeof(*stream));
}

#ifdef QLVGM_TEST

static uint32_t qlz_read_be32(const uint8_t *data) {
    return (
        (uint32_t)data[0] << 24 |
        (uint32_t)data[1] << 16 |
        (uint32_t)data[2] << 8 |
        data[3]
    );
}

static bool qlz_decode(
    const uint8_t *input,
    uint32_t input_size,
    byte_buffer *output,
    qlvgm_error *error
) {
    if (input_size < QLZ_HEADER_BYTES || memcmp(input, "QLZ1", 4) != 0) {
        error_set(error, "invalid QLZ header");
        return false;
    }
    uint32_t decoded_size = qlz_read_be32(input + 4);
    uint32_t payload_size = qlz_read_be32(input + 8);
    if (payload_size != input_size - QLZ_HEADER_BYTES
        || !buffer_reserve(output, decoded_size)) {
        error_set(error, "invalid QLZ sizes");
        return false;
    }
    uint32_t input_at = QLZ_HEADER_BYTES;
    uint32_t input_end = input_size;
    uint32_t output_start = output->size;
    uint32_t output_end = output_start + decoded_size;
    uint32_t last_distance = 0;
    bool can_continue = false;
    while (output->size < output_end) {
        if (input_at >= input_end) {
            error_set(error, "truncated QLZ payload");
            return false;
        }
        uint8_t control = input[input_at];
        input_at += 1;
        uint32_t type = control >> 6;
        uint32_t length = 0;
        if (type == 0) {
            length = (control & 0x3F) + 1;
            if (length > input_end - input_at || length > output_end - output->size) {
                error_set(error, "invalid QLZ literal");
                return false;
            }
            buffer_append(output, input + input_at, length);
            input_at += length;
            can_continue = false;
            continue;
        }
        length = (control & 0x3F) + QLZ_MATCH_MIN;
        if (type == 1) {
            if (input_at >= input_end) {
                error_set(error, "truncated QLZ short match");
                return false;
            }
            last_distance = (uint32_t)input[input_at] + 1;
            input_at += 1;
        } else if (type == 2) {
            if (input_end - input_at < 2) {
                error_set(error, "truncated QLZ long match");
                return false;
            }
            last_distance = (uint32_t)input[input_at] << 8 | input[input_at + 1];
            input_at += 2;
            if (last_distance == 0) {
                error_set(error, "zero QLZ match distance");
                return false;
            }
        } else if (!can_continue) {
            error_set(error, "invalid QLZ continuation");
            return false;
        }
        if (last_distance > output->size - output_start || length > output_end - output->size) {
            error_set(error, "invalid QLZ match");
            return false;
        }
        for (uint32_t i = 0; i < length; i += 1) {
            buffer_append_byte(output, output->data[output->size - last_distance]);
        }
        can_continue = true;
    }
    if (input_at != input_end) {
        error_set(error, "trailing QLZ payload");
        return false;
    }
    return true;
}

#endif
