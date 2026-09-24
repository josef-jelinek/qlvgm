typedef struct {
    const char *input_path;
    const char *output_path;
    const char *screen_path;
    const char *medium_name;
    const char *qlasm;
    const char *player_source;
    uint32_t rate;
    uint32_t sectors;
    uint32_t random_id;
    uint32_t screen_mode;
    uint32_t test_frames;
    bool pitch_conversion;
    bool random_id_set;
    bool screen_mode_set;
    bool force;
    bool verbose;
} image_options;

enum {
    QLVGM_BOOT_BYTES = 192,
    QLVGM_SCREEN_ADDRESS = 0x20000,
    QLVGM_SCREEN_SIZE = 32768
};

typedef struct {
    char directory[QLVGM_PATH_BYTES];
    char source[QLVGM_PATH_BYTES];
    char binary[QLVGM_PATH_BYTES];
} temporary_paths;

static void temporary_paths_clean(temporary_paths *paths) {
    if (paths->source[0] != '\0') {
        unlink(paths->source);
    }
    if (paths->binary[0] != '\0') {
        unlink(paths->binary);
    }
    if (paths->directory[0] != '\0') {
        rmdir(paths->directory);
    }
}

static bool temporary_paths_create(temporary_paths *paths, qlvgm_error *error) {
    memset(paths, 0, sizeof(*paths));
    memcpy(paths->directory, "/tmp/qlvgm.XXXXXX", sizeof "/tmp/qlvgm.XXXXXX");
    if (mkdtemp(paths->directory) == NULL) {
        error_set(error, "cannot create temporary directory: %s", strerror(errno));
        return false;
    }
    int values[] = {
        snprintf(paths->source, sizeof paths->source, "%s/player.asm", paths->directory),
        snprintf(paths->binary, sizeof paths->binary, "%s/qlvgm", paths->directory)
    };
    for (uint32_t i = 0; i < sizeof values / sizeof values[0]; i += 1) {
        if (values[i] < 0 || values[i] >= QLVGM_PATH_BYTES) {
            error_set(error, "temporary path is too long");
            temporary_paths_clean(paths);
            return false;
        }
    }
    return true;
}

