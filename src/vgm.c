enum {
    VGM_TARGET_CLOCK = 2000000,
    VGM_FIXTURE_CLOCK = 3993600,
    VGM_NO_LOOP = UINT32_MAX,
    VGM_DEFAULT_SSG_GAIN = 0x0080,
    VGM_SSG_GAIN_HIGH_REGISTER = 0xF1,
    VGM_SSG_GAIN_LOW_REGISTER = 0xF0
};

typedef struct {
    uint32_t sample;
    uint8_t reg;
    uint8_t value;
} vgm_write;

typedef struct {
    vgm_write *writes;
    uint32_t write_count;
    uint32_t write_capacity;
    uint32_t total_samples;
    uint32_t loop_sample;
    uint32_t loop_write;
    uint32_t clock_hz;
    uint16_t ssg_gain;
    bool has_loop;
} vgm_song;

typedef struct {
    byte_buffer stream;
    uint32_t loop_offset;
    uint32_t frame_count;
    uint32_t loop_frame_count;
    uint32_t clipped_values;
} converted_song;

typedef struct {
    uint8_t source[256];
    uint8_t target[256];
    bool target_known[256];
    uint32_t source_clock;
    uint32_t clipped_values;
    uint8_t fm_high_latch;
    uint8_t fm_channel3_high_latch;
    bool pitch_conversion;
} clock_converter;

static bool vgm_add_write(
    vgm_song *song,
    uint32_t sample,
    uint8_t reg,
    uint8_t value,
    qlvgm_error *error
) {
    if (song->write_count == song->write_capacity) {
        uint32_t capacity = 4096;
        if (song->write_capacity != 0) {
            capacity = song->write_capacity * 2;
        }
        if (capacity < song->write_capacity) {
            error_set(error, "too many YM2203 writes");
            return false;
        }
        vgm_write *writes = realloc(song->writes, (size_t)capacity * sizeof(*writes));
        if (writes == NULL) {
            error_set(error, "out of memory collecting YM2203 writes");
            return false;
        }
        song->writes = writes;
        song->write_capacity = capacity;
    }
    song->writes[song->write_count] = (vgm_write){
        .sample = sample,
        .reg = reg,
        .value = value
    };
    song->write_count += 1;
    return true;
}

static bool vgm_add_samples(uint64_t *samples, uint32_t amount, qlvgm_error *error) {
    *samples += amount;
    if (*samples > UINT32_MAX) {
        error_set(error, "VGM duration exceeds 32-bit sample range");
        return false;
    }
    return true;
}

