static void print_usage(FILE *file) {
    fputs(
        "usage: qlvgm [OPTIONS] INPUT.vgm OUTPUT.mdv\n"
        "\n"
        "Create a bootable Sinclair QL QSound2 Microdrive image.\n"
        "\n"
        "options:\n"
        "  --rate 50|60        target PAL or NTSC frame rate (default: 50)\n"
        "  --no-pitch-conversion  preserve source YM2203 writes exactly\n"
        "  --screen FILE       include a 32 KiB QL screen dump\n"
        "  --screen-mode 4|8   screen display mode (default: 8)\n"
        "  --sectors N         cartridge geometry, 200-255 (default: 255)\n"
        "  --medium-name NAME  cartridge name, 1-10 bytes\n"
        "  --random-id N       cartridge random ID, 0-65535\n"
        "  --force             replace an existing output image\n"
        "  --verbose           show conversion and memory statistics\n"
        "  --help              show this help\n"
        "  --version           show version\n"
        "\n"
        "QLASM and QLVGM_PLAYER override the sibling assembler and bundled\n"
        "target source used by the conversion.\n",
        file
    );
}

static const char *option_value(
    int argc,
    char **argv,
    int *index,
    const char *argument,
    const char *name,
    qlvgm_error *error
) {
    uint32_t name_length = (uint32_t)strlen(name);
    if (strncmp(argument, name, name_length) == 0 && argument[name_length] == '=') {
        if (argument[name_length + 1] == '\0') {
            error_set(error, "%s requires a value", name);
            return NULL;
        }
        return argument + name_length + 1;
    }
    if (strcmp(argument, name) != 0) {
        return NULL;
    }
    if (*index + 1 >= argc) {
        error_set(error, "%s requires a value", name);
        return NULL;
    }
    *index += 1;
    return argv[*index];
}