static bool write_player_source(
    const char *path,
    const char *player_path,
    const converted_song *song,
    const qlz_stream *compressed,
    const image_options *options,
    qlvgm_error *error
) {
    file_data player;
    if (!read_file(player_path, &player, error)) {
        return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error_set(error, "cannot create %s: %s", path, strerror(errno));
        free(player.data);
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        error_set(error, "cannot open stream for %s: %s", path, strerror(errno));
        close(fd);
        free(player.data);
        return false;
    }
    bool ok = fprintf(
        file,
        "TARGET_RATE EQU %" PRIu32 "\n"
        "DECODED_SIZE EQU %" PRIu32 "\n"
        "LOOP_OFFSET EQU $%08" PRIX32 "\n"
        "TEST_FRAMES EQU %" PRIu32 "\n",
        options->rate,
        compressed->raw_size,
        song->loop_offset,
        options->test_frames
    ) > 0;
    if (ok) {
        ok = fwrite(player.data, 1, player.size, file) == player.size;
    }
    if (ok && (player.size == 0 || player.data[player.size - 1] != '\n')) {
        ok = fputc('\n', file) != EOF;
    }
    if (ok) {
        ok = fputs("qlz_data:\n", file) >= 0;
    }
    for (uint32_t i = 0; ok && i < compressed->data.size; i += 16) {
        uint32_t end = i + 16;
        if (end > compressed->data.size) {
            end = compressed->data.size;
        }
        ok = fputs("        DC.B    ", file) >= 0;
        for (uint32_t j = i; ok && j < end; j += 1) {
            if (j != i) {
                ok = fputc(',', file) != EOF;
            }
            if (ok) {
                ok = fprintf(file, "$%02X", compressed->data.data[j]) > 0;
            }
        }
        if (ok) {
            ok = fputc('\n', file) != EOF;
        }
    }
    if (ok) {
        ok = fputs("loaded_end:\n        END\n", file) >= 0;
    }
    free(player.data);
    if (!ok || fflush(file) != 0) {
        error_set(error, "cannot write generated player source %s", path);
        fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        error_set(error, "cannot close generated player source %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool make_boot(
    char boot[QLVGM_BOOT_BYTES],
    uint32_t *boot_size,
    uint32_t loaded_size,
    uint32_t decoded_size,
    bool has_screen,
    uint32_t screen_mode,
    qlvgm_error *error
) {
    if (loaded_size > UINT32_MAX - decoded_size) {
        error_set(error, "player memory requirement exceeds 32-bit QDOS range");
        return false;
    }
    int length;
    if (has_screen) {
        length = snprintf(
            boot,
            QLVGM_BOOT_BYTES,
            "100 MODE %u\n"
            "110 LBYTES \"mdv1_screen\",%u\n"
            "120 a=RESPR(%" PRIu32 ")\n"
            "130 LBYTES \"mdv1_qlvgm\",a\n"
            "140 CALL a\n",
            screen_mode,
            QLVGM_SCREEN_ADDRESS,
            loaded_size + decoded_size
        );
    } else {
        length = snprintf(
            boot,
            QLVGM_BOOT_BYTES,
            "100 a=RESPR(%" PRIu32 ")\n"
            "110 LBYTES \"mdv1_qlvgm\",a\n"
            "120 CALL a\n",
            loaded_size + decoded_size
        );
    }
    if (length < 0 || length >= QLVGM_BOOT_BYTES) {
        error_set(error, "generated BOOT program is too long");
        return false;
    }
    *boot_size = (uint32_t)length;
    return true;
}

static bool create_image(
    const converted_song *song,
    const qlz_stream *compressed,
    const file_data *screen,
    const image_options *options,
    uint32_t *loaded_size,
    qlvgm_error *error
) {
    temporary_paths paths;
    if (!temporary_paths_create(&paths, error)) {
        return false;
    }
    bool ok = write_player_source(
        paths.source,
        options->player_source,
        song,
        compressed,
        options,
        error
    );
    if (ok) {
        char *arguments[5];
        uint32_t count = 0;
        arguments[count] = (char *)options->qlasm;
        count += 1;
        if (options->verbose) {
            arguments[count] = "--verbose";
            count += 1;
        }
        arguments[count] = paths.source;
        count += 1;
        arguments[count] = paths.binary;
        count += 1;
        arguments[count] = NULL;
        ok = run_process(arguments, error) == 0;
    }

    file_data binary = { 0 };
    if (ok) {
        ok = read_file(paths.binary, &binary, error);
    }
    if (ok && binary.size == 0) {
        error_set(error, "assembled player has an invalid size");
        ok = false;
    }

    char boot[QLVGM_BOOT_BYTES];
    uint32_t boot_size = 0;
    if (ok) {
        *loaded_size = binary.size;
        ok = make_boot(
            boot,
            &boot_size,
            *loaded_size,
            compressed->raw_size,
            screen != NULL,
            options->screen_mode,
            error
        );
    }
    if (ok) {
        uint8_t medium_name[QLAY_MEDIUM_NAME_SIZE];
        qlay_medium_name(options->output_path, options->medium_name, medium_name);
        uint16_t random_id = (uint16_t)options->random_id;
        if (!options->random_id_set) {
            random_id = qlay_random_id();
        }
        qlay_file files[QLAY_MAX_FILES] = {
            { .name = "BOOT", .data = (uint8_t *)boot, .size = boot_size },
            { .name = "qlvgm", .data = binary.data, .size = binary.size }
        };
        uint32_t file_count = 2;
        if (screen != NULL) {
            files[file_count] = (qlay_file){
                .name = "screen",
                .data = screen->data,
                .size = screen->size
            };
            file_count += 1;
        }
        ok = qlay_write_image(
            options->output_path,
            options->force,
            options->sectors,
            medium_name,
            random_id,
            files,
            file_count,
            error
        );
    }
    free(binary.data);
    temporary_paths_clean(&paths);
    return ok;
}
