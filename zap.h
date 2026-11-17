#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#define ZAP_ERR "\033[31m[ERROR]\033[0m"
#define ZAP_INFO "\033[34m[INFO]\033[0m"
#define ZAP_CMD "\033[36m[CMD]\033[0m"
#define ZAP_FMT "\033[33m[ZAP]\033[0m"

#define ZAP_REBUILD(argc, argv) \
    do { \
        if (!zap_rebuild(__FILE__, argc, argv)) { \
            fprintf(stderr, "%s Rebuild failed.\n", ZAP_ERR); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} Cmd;

void cmd_append(Cmd *cmd, const char *item);
bool cmd_run(Cmd *cmd);
void cmd_dump(Cmd *cmd);

void zap_log(const char *fmt, const char *msg);
bool zap_rebuild(const char *source, int argc, char **argv);

void cmd_cc(Cmd *cmd);
void cmd_cxx(Cmd *cmd);

void cmd_cflags(Cmd *cmd, char *flag, ...);
void cmd_input_files(Cmd *cmd, char *file, ...);
void cmd_output_file(Cmd *cmd, char *file);

#ifdef ZAP_IMPLEMENTATION 


void cmd_append(Cmd *cmd, const char *item) {
    if (!cmd || !item) return;

    if (cmd->count + 2 > cmd->capacity) {
        size_t new_capacity = cmd->capacity ? cmd->capacity * 2 : 8;

        while (new_capacity < cmd->count + 2) {
            new_capacity *= 2;
        }

        char **new_items = realloc(
            cmd->items,
            new_capacity * sizeof(*cmd->items)
        );

        if (!new_items) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }

        cmd->items = new_items;
        cmd->capacity = new_capacity;
    }

    cmd->items[cmd->count++] = (char *)item;
    cmd->items[cmd->count] = NULL;
}

bool cmd_run(Cmd *cmd) {
    if (!cmd || !cmd->items || cmd->count == 0) {
        return false;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return false;
    }

    if (pid == 0) {
        execvp(cmd->items[0], cmd->items);

        perror("execvp");
        _exit(127);
    }

    int status;

    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void cmd_dump(Cmd *cmd) {
    if (!cmd) return;

    printf("%s ", ZAP_CMD);

    for (size_t i = 0; i < cmd->count; ++i) {
        printf("%s ", cmd->items[i]);
    }

    printf("\n");
}

void zap_log(const char *fmt, const char *msg) {
    printf("%s %s\n", fmt, msg);
}

static char *zap_strdup(const char *str) {
    size_t len = strlen(str) + 1;
    char *copy = malloc(len);

    if (!copy) return NULL;

    memcpy(copy, str, len);
    return copy;
}

/*
 * Resolve argv[0] to an executable path.
 *
 * If argv[0] contains '/', use it directly.
 * Otherwise, search PATH.
 */
static char *zap_find_executable(const char *argv0) {
    if (strchr(argv0, '/')) {
        return zap_strdup(argv0);
    }

    const char *path = getenv("PATH");

    if (!path) {
        return zap_strdup(argv0);
    }

    const char *begin = path;
    size_t name_len = strlen(argv0);

    for (;;) {
        const char *end = strchr(begin, ':');
        size_t dir_len = end ? (size_t)(end - begin) : strlen(begin);

        // An empty PATH component means the current directory.
        size_t actual_dir_len = dir_len ? dir_len : 1;

        char *candidate = malloc(actual_dir_len + 1 + name_len + 1);

        if (!candidate) return NULL;

        if (dir_len) {
            memcpy(candidate, begin, dir_len);
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, argv0, name_len + 1);
        } else {
            candidate[0] = '.';
            candidate[1] = '/';
            memcpy(candidate + 2, argv0, name_len + 1);
        }

        if (access(candidate, X_OK) == 0) {
            return candidate;
        }

        free(candidate);

        if (!end) break;

        begin = end + 1;
    }

    return zap_strdup(argv0);
}

bool zap_rebuild(const char *source, int argc, char **argv) {
    if (!source || argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "%s Invalid arguments to zap_rebuild.\n", ZAP_ERR);
        return false;
    }

    char *executable = zap_find_executable(argv[0]);

    if (!executable) {
        perror("malloc");
        return false;
    }

    struct stat source_stat;
    struct stat executable_stat;

    if (stat(source, &source_stat) == -1) {
        perror(source);
        free(executable);
        return false;
    }

    if (stat(executable, &executable_stat) == -1) {
        perror(executable);
        free(executable);
        return false;
    }

    // The executable is already newer than the source.
    if (source_stat.st_mtime <= executable_stat.st_mtime) {
        free(executable);
        return true;
    }

    zap_log(ZAP_INFO, "Change detected. Rebuilding file.");

    char suffix[64];
    snprintf(suffix, sizeof(suffix), ".zap-tmp-%ld", (long)getpid());

    size_t temp_len = strlen(executable) + strlen(suffix) + 1;
    char *temp_output = malloc(temp_len);

    if (!temp_output) {
        perror("malloc");
        free(executable);
        return false;
    }

    snprintf(temp_output, temp_len, "%s%s", executable, suffix);

#ifdef __cplusplus
    const char *compiler = getenv("CXX");
    if (!compiler || !*compiler) compiler = "clang++";
#else
    const char *compiler = getenv("CC");
    if (!compiler || !*compiler) compiler = "clang";
#endif

    Cmd cmd = {0};

    cmd_append(&cmd, compiler);
    cmd_append(&cmd, source);
    cmd_append(&cmd, "-o");
    cmd_append(&cmd, temp_output);

    cmd_dump(&cmd);

    if (!cmd_run(&cmd)) {
        zap_log(ZAP_ERR, "Compiler returned an error.");

        remove(temp_output);
        free(temp_output);
        free(executable);
        free(cmd.items);

        return false;
    }

    free(cmd.items);

    if (rename(temp_output, executable) == -1) {
        perror("rename");

        remove(temp_output);
        free(temp_output);
        free(executable);

        return false;
    }

    free(temp_output);

    zap_log(ZAP_INFO, "Rebuild successful. Restarting...");

    execv(executable, argv);

    perror("execv");

    free(executable);
    return false;
}

void cmd_cc(Cmd *cmd) {
    cmd_append(cmd, "clang");
}

void cmd_cxx(Cmd *cmd) {
    cmd_append(cmd, "clang++");
}

void cmd_cflags(Cmd *cmd, char *flag, ...) {
    va_list args;
    va_start(args, flag);

    char *current = flag;

    while (current != NULL) {
        cmd_append(cmd, current);
        current = va_arg(args, char *);
    }

    va_end(args);
}

void cmd_input_files(Cmd *cmd, char *file, ...) {
    va_list args;
    va_start(args, file);

    char *current = file;

    while (current != NULL) {
        cmd_append(cmd, current);
        current = va_arg(args, char *);
    }

    va_end(args);
}

void cmd_output_file(Cmd *cmd, char *file) {
    cmd_append(cmd, "-o");
    cmd_append(cmd, file);
}

#endif /* ZAP_IMPLEMENTATION */