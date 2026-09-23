enum {
    QLAY_SECTOR_SIZE = 686,
    QLAY_SECTOR_HEADER_OFFSET = 12,
    QLAY_SECTOR_HEADER_SIZE = 14,
    QLAY_SECTOR_HEADER_CHECKSUM_OFFSET = 26,
    QLAY_BLOCK_HEADER_OFFSET = 40,
    QLAY_BLOCK_HEADER_SIZE = 2,
    QLAY_BLOCK_HEADER_CHECKSUM_OFFSET = 42,
    QLAY_DATA_OFFSET = 52,
    QLAY_DATA_SIZE = 512,
    QLAY_DATA_CHECKSUM_OFFSET = 564,
    QLAY_GAP_OFFSET = 566,
    QLAY_CHECKSUM_SEED = 0x0F0F,
    QLAY_MAP_FILE_ID = 0xF8,
    QLAY_FREE_FILE_ID = 0xFD,
    QLAY_MIN_SECTORS = 200,
    QLAY_MAX_SECTORS = 255,
    QLAY_MEDIUM_NAME_SIZE = 10,
    QLAY_QDOS_HEADER_SIZE = 64,
    QLAY_QDOS_NAME_LENGTH_OFFSET = 14,
    QLAY_QDOS_NAME_OFFSET = 16,
    QLAY_QDOS_NAME_SIZE = 36,
    QLAY_QDOS_UPDATE_OFFSET = 52,
    QLAY_MAX_FILES = 2,
    QLAY_FILE_BUFFER_SIZE = 4096
};

// QDOS local time for 2000-01-01 00:00:00, kept stable for reproducible images.
#define QLAY_FIXED_UPDATE_TIME UINT32_C(0x495AB600)

_Static_assert(
    QLAY_SECTOR_HEADER_OFFSET + QLAY_SECTOR_HEADER_SIZE == QLAY_SECTOR_HEADER_CHECKSUM_OFFSET &&
        QLAY_BLOCK_HEADER_OFFSET + QLAY_BLOCK_HEADER_SIZE == QLAY_BLOCK_HEADER_CHECKSUM_OFFSET &&
        QLAY_DATA_OFFSET + QLAY_DATA_SIZE == QLAY_DATA_CHECKSUM_OFFSET &&
        QLAY_DATA_CHECKSUM_OFFSET + 2 == QLAY_GAP_OFFSET,
    "QLAY sector layout changed"
);

_Static_assert(
    (QLAY_MAX_FILES + 1) * QLAY_QDOS_HEADER_SIZE <= QLAY_DATA_SIZE,
    "QLAY directory must fit one block"
);

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
} qlay_file;

typedef struct {
    int fd;
    const char *path;
    char temporary_path[QLVGM_PATH_BYTES];
    uint8_t buffer[QLAY_FILE_BUFFER_SIZE];
    uint32_t buffered;
    int saved_error;
} qlay_output;

static uint16_t qlay_checksum(const uint8_t *bytes, uint32_t size) {
    uint32_t checksum = QLAY_CHECKSUM_SEED;
    for (uint32_t i = 0; i < size; i += 1) {
        checksum = (checksum + bytes[i]) & UINT16_MAX;
    }
    return (uint16_t)checksum;
}

static void qlay_write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void qlay_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void qlay_write_be32(uint8_t *bytes, uint32_t value) {
    qlay_write_be16(bytes, (uint16_t)(value >> 16));
    qlay_write_be16(bytes + 2, (uint16_t)value);
}