static bool vgm_parse(const uint8_t *data, uint32_t size, vgm_song *song, qlvgm_error *error) {
    memset(song, 0, sizeof(*song));
    song->ssg_gain = VGM_DEFAULT_SSG_GAIN;
    if (size < 0x48 || memcmp(data, "Vgm ", 4) != 0) {
        error_set(error, "input is not a supported VGM file");
        return false;
    }
    uint32_t version = read_le32(data + 0x08);
    if (version < 0x150 || version > 0x171) {
        error_set(error, "unsupported VGM version %x.%02x", version >> 8, version & 0xFF);
        return false;
    }
    uint64_t eof64 = (uint64_t)read_le32(data + 0x04) + 4;
    if (eof64 > size || eof64 < 0x40) {
        error_set(error, "invalid VGM EOF offset");
        return false;
    }
    uint32_t eof = (uint32_t)eof64;
    uint32_t relative_data = read_le32(data + 0x34);
    uint32_t data_offset = 0x40;
    if (relative_data != 0) {
        data_offset = 0x34 + relative_data;
    }
    if (data_offset < 0x40 || data_offset >= eof) {
        error_set(error, "invalid VGM data offset");
        return false;
    }

    uint32_t clock = read_le32(data + 0x44);
    if ((clock & 0xC0000000u) != 0) {
        error_set(error, "dual or flagged YM2203 clocks are not supported");
        return false;
    }
    if (clock != VGM_TARGET_CLOCK && clock != VGM_FIXTURE_CLOCK) {
        error_set(
            error,
            "unsupported YM2203 clock %" PRIu32 " Hz; expected %u or %u Hz",
            clock,
            VGM_TARGET_CLOCK,
            VGM_FIXTURE_CLOCK
        );
        return false;
    }
    song->clock_hz = clock;

    if (version >= 0x170 && data_offset >= 0xC0) {
        uint32_t extra_relative = read_le32(data + 0xBC);
        if (extra_relative != 0) {
            uint64_t extra64 = (uint64_t)0xBC + extra_relative;
            if (extra64 + 12 > data_offset) {
                error_set(error, "invalid VGM extra header");
                return false;
            }
            uint32_t extra = (uint32_t)extra64;
            uint32_t extra_length = read_le32(data + extra);
            if (extra_length >= 12) {
                uint32_t volume_relative = read_le32(data + extra + 8);
                if (volume_relative != 0) {
                    uint64_t volume64 = (uint64_t)extra + 8 + volume_relative;
                    if (volume64 >= data_offset) {
                        error_set(error, "invalid VGM chip-volume header");
                        return false;
                    }
                    uint32_t volume = (uint32_t)volume64;
                    uint32_t count = data[volume];
                    if ((uint64_t)volume + 1 + (uint64_t)count * 4 > data_offset) {
                        error_set(error, "truncated VGM chip-volume header");
                        return false;
                    }
                    for (uint32_t i = 0; i < count; i += 1) {
                        uint32_t at = volume + 1 + i * 4;
                        uint8_t type = data[at];
                        uint8_t flags = data[at + 1];
                        uint16_t gain = read_le16(data + at + 2);
                        if (type != 0x86 || (flags & 1) != 0) {
                            continue;
                        }
                        if ((gain & 0x8000) != 0) {
                            uint32_t relative = gain & 0x7FFF;
                            song->ssg_gain = (uint16_t)((VGM_DEFAULT_SSG_GAIN * relative + 0x80) >> 8);
                        } else {
                            song->ssg_gain = gain;
                        }
                        break;
                    }
                }
            }
        }
    }

    uint32_t relative_loop = read_le32(data + 0x1C);
    uint32_t loop_offset = 0;
    if (relative_loop != 0) {
        uint64_t loop64 = (uint64_t)relative_loop + 0x1C;
        if (loop64 < data_offset || loop64 >= eof) {
            error_set(error, "invalid VGM loop offset");
            return false;
        }
        loop_offset = (uint32_t)loop64;
        song->has_loop = true;
    }

    uint32_t position = data_offset;
    uint64_t samples = 0;
    bool loop_found = false;
    bool end_found = false;
    while (position < eof) {
        uint32_t command_offset = position;
        if (song->has_loop && position == loop_offset) {
            song->loop_sample = (uint32_t)samples;
            song->loop_write = song->write_count;
            loop_found = true;
        }
        uint8_t command = data[position];
        position += 1;
        if (command == 0x55) {
            if (position + 2 > eof) {
                error_set(error, "truncated YM2203 write at VGM offset 0x%X", command_offset);
                break;
            }
            uint8_t reg = data[position];
            uint8_t value = data[position + 1];
            position += 2;
            if (clock != VGM_TARGET_CLOCK
                && (reg == 0x24 || reg == 0x25 || reg == 0x26
                    || reg == 0x2D || reg == 0x2E || reg == 0x2F)) {
                error_set(error, "clock-dependent YM2203 register 0x%02X is unsupported", reg);
                break;
            }
            if (!vgm_add_write(song, (uint32_t)samples, reg, value, error)) {
                break;
            }
            continue;
        }
        if (command == 0x61) {
            if (position + 2 > eof) {
                error_set(error, "truncated VGM wait at offset 0x%X", command_offset);
                break;
            }
            uint32_t amount = read_le16(data + position);
            position += 2;
            if (!vgm_add_samples(&samples, amount, error)) {
                break;
            }
            continue;
        }
        if (command == 0x62 || command == 0x63) {
            uint32_t amount = 882;
            if (command == 0x62) {
                amount = 735;
            }
            if (!vgm_add_samples(&samples, amount, error)) {
                break;
            }
            continue;
        }
        if (command >= 0x70 && command <= 0x7F) {
            if (!vgm_add_samples(&samples, (command & 0x0F) + 1, error)) {
                break;
            }
            continue;
        }
        if (command == 0x66) {
            end_found = true;
            break;
        }
        error_set(error, "unsupported VGM command 0x%02X at offset 0x%X", command, command_offset);
        break;
    }
    if (error->text[0] != '\0') {
        return false;
    }
    if (!end_found) {
        error_set(error, "VGM command stream has no end command");
        return false;
    }
    if (samples == 0) {
        error_set(error, "VGM contains no playback time");
        return false;
    }
    song->total_samples = (uint32_t)samples;
    uint32_t header_samples = read_le32(data + 0x18);
    if (header_samples != song->total_samples) {
        error_set(
            error,
            "VGM sample count mismatch: header %" PRIu32 ", commands %" PRIu32,
            header_samples,
            song->total_samples
        );
        return false;
    }
    if (song->has_loop) {
        if (!loop_found) {
            error_set(error, "VGM loop offset is not a command boundary");
            return false;
        }
        uint32_t loop_samples = song->total_samples - song->loop_sample;
        if (read_le32(data + 0x20) != loop_samples || loop_samples == 0) {
            error_set(error, "VGM loop sample count does not match its loop offset");
            return false;
        }
    } else if (read_le32(data + 0x20) != 0) {
        error_set(error, "VGM has loop samples without a loop offset");
        return false;
    }
    return true;
}

