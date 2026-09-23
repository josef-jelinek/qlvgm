#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    QLVGM_MAX_INPUT_BYTES = 16 * 1024 * 1024,
    QLVGM_PATH_BYTES = 4096
};

typedef struct {
    char text[512];
} qlvgm_error;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    bool failed;
} byte_buffer;

typedef struct {
    uint8_t *data;
    uint32_t size;
} file_data;

static void error_set(qlvgm_error *error, const char *format, ...) {
    if (error == NULL || error->text[0] != '\0') {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error->text, sizeof error->text, format, arguments);
    va_end(arguments);
}

static bool buffer_reserve(byte_buffer *buffer, uint32_t extra) {
    if (buffer->failed || extra > UINT32_MAX - buffer->size) {
        buffer->failed = true;
        return false;
    }
    uint32_t needed = buffer->size + extra;
    if (needed <= buffer->capacity) {
        return true;
    }
    uint32_t capacity = buffer->capacity;
    if (capacity == 0) {
        capacity = 256;
    }
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    uint8_t *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failed = true;
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_append(byte_buffer *buffer, const void *data, uint32_t size) {
    if (size == 0) {
        return true;
    }
    if (!buffer_reserve(buffer, size)) {
        return false;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return true;
}

static bool buffer_append_byte(byte_buffer *buffer, uint8_t value) {
    return buffer_append(buffer, &value, 1);
}

static bool buffer_append_be16(byte_buffer *buffer, uint16_t value) {
    uint8_t bytes[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    return buffer_append(buffer, bytes, sizeof bytes);
}

static void buffer_free(byte_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0]
        | (uint32_t)data[1] << 8
        | (uint32_t)data[2] << 16
        | (uint32_t)data[3] << 24;
}

#ifndef QLVGM_TEST
static bool read_file(const char *path, file_data *result, qlvgm_error *error) {
    memset(result, 0, sizeof(*result));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error_set(error, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    struct stat status;
    if (fstat(fd, &status) != 0) {
        error_set(error, "cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || status.st_size > QLVGM_MAX_INPUT_BYTES) {
        error_set(error, "%s is not a regular file of at most %u bytes", path, QLVGM_MAX_INPUT_BYTES);
        close(fd);
        return false;
    }
    uint32_t size = (uint32_t)status.st_size;
    uint8_t *data = malloc(size + 1);
    if (data == NULL) {
        error_set(error, "out of memory reading %s", path);
        close(fd);
        return false;
    }
    uint32_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const char *detail = "unexpected EOF";
            if (count < 0) {
                detail = strerror(errno);
            }
            error_set(error, "cannot read %s: %s", path, detail);
            free(data);
            close(fd);
            return false;
        }
        offset += (uint32_t)count;
    }
    if (close(fd) != 0) {
        error_set(error, "cannot close %s: %s", path, strerror(errno));
        free(data);
        return false;
    }
    data[size] = 0;
    result->data = data;
    result->size = size;
    return true;
}

static int run_process(char *const arguments[], qlvgm_error *error) {
    pid_t child = fork();
    if (child < 0) {
        error_set(error, "cannot start %s: %s", arguments[0], strerror(errno));
        return -1;
    }
    if (child == 0) {
        execvp(arguments[0], arguments);
        fprintf(stderr, "qlvgm: cannot execute %s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        error_set(error, "cannot wait for %s: %s", arguments[0], strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) {
            error_set(error, "%s exited with status %d", arguments[0], WEXITSTATUS(status));
        } else {
            error_set(error, "%s terminated abnormally", arguments[0]);
        }
        return -1;
    }
    return 0;
}

static bool executable_relative_path(
    const char *relative,
    char path[QLVGM_PATH_BYTES],
    qlvgm_error *error
) {
    ssize_t length = readlink("/proc/self/exe", path, QLVGM_PATH_BYTES - 1);
    if (length < 0 || length >= QLVGM_PATH_BYTES - 1) {
        error_set(error, "cannot locate qlvgm executable: %s", strerror(errno));
        return false;
    }
    path[length] = '\0';
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        error_set(error, "cannot resolve qlvgm executable directory");
        return false;
    }
    slash[1] = '\0';
    uint32_t prefix = (uint32_t)(slash + 1 - path);
    uint32_t relative_length = (uint32_t)strlen(relative);
    if (prefix + relative_length >= QLVGM_PATH_BYTES) {
        error_set(error, "companion path is too long");
        return false;
    }
    memcpy(path + prefix, relative, relative_length + 1);
    return true;
}
#endif