static void qlay_encode_sector(
    uint8_t sector[QLAY_SECTOR_SIZE],
    uint8_t logical,
    const uint8_t medium_name[QLAY_MEDIUM_NAME_SIZE],
    uint16_t random_id,
    uint8_t file_id,
    uint8_t block,
    const uint8_t payload[QLAY_DATA_SIZE]
) {
    memset(sector, 0, QLAY_SECTOR_SIZE);
    sector[10] = 0xFF;
    sector[11] = 0xFF;
    sector[QLAY_SECTOR_HEADER_OFFSET] = 0xFF;
    sector[QLAY_SECTOR_HEADER_OFFSET + 1] = logical;
    memcpy(sector + QLAY_SECTOR_HEADER_OFFSET + 2, medium_name, QLAY_MEDIUM_NAME_SIZE);
    qlay_write_be16(sector + QLAY_SECTOR_HEADER_OFFSET + 12, random_id);
    qlay_write_le16(
        sector + QLAY_SECTOR_HEADER_CHECKSUM_OFFSET,
        qlay_checksum(sector + QLAY_SECTOR_HEADER_OFFSET, QLAY_SECTOR_HEADER_SIZE)
    );

    sector[38] = 0xFF;
    sector[39] = 0xFF;
    sector[QLAY_BLOCK_HEADER_OFFSET] = file_id;
    sector[QLAY_BLOCK_HEADER_OFFSET + 1] = block;
    qlay_write_le16(
        sector + QLAY_BLOCK_HEADER_CHECKSUM_OFFSET,
        qlay_checksum(sector + QLAY_BLOCK_HEADER_OFFSET, QLAY_BLOCK_HEADER_SIZE)
    );

    sector[50] = 0xFF;
    sector[51] = 0xFF;
    memcpy(sector + QLAY_DATA_OFFSET, payload, QLAY_DATA_SIZE);
    qlay_write_le16(
        sector + QLAY_DATA_CHECKSUM_OFFSET,
        qlay_checksum(sector + QLAY_DATA_OFFSET, QLAY_DATA_SIZE)
    );
    memset(sector + QLAY_GAP_OFFSET, 0x5A, QLAY_SECTOR_SIZE - QLAY_GAP_OFFSET);
}

static void qlay_write_qdos_header(uint8_t header[QLAY_QDOS_HEADER_SIZE], const qlay_file *file) {
    memset(header, 0, QLAY_QDOS_HEADER_SIZE);
    qlay_write_be32(header, file->size + QLAY_QDOS_HEADER_SIZE);
    uint32_t name_length = (uint32_t)strlen(file->name);
    qlay_write_be16(header + QLAY_QDOS_NAME_LENGTH_OFFSET, (uint16_t)name_length);
    memcpy(header + QLAY_QDOS_NAME_OFFSET, file->name, name_length);
    qlay_write_be32(header + QLAY_QDOS_UPDATE_OFFSET, QLAY_FIXED_UPDATE_TIME);
}

static bool qlay_image_fits(
    uint32_t sectors,
    const qlay_file files[],
    uint32_t file_count,
    qlvgm_error *error
) {
    uint64_t needed = 1;
    for (uint32_t i = 0; i < file_count; i += 1) {
        uint64_t total = (uint64_t)files[i].size + QLAY_QDOS_HEADER_SIZE;
        needed += (total + QLAY_DATA_SIZE - 1) / QLAY_DATA_SIZE;
    }
    if (needed <= sectors - 1) {
        return true;
    }
    error_set(
        error,
        "image capacity exceeded: %llu data block(s) needed, %u available",
        (unsigned long long)needed,
        sectors - 1
    );
    return false;
}

static int qlay_errno_or_eio(void) {
    if (errno != 0) {
        return errno;
    }
    return EIO;
}

static bool qlay_output_begin(
    qlay_output *output,
    const char *path,
    bool force,
    qlvgm_error *error
) {
    memset(output, 0, sizeof(*output));
    output->fd = -1;
    output->path = path;

    struct stat existing;
    bool target_exists = lstat(path, &existing) == 0;
    if (!target_exists && errno != ENOENT) {
        error_set(error, "cannot stat %s: %s", path, strerror(qlay_errno_or_eio()));
        return false;
    }
    if (target_exists && !force) {
        error_set(error, "%s already exists; use --force to replace it", path);
        return false;
    }
    if (target_exists && !S_ISREG(existing.st_mode)) {
        error_set(error, "%s is not a regular file", path);
        return false;
    }
    if (strlen(path) > QLVGM_PATH_BYTES - sizeof ".tmp.XXXXXX") {
        error_set(error, "output path is too long");
        return false;
    }

    int length = snprintf(
        output->temporary_path,
        sizeof output->temporary_path,
        "%s.tmp.XXXXXX",
        path
    );
    if (length < 0 || (uint32_t)length >= sizeof output->temporary_path) {
        error_set(error, "output path is too long");
        return false;
    }
    output->fd = mkstemp(output->temporary_path);
    if (output->fd < 0) {
        error_set(error, "cannot create temporary output: %s", strerror(qlay_errno_or_eio()));
        return false;
    }

    mode_t mode;
    if (target_exists) {
        mode = existing.st_mode & 07777;
    } else {
        mode_t mask = umask(0);
        (void)umask(mask);
        mode = 0666 & ~mask;
    }
    if (fchmod(output->fd, mode) == 0) {
        return true;
    }
    int saved_error = qlay_errno_or_eio();
    (void)close(output->fd);
    output->fd = -1;
    (void)unlink(output->temporary_path);
    error_set(error, "cannot create %s: %s", path, strerror(saved_error));
    return false;
}