static void vgm_song_free(vgm_song *song) {
    free(song->writes);
    memset(song, 0, sizeof(*song));
}

static bool converter_emit(clock_converter *converter, byte_buffer *frame, uint8_t reg, uint8_t value) {
    converter->target[reg] = value;
    converter->target_known[reg] = true;
    return buffer_append_byte(frame, reg) && buffer_append_byte(frame, value);
}

static bool converter_emit_changed(
    clock_converter *converter,
    byte_buffer *frame,
    uint8_t reg,
    uint8_t value
) {
    if (converter->target_known[reg] && converter->target[reg] == value) {
        return true;
    }
    return converter_emit(converter, frame, reg, value);
}

static uint32_t scale_period(clock_converter *converter, uint32_t period, uint32_t maximum) {
    if (period == 0) {
        return 0;
    }
    uint64_t scaled = (uint64_t)period * VGM_TARGET_CLOCK + converter->source_clock / 2;
    scaled /= converter->source_clock;
    if (scaled == 0) {
        scaled = 1;
    }
    if (scaled > maximum) {
        scaled = maximum;
        converter->clipped_values += 1;
    }
    return (uint32_t)scaled;
}

static bool converter_emit_psg_pair(
    clock_converter *converter,
    byte_buffer *frame,
    uint8_t low_reg,
    uint8_t high_reg,
    uint32_t mask
) {
    uint32_t source = converter->source[low_reg] | (uint32_t)converter->source[high_reg] << 8;
    source &= mask;
    uint32_t target = scale_period(converter, source, mask);
    return converter_emit_changed(converter, frame, low_reg, (uint8_t)target)
        && converter_emit_changed(converter, frame, high_reg, (uint8_t)(target >> 8));
}

static uint64_t absolute_difference(uint64_t left, uint64_t right) {
    if (left >= right) {
        return left - right;
    }
    return right - left;
}

static bool converter_emit_fm_pair(
    clock_converter *converter,
    byte_buffer *frame,
    uint8_t low_reg,
    uint8_t high_reg,
    uint8_t low,
    uint8_t high
) {
    uint32_t source_fnum = (uint32_t)low | (high & 7u) << 8;
    uint32_t source_block = (high >> 3) & 7u;
    uint32_t best_fnum = 0;
    uint32_t best_block = source_block;
    uint64_t best_error = UINT64_MAX;
    uint64_t wanted = (uint64_t)converter->source_clock * source_fnum * (1u << source_block);
    for (uint32_t block = 0; block < 8; block += 1) {
        uint64_t divisor = (uint64_t)VGM_TARGET_CLOCK * (1u << block);
        uint64_t candidate = (wanted + divisor / 2) / divisor;
        if (candidate > 2047) {
            candidate = 2047;
        }
        uint64_t actual = divisor * candidate;
        uint64_t difference = absolute_difference(actual, wanted);
        if (difference < best_error) {
            best_error = difference;
            best_fnum = (uint32_t)candidate;
            best_block = block;
        }
    }
    if (source_fnum != 0 && best_fnum == 2047 && best_block == 7 && best_error != 0) {
        converter->clipped_values += 1;
    }
    uint8_t target_low = (uint8_t)best_fnum;
    uint8_t target_high = (uint8_t)((high & 0xC0) | best_block << 3 | best_fnum >> 8);
    return converter_emit(converter, frame, high_reg, target_high)
        && converter_emit(converter, frame, low_reg, target_low);
}