static bool parse_u32(
    const char *text,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t *value,
    const char *name,
    qlvgm_error *error
) {
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        error_set(error, "invalid %s: %s", name, text);
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static int cli_main(int argc, char **argv) {
    image_options options;
    memset(&options, 0, sizeof(options));
    options.rate = 50;
    options.sectors = 255;
    options.screen_mode = 8;
    options.pitch_conversion = true;
#ifdef QLVGM_TARGET_TEST_FRAMES
    options.test_frames = QLVGM_TARGET_TEST_FRAMES;
#endif
    options.qlasm = getenv("QLASM");
    options.player_source = getenv("QLVGM_PLAYER");
    qlvgm_error error;
    memset(&error, 0, sizeof(error));
    const char *operands[2];
    uint32_t operand_count = 0;
    bool options_done = false;
    for (int i = 1; i < argc; i += 1) {
        const char *argument = argv[i];
        if (!options_done && strcmp(argument, "--") == 0) {
            options_done = true;
            continue;
        }
        if (!options_done && strcmp(argument, "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (!options_done && strcmp(argument, "--version") == 0) {
            puts("qlvgm 0.1");
            return 0;
        }
        if (!options_done && strcmp(argument, "--force") == 0) {
            options.force = true;
            continue;
        }
        if (!options_done && strcmp(argument, "--verbose") == 0) {
            options.verbose = true;
            continue;
        }
        if (!options_done && strcmp(argument, "--no-pitch-conversion") == 0) {
            options.pitch_conversion = false;
            continue;
        }
        const char *value = NULL;
        if (!options_done
            && (strcmp(argument, "--screen") == 0 || strncmp(argument, "--screen=", 9) == 0)) {
            value = option_value(argc, argv, &i, argument, "--screen", &error);
            if (value == NULL) {
                break;
            }
            options.screen_path = value;
            continue;
        }
        if (!options_done
            && (strcmp(argument, "--screen-mode") == 0
                || strncmp(argument, "--screen-mode=", 14) == 0)) {
            value = option_value(argc, argv, &i, argument, "--screen-mode", &error);
            if (value == NULL
                || !parse_u32(value, 4, 8, &options.screen_mode, "screen mode", &error)
                || (options.screen_mode != 4 && options.screen_mode != 8)) {
                if (error.text[0] == '\0') {
                    error_set(&error, "screen mode must be 4 or 8");
                }
                break;
            }
            options.screen_mode_set = true;
            continue;
        }
        if (!options_done
            && (strcmp(argument, "--rate") == 0 || strncmp(argument, "--rate=", 7) == 0)) {
            value = option_value(argc, argv, &i, argument, "--rate", &error);
            if (value == NULL
                || !parse_u32(value, 50, 60, &options.rate, "rate", &error)
                || (options.rate != 50 && options.rate != 60)) {
                if (error.text[0] == '\0') {
                    error_set(&error, "rate must be 50 or 60");
                }
                break;
            }
            continue;
        }
        if (!options_done
            && (strcmp(argument, "--sectors") == 0 || strncmp(argument, "--sectors=", 10) == 0)) {
            value = option_value(argc, argv, &i, argument, "--sectors", &error);
            if (value == NULL
                || !parse_u32(value, 200, 255, &options.sectors, "sector count", &error)) {
                break;
            }
            continue;
        }
        if (!options_done
            && (strcmp(argument, "--medium-name") == 0
                || strncmp(argument, "--medium-name=", 14) == 0)) {
            value = option_value(argc, argv, &i, argument, "--medium-name", &error);
            if (value == NULL) {
                break;
            }
            uint32_t length = (uint32_t)strlen(value);
            if (length == 0 || length > 10) {
                error_set(&error, "medium name must contain 1-10 bytes");
                break;
            }
            options.medium_name = value;
            continue;
        }
        if (!options_done
            && (strcmp(argument, "--random-id") == 0
                || strncmp(argument, "--random-id=", 12) == 0)) {
            value = option_value(argc, argv, &i, argument, "--random-id", &error);
            if (value == NULL
                || !parse_u32(value, 0, UINT16_MAX, &options.random_id, "random ID", &error)) {
                break;
            }
            options.random_id_set = true;
            continue;
        }
        if (!options_done && argument[0] == '-') {
            error_set(&error, "unknown option: %s", argument);
            break;
        }
        if (operand_count == 2) {
            error_set(&error, "too many operands");
            break;
        }
        operands[operand_count] = argument;
        operand_count += 1;
    }
    if (error.text[0] == '\0' && options.screen_mode_set && options.screen_path == NULL) {
        error_set(&error, "--screen-mode requires --screen");
    }
    if (error.text[0] == '\0' && operand_count != 2) {
        error_set(&error, "INPUT.vgm and OUTPUT.mdv are required");
    }
    if (error.text[0] != '\0') {
        fprintf(stderr, "qlvgm: %s\n", error.text);
        print_usage(stderr);
        return 2;
    }
    options.input_path = operands[0];
    options.output_path = operands[1];

    char qlasm_path[QLVGM_PATH_BYTES];
    if (options.qlasm == NULL || options.qlasm[0] == '\0') {
        if (!executable_relative_path("../qlasm/qlasm", qlasm_path, &error)) {
            fprintf(stderr, "qlvgm: %s\n", error.text);
            return 1;
        }
        options.qlasm = qlasm_path;
    }
    char player_path[QLVGM_PATH_BYTES];
    if (options.player_source == NULL || options.player_source[0] == '\0') {
        if (!executable_relative_path("player.asm", player_path, &error)) {
            fprintf(stderr, "qlvgm: %s\n", error.text);
            return 1;
        }
        options.player_source = player_path;
    }

    file_data screen = { 0 };
    const file_data *screen_file = NULL;
    if (options.screen_path != NULL) {
        if (!read_file(options.screen_path, &screen, &error)) {
            fprintf(stderr, "qlvgm: %s\n", error.text);
            return 1;
        }
        if (screen.size != QLVGM_SCREEN_SIZE) {
            fprintf(
                stderr,
                "qlvgm: screen dump must contain exactly %u bytes: %s contains %" PRIu32 "\n",
                QLVGM_SCREEN_SIZE,
                options.screen_path,
                screen.size
            );
            free(screen.data);
            return 1;
        }
        screen_file = &screen;
    }

    file_data input;
    if (!read_file(options.input_path, &input, &error)) {
        free(screen.data);
        fprintf(stderr, "qlvgm: %s\n", error.text);
        return 1;
    }
    vgm_song song;
    bool parsed = vgm_parse(input.data, input.size, &song, &error);
    free(input.data);
    if (!parsed) {
        vgm_song_free(&song);
        free(screen.data);
        fprintf(stderr, "qlvgm: %s\n", error.text);
        return 1;
    }

    converted_song converted;
    if (!vgm_convert(
            &song,
            options.rate,
            options.pitch_conversion,
            &converted,
            &error
        )) {
        vgm_song_free(&song);
        free(screen.data);
        fprintf(stderr, "qlvgm: %s\n", error.text);
        return 1;
    }
    qlz_stream compressed;
    if (!qlz_stream_create(&converted.stream, &compressed, &error)) {
        converted_song_free(&converted);
        vgm_song_free(&song);
        free(screen.data);
        fprintf(stderr, "qlvgm: %s\n", error.text);
        return 1;
    }

    uint32_t loaded_size = 0;
    bool created = create_image(
        &converted,
        &compressed,
        screen_file,
        &options,
        &loaded_size,
        &error
    );
    if (created) {
        printf("created %s", options.output_path);
        if (options.verbose) {
            uint64_t target_memory = (uint64_t)loaded_size + compressed.raw_size;
            printf(
                ": %" PRIu32 " frames at %" PRIu32 " Hz, "
                "%" PRIu32 " decoded -> %" PRIu32 " QLZ bytes, "
                "%" PRIu32 " loaded bytes",
                converted.frame_count,
                options.rate,
                compressed.raw_size,
                compressed.data.size,
                loaded_size
            );
            if (screen_file != NULL) {
                printf(", %u screen bytes", QLVGM_SCREEN_SIZE);
            }
            printf(", %" PRIu64 " target bytes", target_memory);
        }
        fputc('\n', stdout);
        if (converted.clipped_values != 0) {
            fprintf(
                stderr,
                "qlvgm: warning: %" PRIu32 " frequency value(s) were clipped for QSound2\n",
                converted.clipped_values
            );
        }
    }
    qlz_stream_free(&compressed);
    converted_song_free(&converted);
    vgm_song_free(&song);
    free(screen.data);
    if (!created) {
        fprintf(stderr, "qlvgm: %s\n", error.text);
        return 1;
    }
    return 0;
}