static bool qlay_write_bytes(qlay_output *output, const uint8_t *bytes, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        ssize_t count = write(output->fd, bytes + done, size - done);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            output->saved_error = qlay_errno_or_eio();
            return false;
        }
        done += (uint32_t)count;
    }
    return true;
}

static bool qlay_output_write(qlay_output *output, const uint8_t *bytes, uint32_t size) {
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t available = sizeof output->buffer - output->buffered;
        uint32_t count = size - offset;
        if (count > available) {
            count = available;
        }
        memcpy(output->buffer + output->buffered, bytes + offset, count);
        output->buffered += count;
        offset += count;
        if (output->buffered == sizeof output->buffer) {
            if (!qlay_write_bytes(output, output->buffer, output->buffered)) {
                return false;
            }
            output->buffered = 0;
        }
    }
    return true;
}

static bool qlay_output_finish(qlay_output *output, bool rendered, qlvgm_error *error) {
    bool ok = rendered;
    if (!ok && output->saved_error == 0) {
        output->saved_error = EIO;
    }
    if (ok && output->buffered != 0) {
        ok = qlay_write_bytes(output, output->buffer, output->buffered);
        output->buffered = 0;
    }
    if (ok && fsync(output->fd) != 0) {
        output->saved_error = qlay_errno_or_eio();
        ok = false;
    }
    if (close(output->fd) != 0 && ok) {
        output->saved_error = qlay_errno_or_eio();
        ok = false;
    }
    output->fd = -1;
    if (ok && rename(output->temporary_path, output->path) != 0) {
        output->saved_error = qlay_errno_or_eio();
        ok = false;
    }
    if (ok) {
        return true;
    }
    (void)unlink(output->temporary_path);
    error_set(error, "cannot write %s: %s", output->path, strerror(output->saved_error));
    return false;
}

static bool qlay_render_image(
    qlay_output *output,
    uint32_t sectors,
    const uint8_t medium_name[QLAY_MEDIUM_NAME_SIZE],
    uint16_t random_id,
    const qlay_file files[],
    uint32_t file_count
) {
    uint8_t directory[QLAY_DATA_SIZE] = { 0 };
    uint8_t map[QLAY_DATA_SIZE] = { 0 };
    qlay_write_be32(directory, (file_count + 1) * QLAY_QDOS_HEADER_SIZE);
    map[0] = QLAY_MAP_FILE_ID;
    for (uint32_t logical = 1; logical < sectors; logical += 1) {
        map[logical * 2] = QLAY_FREE_FILE_ID;
    }

    uint32_t next = 1;
    map[next * 2] = 0;
    next += 1;
    for (uint32_t i = 0; i < file_count; i += 1) {
        uint8_t *header = directory + (i + 1) * QLAY_QDOS_HEADER_SIZE;
        qlay_write_qdos_header(header, &files[i]);
        uint32_t total = files[i].size + QLAY_QDOS_HEADER_SIZE;
        for (uint32_t block = 0; block * QLAY_DATA_SIZE < total; block += 1) {
            map[next * 2] = (uint8_t)(i + 1);
            map[next * 2 + 1] = (uint8_t)block;
            next += 1;
        }
    }
    map[QLAY_DATA_SIZE - 1] = (uint8_t)(next - 1);

    for (uint32_t physical = 0; physical < sectors; physical += 1) {
        uint32_t logical = 0;
        if (physical != 0) {
            logical = sectors - physical;
        }
        uint8_t file_id = map[logical * 2];
        uint8_t block = map[logical * 2 + 1];
        uint8_t payload[QLAY_DATA_SIZE] = { 0 };
        if (logical == 0) {
            memcpy(payload, map, sizeof payload);
        } else if (file_id == 0) {
            memcpy(payload, directory, sizeof payload);
        } else if (file_id != QLAY_FREE_FILE_ID) {
            const qlay_file *file = &files[file_id - 1];
            const uint8_t *header = directory + (uint32_t)file_id * QLAY_QDOS_HEADER_SIZE;
            uint32_t offset = (uint32_t)block * QLAY_DATA_SIZE;
            uint32_t total = file->size + QLAY_QDOS_HEADER_SIZE;
            for (uint32_t i = 0; i < QLAY_DATA_SIZE && offset + i < total; i += 1) {
                uint32_t position = offset + i;
                if (position < QLAY_QDOS_HEADER_SIZE) {
                    payload[i] = header[position];
                } else {
                    payload[i] = file->data[position - QLAY_QDOS_HEADER_SIZE];
                }
            }
        }
        uint8_t sector[QLAY_SECTOR_SIZE];
        qlay_encode_sector(
            sector,
            (uint8_t)logical,
            medium_name,
            random_id,
            file_id,
            block,
            payload
        );
        if (!qlay_output_write(output, sector, sizeof sector)) {
            return false;
        }
    }
    return true;
}