static bool converter_write(clock_converter *converter, byte_buffer *frame, uint8_t reg, uint8_t value) {
    converter->source[reg] = value;
    if (!converter->pitch_conversion || converter->source_clock == VGM_TARGET_CLOCK) {
        return converter_emit(converter, frame, reg, value);
    }
    if (reg <= 5) {
        uint8_t low_reg = reg & 0xFE;
        return converter_emit_psg_pair(converter, frame, low_reg, low_reg + 1, 0x0FFF);
    }
    if (reg == 6) {
        uint8_t scaled = (uint8_t)scale_period(converter, value & 0x1F, 0x1F);
        return converter_emit_changed(converter, frame, reg, scaled);
    }
    if (reg == 11 || reg == 12) {
        return converter_emit_psg_pair(converter, frame, 11, 12, 0xFFFF);
    }
    if (reg >= 0xA4 && reg <= 0xA6) {
        converter->fm_high_latch = value;
        return true;
    }
    if (reg >= 0xA0 && reg <= 0xA2) {
        return converter_emit_fm_pair(
            converter,
            frame,
            reg,
            reg + 4,
            value,
            converter->fm_high_latch
        );
    }
    if (reg >= 0xAC && reg <= 0xAE) {
        converter->fm_channel3_high_latch = value;
        return true;
    }
    if (reg >= 0xA8 && reg <= 0xAA) {
        return converter_emit_fm_pair(
            converter,
            frame,
            reg,
            reg + 4,
            value,
            converter->fm_channel3_high_latch
        );
    }
    return converter_emit(converter, frame, reg, value);
}

static bool convert_segment(
    const vgm_song *song,
    uint32_t first_write,
    uint32_t write_count,
    uint32_t start_sample,
    uint32_t duration,
    uint32_t rate,
    clock_converter *converter,
    uint16_t ssg_gain,
    bool emit_ssg_gain,
    byte_buffer *output,
    uint32_t *frame_count,
    qlvgm_error *error
) {
    uint32_t samples_per_frame = 44100 / rate;
    uint32_t count = duration / samples_per_frame;
    if (duration % samples_per_frame != 0) {
        count += 1;
    }
    for (uint32_t i = 0; i < write_count; i += 1) {
        uint32_t sample = song->writes[first_write + i].sample;
        if (sample < start_sample) {
            error_set(error, "internal VGM event order error");
            return false;
        }
        uint32_t index = (sample - start_sample) / samples_per_frame;
        if (index >= count) {
            count = index + 1;
        }
    }
    if (count == 0) {
        error_set(error, "VGM segment contains no playback frames");
        return false;
    }
    byte_buffer *frames = calloc(count, sizeof(*frames));
    if (frames == NULL) {
        error_set(error, "out of memory batching VGM frames");
        return false;
    }
    bool ok = true;
    if (emit_ssg_gain) {
        ok = converter_emit(
            converter,
            &frames[0],
            VGM_SSG_GAIN_HIGH_REGISTER,
            (uint8_t)(ssg_gain >> 8)
        ) && converter_emit(
            converter,
            &frames[0],
            VGM_SSG_GAIN_LOW_REGISTER,
            (uint8_t)ssg_gain
        );
    }
    for (uint32_t i = 0; ok && i < write_count; i += 1) {
        vgm_write write = song->writes[first_write + i];
        uint32_t index = (write.sample - start_sample) / samples_per_frame;
        ok = converter_write(converter, &frames[index], write.reg, write.value);
    }
    for (uint32_t i = 0; ok && i < count; i += 1) {
        if ((frames[i].size & 1) != 0 || frames[i].size / 2 > UINT16_MAX) {
            error_set(error, "too many YM2203 writes in playback frame %" PRIu32, i);
            ok = false;
            break;
        }
        ok = buffer_append_be16(output, (uint16_t)(frames[i].size / 2))
            && buffer_append(output, frames[i].data, frames[i].size);
    }
    for (uint32_t i = 0; i < count; i += 1) {
        buffer_free(&frames[i]);
    }
    free(frames);
    if (!ok && error->text[0] == '\0') {
        error_set(error, "out of memory creating playback stream");
    }
    *frame_count = count;
    return ok;
}

