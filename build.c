#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int build_and_run_tests(char *compiler) {
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        execlp(
            compiler,
            compiler,
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-O0",
            "-g",
            "test.c",
            "-o",
            "test",
            (char *)NULL
        );
        perror(compiler);
        _exit(127);
    }

    int status;
    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status)) {
        return 1;
    }
    if (WEXITSTATUS(status) != 0) {
        return WEXITSTATUS(status);
    }

    execl("./test", "./test", (char *)NULL);
    perror("./test");
    return 1;
}

int main(int argc, char **argv) {
    bool release = false;
    bool run_tests = false;
    if (argc == 2 && strcmp(argv[1], "release") == 0) {
        release = true;
    } else if (argc == 2 && strcmp(argv[1], "debug") == 0) {
        release = false;
    } else if (argc == 2 && strcmp(argv[1], "test") == 0) {
        run_tests = true;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [debug|release|test]\n", argv[0]);
        return 2;
    }

    char *compiler = getenv("CC");
    if (compiler == NULL || compiler[0] == '\0') {
        compiler = "cc";
    }
    if (run_tests) {
        return build_and_run_tests(compiler);
    }
    if (release) {
        execlp(
            compiler,
            compiler,
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Os",
            "-s",
            "qlvgm.c",
            "-o",
            "qlvgm",
            (char *)NULL
        );
    } else {
        execlp(
            compiler,
            compiler,
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-O0",
            "-g",
            "qlvgm.c",
            "-o",
            "qlvgm",
            (char *)NULL
        );
    }
    perror(compiler);
    return 1;
}