static bool qlay_write_image(
    const char *path,
    bool force,
    uint32_t sectors,
    const uint8_t medium_name[QLAY_MEDIUM_NAME_SIZE],
    uint16_t random_id,
    const qlay_file files[],
    uint32_t file_count,
    qlvgm_error *error
) {
    if (sectors < QLAY_MIN_SECTORS || sectors > QLAY_MAX_SECTORS) {
        error_set(error, "new images must contain 200-255 sectors");
        return false;
    }
    if (file_count == 0 || file_count > QLAY_MAX_FILES) {
        error_set(error, "an image must contain 1-%u files", QLAY_MAX_FILES);
        return false;
    }
    for (uint32_t i = 0; i < file_count; i += 1) {
        uint32_t length = (uint32_t)strlen(files[i].name);
        if (length == 0 || length > QLAY_QDOS_NAME_SIZE) {
            error_set(error, "QDOS filenames must contain 1-36 bytes");
            return false;
        }
    }
    if (!qlay_image_fits(sectors, files, file_count, error)) {
        return false;
    }

    qlay_output output;
    if (!qlay_output_begin(&output, path, force, error)) {
        return false;
    }
    bool rendered = qlay_render_image(
        &output,
        sectors,
        medium_name,
        random_id,
        files,
        file_count
    );
    return qlay_output_finish(&output, rendered, error);
}

static bool qlay_ascii_equal_ignore_case(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        uint8_t a = (uint8_t)*left;
        uint8_t b = (uint8_t)*right;
        if (a >= 'A' && a <= 'Z') {
            a += 'a' - 'A';
        }
        if (b >= 'A' && b <= 'Z') {
            b += 'a' - 'A';
        }
        if (a != b) {
            return false;
        }
        left += 1;
        right += 1;
    }
    return *left == *right;
}

static void qlay_medium_name(
    const char *path,
    const char *override,
    uint8_t medium_name[QLAY_MEDIUM_NAME_SIZE]
) {
    memset(medium_name, ' ', QLAY_MEDIUM_NAME_SIZE);
    const char *name = override;
    if (name == NULL) {
        name = strrchr(path, '/');
        if (name == NULL) {
            name = path;
        } else {
            name += 1;
        }
    }
    uint32_t length = (uint32_t)strlen(name);
    if (override == NULL && length >= 4 && qlay_ascii_equal_ignore_case(name + length - 4, ".mdv")) {
        length -= 4;
    }
    if (length > QLAY_MEDIUM_NAME_SIZE) {
        length = QLAY_MEDIUM_NAME_SIZE;
    }
    memcpy(medium_name, name, length);
}

static uint64_t qlay_random_mix(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

static uint16_t qlay_random_id(void) {
    uint16_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        uint32_t done = 0;
        while (done < sizeof value) {
            ssize_t count = read(fd, (uint8_t *)&value + done, sizeof value - done);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            done += (uint32_t)count;
        }
        (void)close(fd);
        if (done == sizeof value) {
            return value;
        }
    }
    uint64_t seed = (uint64_t)time(NULL);
    seed ^= (uint64_t)(uintmax_t)getpid() << 32;
    seed ^= (uintptr_t)&value;
    uint64_t mixed = qlay_random_mix(seed);
    return (uint16_t)(mixed ^ (mixed >> 16) ^ (mixed >> 32) ^ (mixed >> 48));
}