static bool vgm_convert(
    const vgm_song *song,
    uint32_t rate,
    bool pitch_conversion,
    converted_song *converted,
    qlvgm_error *error
) {
    memset(converted, 0, sizeof(*converted));
    converted->loop_offset = VGM_NO_LOOP;
    clock_converter converter;
    memset(&converter, 0, sizeof(converter));
    converter.source_clock = song->clock_hz;
    converter.pitch_conversion = pitch_conversion;
    bool emit_ssg_gain = song->ssg_gain != VGM_DEFAULT_SSG_GAIN;

    uint32_t intro_writes = song->write_count;
    uint32_t intro_samples = song->total_samples;
    if (song->has_loop) {
        intro_writes = song->loop_write;
        intro_samples = song->loop_sample;
    }
    uint32_t intro_frames = 0;
    if (intro_samples != 0 || intro_writes != 0) {
        if (!convert_segment(
                song,
                0,
                intro_writes,
                0,
                intro_samples,
                rate,
                &converter,
                song->ssg_gain,
                emit_ssg_gain,
                &converted->stream,
                &intro_frames,
                error
            )) {
            buffer_free(&converted->stream);
            return false;
        }
    }
    converted->frame_count = intro_frames;
    if (song->has_loop) {
        byte_buffer first_loop = { 0 };
        memset(converter.target_known, 0, sizeof converter.target_known);
        uint32_t first_loop_frames = 0;
        if (!convert_segment(
                song,
                song->loop_write,
                song->write_count - song->loop_write,
                song->loop_sample,
                song->total_samples - song->loop_sample,
                rate,
                &converter,
                song->ssg_gain,
                emit_ssg_gain && intro_frames == 0,
                &first_loop,
                &first_loop_frames,
                error
            )) {
            buffer_free(&first_loop);
            buffer_free(&converted->stream);
            return false;
        }
        clock_converter steady_converter = converter;
        memset(steady_converter.target_known, 0, sizeof steady_converter.target_known);
        byte_buffer steady_loop = { 0 };
        uint32_t steady_loop_frames = 0;
        if (!convert_segment(
                song,
                song->loop_write,
                song->write_count - song->loop_write,
                song->loop_sample,
                song->total_samples - song->loop_sample,
                rate,
                &steady_converter,
                song->ssg_gain,
                false,
                &steady_loop,
                &steady_loop_frames,
                error
            )) {
            buffer_free(&steady_loop);
            buffer_free(&first_loop);
            buffer_free(&converted->stream);
            return false;
        }
        bool loop_is_stable = first_loop.size == steady_loop.size
            && memcmp(first_loop.data, steady_loop.data, first_loop.size) == 0;
        if (loop_is_stable) {
            converted->loop_offset = converted->stream.size;
        } else {
            converted->loop_offset = converted->stream.size + first_loop.size;
        }
        bool appended = buffer_append(
            &converted->stream,
            first_loop.data,
            first_loop.size
        );
        if (appended && !loop_is_stable) {
            appended = buffer_append(
                &converted->stream,
                steady_loop.data,
                steady_loop.size
            );
        }
        if (!appended) {
            error_set(error, "out of memory creating playback loop");
            buffer_free(&steady_loop);
            buffer_free(&first_loop);
            buffer_free(&converted->stream);
            return false;
        }
        converted->loop_frame_count = steady_loop_frames;
        converted->frame_count += first_loop_frames;
        if (!loop_is_stable) {
            converted->frame_count += steady_loop_frames;
            converter.clipped_values = steady_converter.clipped_values;
        }
        buffer_free(&steady_loop);
        buffer_free(&first_loop);
    }
    if (converted->stream.failed) {
        error_set(error, "out of memory creating playback stream");
        buffer_free(&converted->stream);
        return false;
    }
    converted->clipped_values = converter.clipped_values;
    return true;
}

static void converted_song_free(converted_song *song) {
    buffer_free(&song->stream);
    memset(song, 0, sizeof(*song));
}
