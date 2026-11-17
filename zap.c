#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#define ZAP_IMPLEMENTATION

/*
 * Serenity build configuration, implemented with the Zap C API.
 *
 * Expected checkout layout:
 *   workspace/
 *     serenity/
 *       zap.c
 *     zap.h/
 *       zap.h
 *
 * Bootstrap once from the Serenity repository root:
 *   clang -std=c11 -O2 zap.c -o zap
 *
 * Then use:
 *   ./zap                 Build all three executables
 *   ./zap test            Build and run the test suite
 *   ./zap interpreter     Build sninterp without LLVM
 *   ./zap compiler       Build the AOT compiler
 *   ./zap jit            Build snjit
 *   ./zap clean          Remove generated files
 *
 * Zap's command execution and self-rebuild facilities are used directly.
 */

#if defined(__has_include)
#  if __has_include("zap.h")
#    include "zap.h"
#  elif __has_include("../zap.h/zap.h")
#    include "../zap.h/zap.h"
#  else
#    error "Cannot find zap.h. Place the Zap checkout beside serenity (../zap.h/zap.h) or put zap.h beside zap.c."
#  endif
#else
#  include "../zap.h/zap.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SERENITY_DEFAULT_BUILD_DIR "build"
#define SERENITY_MACOS_MIN_VERSION "13.0"
#define SERENITY_OBJECT_CAPACITY 96
#define SERENITY_TEST_CAPACITY 16
#define SERENITY_ACTIVE_CAPACITY 128

#if defined(__APPLE__)
#define SERENITY_PLATFORM_MACOS 1
#else
#define SERENITY_PLATFORM_MACOS 0
#endif

typedef enum {
    COMMAND_BUILD,
    COMMAND_TEST,
    COMMAND_COMPILER,
    COMMAND_INTERPRETER,
    COMMAND_JIT,
    COMMAND_CLEAN,
    COMMAND_HELP
} CommandKind;

typedef enum {
    JOB_NORMAL,
    JOB_LLVM,
    JOB_SERENITY_MAIN,
    JOB_STDLIB_TEST,
    JOB_IO_TEST
} JobKind;

typedef struct {
    char source[PATH_MAX];
    char object[PATH_MAX];
    char depfile[PATH_MAX];
    JobKind kind;
} CompileJob;

typedef struct {
    pid_t pid;
    size_t job_index;
} ActiveJob;

typedef struct {
    Cmd cmd;
    char **owned;
    size_t owned_count;
    size_t owned_capacity;
} OwnedCommand;

typedef struct {
    CommandKind command;
    bool release;
    bool verbose;
    bool no_llvm;
    bool require_llvm;
    int jobs;
    char build_dir[PATH_MAX];
    char cc[PATH_MAX];
    char cxx[PATH_MAX];
    char ar[PATH_MAX];
    char llvm_config[PATH_MAX];
    char llvm_version[128];
    char llvm_include[PATH_MAX];
    char llvm_libdir[PATH_MAX];
    char sdk_root[PATH_MAX];
    char sdk_version[128];
    bool llvm_available;
} BuildConfig;

static BuildConfig g_config;
static CompileJob g_jobs[SERENITY_OBJECT_CAPACITY];
static size_t g_job_count = 0;
static char g_project_root[PATH_MAX];

static void fail_usage(const char *message);
static bool build_selected_target(void);
static bool run_tests(void);

static bool path_join(char *out, size_t capacity, const char *left, const char *right) {
    int written;
    if (left[0] == '\0') {
        written = snprintf(out, capacity, "%s", right);
    } else if (left[strlen(left) - 1] == '/') {
        written = snprintf(out, capacity, "%s%s", left, right);
    } else {
        written = snprintf(out, capacity, "%s/%s", left, right);
    }
    return written >= 0 && (size_t)written < capacity;
}

static bool path_parent(char *out, size_t capacity, const char *path) {
    size_t length = strlen(path);
    if (length + 1 > capacity) return false;
    memcpy(out, path, length + 1);
    char *slash = strrchr(out, '/');
    if (slash == NULL) {
        snprintf(out, capacity, ".");
    } else if (slash == out) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return true;
}

static bool mkdir_p(const char *path) {
    char buffer[PATH_MAX];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, path, length + 1);

    for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;
        *cursor = '/';
    }
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;

    struct stat status;
    return stat(buffer, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool ensure_parent_directory(const char *path) {
    char parent[PATH_MAX];
    return path_parent(parent, sizeof(parent), path) && mkdir_p(parent);
}

static bool remove_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode)) return unlink(path) == 0;

    DIR *directory = opendir(path);
    if (directory == NULL) return false;
    struct dirent *entry;
    bool success = true;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        if (!path_join(child, sizeof(child), path, entry->d_name) || !remove_tree(child)) {
            success = false;
            break;
        }
    }
    if (closedir(directory) != 0) success = false;
    if (success && rmdir(path) != 0) success = false;
    return success;
}

static bool executable_exists(const char *path) {
    return path[0] != '\0' && access(path, X_OK) == 0;
}

static bool copy_string(char *out, size_t capacity, const char *value) {
    int written = snprintf(out, capacity, "%s", value != NULL ? value : "");
    return written >= 0 && (size_t)written < capacity;
}

static bool find_in_path(char *out, size_t capacity, const char *name) {
    if (strchr(name, '/') != NULL) {
        return copy_string(out, capacity, name) && executable_exists(out);
    }

    const char *path = getenv("PATH");
    if (path == NULL) path = "";
    const char *begin = path;
    for (;;) {
        const char *end = strchr(begin, ':');
        size_t directory_length = end != NULL ? (size_t)(end - begin) : strlen(begin);
        char directory[PATH_MAX];
        if (directory_length == 0) {
            copy_string(directory, sizeof(directory), ".");
        } else if (directory_length < sizeof(directory)) {
            memcpy(directory, begin, directory_length);
            directory[directory_length] = '\0';
        } else {
            directory[0] = '\0';
        }
        if (directory[0] != '\0' && path_join(out, capacity, directory, name)
            && executable_exists(out)) {
            return true;
        }
        if (end == NULL) break;
        begin = end + 1;
    }
    return false;
}

static char *capture_process(const char *executable, char *const arguments[]) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return NULL;

    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return NULL;
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
        close(descriptors[1]);
        execvp(executable, arguments);
        _exit(127);
    }

    close(descriptors[1]);
    size_t length = 0;
    size_t capacity = 4096;
    char *output = malloc(capacity);
    if (output == NULL) {
        close(descriptors[0]);
        (void)waitpid(child, NULL, 0);
        return NULL;
    }

    char chunk[2048];
    ssize_t amount;
    while ((amount = read(descriptors[0], chunk, sizeof(chunk))) > 0) {
        size_t needed = length + (size_t)amount + 1;
        if (needed > capacity) {
            size_t next_capacity = capacity;
            while (next_capacity < needed) next_capacity *= 2;
            char *next = realloc(output, next_capacity);
            if (next == NULL) {
                free(output);
                close(descriptors[0]);
                (void)waitpid(child, NULL, 0);
                return NULL;
            }
            output = next;
            capacity = next_capacity;
        }
        memcpy(output + length, chunk, (size_t)amount);
        length += (size_t)amount;
    }
    close(descriptors[0]);
    output[length] = '\0';

    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(output);
        return NULL;
    }
    return output;
}

static char *capture_tool(const char *tool, const char *argument) {
    char *arguments[3];
    arguments[0] = (char *)tool;
    arguments[1] = (char *)argument;
    arguments[2] = NULL;
    return capture_process(tool, arguments);
}

static uint64_t stat_mtime_ns(const struct stat *status) {
#if defined(__APPLE__)
    return (uint64_t)status->st_mtimespec.tv_sec * UINT64_C(1000000000)
        + (uint64_t)status->st_mtimespec.tv_nsec;
#else
    return (uint64_t)status->st_mtim.tv_sec * UINT64_C(1000000000)
        + (uint64_t)status->st_mtim.tv_nsec;
#endif
}

static bool depfile_is_current(const char *object, const char *source, const char *depfile) {
    struct stat object_status;
    struct stat source_status;
    struct stat configuration_status;
    char configuration_path[PATH_MAX];
    if (!path_join(configuration_path, sizeof(configuration_path), g_config.build_dir, ".zap-config")) return false;
    if (stat(object, &object_status) != 0 || stat(source, &source_status) != 0
        || stat(configuration_path, &configuration_status) != 0) return false;
    uint64_t object_time = stat_mtime_ns(&object_status);
    if (stat_mtime_ns(&source_status) > object_time
        || stat_mtime_ns(&configuration_status) > object_time) return false;

    FILE *file = fopen(depfile, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long file_length = ftell(file);
    if (file_length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    char *contents = malloc((size_t)file_length + 1);
    if (contents == NULL) { fclose(file); return false; }
    size_t read_count = fread(contents, 1, (size_t)file_length, file);
    fclose(file);
    contents[read_count] = '\0';

    char *cursor = strchr(contents, ':');
    if (cursor == NULL) { free(contents); return false; }
    ++cursor;
    bool current = true;

    while (*cursor != '\0' && current) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
        if (*cursor == '\0') break;

        char dependency[PATH_MAX];
        size_t used = 0;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t'
               && *cursor != '\r' && *cursor != '\n') {
            if (*cursor == '\\' && cursor[1] == '\n') {
                cursor += 2;
                continue;
            }
            if (*cursor == '\\' && cursor[1] != '\0') ++cursor;
            if (used + 1 >= sizeof(dependency)) { current = false; break; }
            dependency[used++] = *cursor++;
        }
        dependency[used] = '\0';
        if (used == 0) continue;

        struct stat dependency_status;
        if (stat(dependency, &dependency_status) != 0
            || stat_mtime_ns(&dependency_status) > object_time) {
            current = false;
        }
    }

    free(contents);
    return current;
}

static void command_init(OwnedCommand *command) {
    memset(command, 0, sizeof(*command));
}

static bool command_add(OwnedCommand *command, const char *argument) {
    if (command->owned_count == command->owned_capacity) {
        size_t capacity = command->owned_capacity == 0 ? 16 : command->owned_capacity * 2;
        char **next = realloc(command->owned, capacity * sizeof(*next));
        if (next == NULL) return false;
        command->owned = next;
        command->owned_capacity = capacity;
    }
    char *copy = strdup(argument);
    if (copy == NULL) return false;
    command->owned[command->owned_count++] = copy;
    cmd_append(&command->cmd, copy);
    return true;
}

static bool command_add_format(OwnedCommand *command, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) { va_end(arguments); return false; }
    char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) { va_end(arguments); return false; }
    (void)vsnprintf(buffer, (size_t)length + 1, format, arguments);
    va_end(arguments);
    bool result = command_add(command, buffer);
    free(buffer);
    return result;
}

static void command_destroy(OwnedCommand *command) {
    for (size_t index = 0; index < command->owned_count; ++index) free(command->owned[index]);
    free(command->owned);
    free(command->cmd.items);
    memset(command, 0, sizeof(*command));
}

static int default_job_count(void) {
#if defined(__APPLE__)
    int apple_cpu_count = 0;
    size_t size = sizeof(apple_cpu_count);
    if (sysctlbyname("hw.ncpu", &apple_cpu_count, &size, NULL, 0) == 0 && apple_cpu_count > 0) {
        return apple_cpu_count;
    }
#endif
    long online_cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (online_cpu_count <= 0) return 4;
    return online_cpu_count > 64 ? 64 : (int)online_cpu_count;
}

static bool parse_positive_integer(const char *text, int *value) {
    if (text == NULL || *text == '\0') return false;
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 1 || parsed > 128) return false;
    *value = (int)parsed;
    return true;
}

static void print_usage(void) {
    puts("Serenity build system (Zap)\n"
         "Usage: ./zap [command] [options]\n\n"
         "Commands:\n"
         "  build          Build serenity, sninterp, and snjit (default)\n"
         "  compiler       Build the AOT compiler: build/bin/serenity\n"
         "  interpreter    Build the LLVM-independent interpreter: build/bin/sninterp\n"
         "  jit            Build the tracing JIT: build/bin/snjit\n"
         "  test           Build and run the unit and backend tests\n"
         "  clean          Remove the build directory\n"
         "  help           Show this help\n\n"
         "Options:\n"
         "  --debug        Build with -O0 and debug information (default)\n"
         "  --release      Build with -O2 and NDEBUG\n"
         "  -j N, --jobs N Set the number of parallel compile jobs\n"
         "  --no-llvm      Build/test the LLVM-independent interpreter subset\n"
         "  --verbose      Print complete compiler and linker command lines\n"
         "  --build-dir P  Set the output directory (default: build)\n"
         "  -h, --help     Show this help\n\n"
         "LLVM is discovered through LLVM_CONFIG or Homebrew llvm-config.\n"
         "The Zap header is expected at ../zap.h/zap.h or beside zap.c.");
}

static bool normalize_build_directory(void) {
    if (g_config.build_dir[0] == '/') return true;
    char absolute[PATH_MAX];
    if (!path_join(absolute, sizeof(absolute), g_project_root, g_config.build_dir)) return false;
    return copy_string(g_config.build_dir, sizeof(g_config.build_dir), absolute);
}

static bool parse_arguments(int argc, char **argv) {
    memset(&g_config, 0, sizeof(g_config));
    g_config.command = COMMAND_BUILD;
    g_config.jobs = default_job_count();
    (void)copy_string(g_config.build_dir, sizeof(g_config.build_dir), SERENITY_DEFAULT_BUILD_DIR);

    int index = 1;
    if (index < argc && argv[index][0] != '-') {
        const char *name = argv[index++];
        if (strcmp(name, "build") == 0 || strcmp(name, "all") == 0) g_config.command = COMMAND_BUILD;
        else if (strcmp(name, "test") == 0) g_config.command = COMMAND_TEST;
        else if (strcmp(name, "compiler") == 0 || strcmp(name, "serenity") == 0) g_config.command = COMMAND_COMPILER;
        else if (strcmp(name, "interpreter") == 0 || strcmp(name, "sninterp") == 0) g_config.command = COMMAND_INTERPRETER;
        else if (strcmp(name, "jit") == 0 || strcmp(name, "snjit") == 0) g_config.command = COMMAND_JIT;
        else if (strcmp(name, "clean") == 0) g_config.command = COMMAND_CLEAN;
        else if (strcmp(name, "help") == 0) g_config.command = COMMAND_HELP;
        else {
            fprintf(stderr, "%s Unknown command '%s'.\n", ZAP_ERR, name);
            return false;
        }
    }

    while (index < argc) {
        const char *option = argv[index++];
        if (strcmp(option, "--debug") == 0) g_config.release = false;
        else if (strcmp(option, "--release") == 0) g_config.release = true;
        else if (strcmp(option, "--verbose") == 0 || strcmp(option, "-v") == 0) g_config.verbose = true;
        else if (strcmp(option, "--no-llvm") == 0) g_config.no_llvm = true;
        else if (strcmp(option, "--require-llvm") == 0) g_config.require_llvm = true;
        else if (strcmp(option, "--jobs") == 0 || strcmp(option, "-j") == 0) {
            if (index >= argc || !parse_positive_integer(argv[index++], &g_config.jobs)) {
                fprintf(stderr, "%s --jobs requires an integer from 1 to 128.\n", ZAP_ERR);
                return false;
            }
        } else if (strncmp(option, "-j", 2) == 0 && option[2] != '\0') {
            if (!parse_positive_integer(option + 2, &g_config.jobs)) {
                fprintf(stderr, "%s Invalid job count '%s'.\n", ZAP_ERR, option + 2);
                return false;
            }
        } else if (strcmp(option, "--build-dir") == 0) {
            if (index >= argc || !copy_string(g_config.build_dir, sizeof(g_config.build_dir), argv[index++])) {
                fprintf(stderr, "%s --build-dir requires a path.\n", ZAP_ERR);
                return false;
            }
        } else if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0) {
            g_config.command = COMMAND_HELP;
        } else {
            fprintf(stderr, "%s Unknown option '%s'.\n", ZAP_ERR, option);
            return false;
        }
    }

    if (g_config.no_llvm && g_config.require_llvm) {
        fprintf(stderr, "%s --no-llvm and --require-llvm cannot be used together.\n", ZAP_ERR);
        return false;
    }
    if (g_config.no_llvm && (g_config.command == COMMAND_COMPILER || g_config.command == COMMAND_JIT)) {
        fprintf(stderr, "%s This target requires LLVM; remove --no-llvm.\n", ZAP_ERR);
        return false;
    }
    if (g_config.build_dir[0] == '\0') {
        fprintf(stderr, "%s Build directory cannot be empty.\n", ZAP_ERR);
        return false;
    }
    return true;
}

static bool initialize_toolchain(void) {
    const char *cc_env = getenv("CC");
    const char *cxx_env = getenv("CXX");
    const char *ar_env = getenv("AR");

    if (cc_env != NULL && *cc_env != '\0') copy_string(g_config.cc, sizeof(g_config.cc), cc_env);
    else if (executable_exists("/opt/homebrew/opt/llvm/bin/clang"))
        copy_string(g_config.cc, sizeof(g_config.cc), "/opt/homebrew/opt/llvm/bin/clang");
    else copy_string(g_config.cc, sizeof(g_config.cc), "clang");

    if (cxx_env != NULL && *cxx_env != '\0') copy_string(g_config.cxx, sizeof(g_config.cxx), cxx_env);
    else if (executable_exists("/opt/homebrew/opt/llvm/bin/clang++"))
        copy_string(g_config.cxx, sizeof(g_config.cxx), "/opt/homebrew/opt/llvm/bin/clang++");
    else copy_string(g_config.cxx, sizeof(g_config.cxx), "clang++");

    if (ar_env != NULL && *ar_env != '\0') copy_string(g_config.ar, sizeof(g_config.ar), ar_env);
    else if (executable_exists("/opt/homebrew/opt/llvm/bin/llvm-ar"))
        copy_string(g_config.ar, sizeof(g_config.ar), "/opt/homebrew/opt/llvm/bin/llvm-ar");
    else copy_string(g_config.ar, sizeof(g_config.ar), "ar");

    if (!executable_exists(g_config.cc) && strchr(g_config.cc, '/') != NULL) {
        fprintf(stderr, "%s C compiler not executable: %s\n", ZAP_ERR, g_config.cc);
        return false;
    }
    if (!executable_exists(g_config.cxx) && strchr(g_config.cxx, '/') != NULL) {
        fprintf(stderr, "%s C++ compiler not executable: %s\n", ZAP_ERR, g_config.cxx);
        return false;
    }

    if (g_config.no_llvm || g_config.command == COMMAND_INTERPRETER || g_config.command == COMMAND_CLEAN) {
        g_config.llvm_available = false;
    } else {
        const char *llvm_env = getenv("LLVM_CONFIG");
        if (llvm_env != NULL && *llvm_env != '\0') {
            copy_string(g_config.llvm_config, sizeof(g_config.llvm_config), llvm_env);
        } else if (executable_exists("/opt/homebrew/opt/llvm/bin/llvm-config")) {
            copy_string(g_config.llvm_config, sizeof(g_config.llvm_config),
                        "/opt/homebrew/opt/llvm/bin/llvm-config");
        } else if (!find_in_path(g_config.llvm_config, sizeof(g_config.llvm_config), "llvm-config")) {
            g_config.llvm_config[0] = '\0';
        }

        if (g_config.llvm_config[0] != '\0') {
            char *version = capture_tool(g_config.llvm_config, "--version");
            char *include = capture_tool(g_config.llvm_config, "--includedir");
            char *libdir = capture_tool(g_config.llvm_config, "--libdir");
            if (version != NULL && include != NULL && libdir != NULL) {
                version[strcspn(version, "\r\n")] = '\0';
                include[strcspn(include, "\r\n")] = '\0';
                libdir[strcspn(libdir, "\r\n")] = '\0';
                copy_string(g_config.llvm_version, sizeof(g_config.llvm_version), version);
                copy_string(g_config.llvm_include, sizeof(g_config.llvm_include), include);
                copy_string(g_config.llvm_libdir, sizeof(g_config.llvm_libdir), libdir);
                g_config.llvm_available = true;
            }
            free(version);
            free(include);
            free(libdir);
        }

        if (!g_config.llvm_available && !g_config.no_llvm && (g_config.require_llvm
            || g_config.command == COMMAND_BUILD || g_config.command == COMMAND_TEST
            || g_config.command == COMMAND_COMPILER || g_config.command == COMMAND_JIT)) {
            fprintf(stderr, "%s LLVM development tools were not found. Set LLVM_CONFIG or install Homebrew LLVM.\n", ZAP_ERR);
            fprintf(stderr, "  Expected: /opt/homebrew/opt/llvm/bin/llvm-config\n");
            fprintf(stderr, "  Interpreter-only build: ./zap interpreter\n");
            return false;
        }
    }

#if SERENITY_PLATFORM_MACOS
    const char *sdk_env = getenv("SDKROOT");
    if (sdk_env != NULL && *sdk_env != '\0') copy_string(g_config.sdk_root, sizeof(g_config.sdk_root), sdk_env);
    else {
        char *sdk_path = NULL;
        {
            char *arguments[] = {"xcrun", "--sdk", "macosx", "--show-sdk-path", NULL};
            sdk_path = capture_process("xcrun", arguments);
        }
        char *sdk_version = NULL;
        {
            char *arguments[] = {"xcrun", "--sdk", "macosx", "--show-sdk-version", NULL};
            sdk_version = capture_process("xcrun", arguments);
        }
        if (sdk_path != NULL) {
            sdk_path[strcspn(sdk_path, "\r\n")] = '\0';
            copy_string(g_config.sdk_root, sizeof(g_config.sdk_root), sdk_path);
        }
        if (sdk_version != NULL) {
            sdk_version[strcspn(sdk_version, "\r\n")] = '\0';
            copy_string(g_config.sdk_version, sizeof(g_config.sdk_version), sdk_version);
        }
        free(sdk_path);
        free(sdk_version);
    }
#endif

    /* Zap's self-rebuilder uses CC; make its default match the selected toolchain. */
    if (getenv("CC") == NULL || *getenv("CC") == '\0') setenv("CC", g_config.cc, 0);
    return true;
}

static bool write_build_signature(void) {
    char path[PATH_MAX];
    if (!path_join(path, sizeof(path), g_config.build_dir, ".zap-config")) return false;

    char signature[PATH_MAX * 4];
    int length = snprintf(signature, sizeof(signature),
        "zap-build-v1\nPROJECT_ROOT=%s\nCC=%s\nCXX=%s\nAR=%s\nLLVM_CONFIG=%s\n"
        "LLVM_VERSION=%s\nLLVM_INCLUDE=%s\nLLVM_LIBDIR=%s\n"
        "LLVM_ENABLED=%d\nMODE=%s\nSDKROOT=%s\nSDK_VERSION=%s\n"
        "MACOS_MIN=%s\n",
        g_project_root, g_config.cc, g_config.cxx, g_config.ar, g_config.llvm_config,
        g_config.llvm_version, g_config.llvm_include, g_config.llvm_libdir,
        g_config.llvm_available ? 1 : 0, g_config.release ? "release" : "debug",
        g_config.sdk_root, g_config.sdk_version, SERENITY_MACOS_MIN_VERSION);
    if (length < 0 || (size_t)length >= sizeof(signature)) return false;

    FILE *existing = fopen(path, "rb");
    if (existing != NULL) {
        char previous[PATH_MAX * 4];
        size_t count = fread(previous, 1, sizeof(previous) - 1, existing);
        bool read_ok = !ferror(existing);
        fclose(existing);
        previous[count] = '\0';
        if (read_ok && strcmp(previous, signature) == 0) return true;
    }

    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid())
        >= (int)sizeof(temporary)) return false;
    FILE *output = fopen(temporary, "wb");
    if (output == NULL) return false;
    bool success = fwrite(signature, 1, (size_t)length, output) == (size_t)length;
    if (fclose(output) != 0) success = false;
    if (success && rename(temporary, path) != 0) success = false;
    if (!success) {
        (void)unlink(temporary);
        fprintf(stderr, "%s Could not update build configuration stamp: %s\n", ZAP_ERR, strerror(errno));
    }
    return success;
}

static bool prepare_directories(void) {
    const char *subdirectories[] = {"obj", "bin", "lib", "tests", NULL};
    for (size_t index = 0; subdirectories[index] != NULL; ++index) {
        char build_subdir[PATH_MAX];
        if (!path_join(build_subdir, sizeof(build_subdir), g_config.build_dir, subdirectories[index])) return false;
        if (!mkdir_p(build_subdir)) {
            fprintf(stderr, "%s Cannot create directory '%s': %s\n", ZAP_ERR, build_subdir, strerror(errno));
            return false;
        }
    }
    return write_build_signature();
}

static bool source_object_paths(const char *source, char *object, size_t object_capacity,
                                char *depfile, size_t depfile_capacity) {
    char relative[PATH_MAX];
    if (!copy_string(relative, sizeof(relative), source)) return false;
    size_t length = strlen(relative);
    if (length + 4 >= sizeof(relative)) return false;
    strcat(relative, ".o");

    char obj_root[PATH_MAX];
    if (!path_join(obj_root, sizeof(obj_root), g_config.build_dir, "obj")) return false;
    if (!path_join(object, object_capacity, obj_root, relative)) return false;
    if (!copy_string(depfile, depfile_capacity, object)) return false;
    length = strlen(depfile);
    if (length + 2 >= depfile_capacity) return false;
    depfile[length - 1] = 'd'; /* .o -> .d */
    depfile[length] = '\0';
    return true;
}

static bool add_compile_job(const char *source, JobKind kind) {
    for (size_t index = 0; index < g_job_count; ++index) {
        if (strcmp(g_jobs[index].source, source) == 0) {
            if (g_jobs[index].kind != kind && kind != JOB_NORMAL) g_jobs[index].kind = kind;
            return true;
        }
    }
    if (g_job_count >= SERENITY_OBJECT_CAPACITY) {
        fprintf(stderr, "%s Internal compile-job capacity exceeded.\n", ZAP_ERR);
        return false;
    }
    CompileJob *job = &g_jobs[g_job_count++];
    memset(job, 0, sizeof(*job));
    if (!copy_string(job->source, sizeof(job->source), source)
        || !source_object_paths(source, job->object, sizeof(job->object),
                                job->depfile, sizeof(job->depfile))) {
        --g_job_count;
        return false;
    }
    job->kind = kind;
    return true;
}

static bool append_macro(OwnedCommand *command, const char *name, const char *value) {
    return command_add_format(command, "-D%s=\"%s\"", name, value);
}

static bool build_one_object(const CompileJob *job) {
    if (depfile_is_current(job->object, job->source, job->depfile)) return true;
    if (!ensure_parent_directory(job->object)) {
        fprintf(stderr, "%s Cannot create object directory for %s\n", ZAP_ERR, job->source);
        return false;
    }

    OwnedCommand command;
    command_init(&command);
    bool ok = command_add(&command, g_config.cxx);
    ok = ok && command_add(&command, "-std=c++20");
    ok = ok && command_add(&command, "-Wall");
    ok = ok && command_add(&command, "-Wextra");
    ok = ok && command_add(&command, "-Wpedantic");
    ok = ok && command_add(&command, "-Wconversion");
    ok = ok && command_add(&command, "-Wshadow");
    if (g_config.release) {
        ok = ok && command_add(&command, "-O2");
        ok = ok && command_add(&command, "-DNDEBUG");
    } else {
        ok = ok && command_add(&command, "-O0");
        ok = ok && command_add(&command, "-g3");
        ok = ok && command_add(&command, "-fno-omit-frame-pointer");
    }

    ok = ok && command_add_format(&command, "-I%s/include", ".");
    ok = ok && command_add_format(&command, "-DSERENITY_HAS_LLVM=%d",
                                  g_config.llvm_available ? 1 : 0);
    char stdlib_path[PATH_MAX];
    ok = ok && path_join(stdlib_path, sizeof(stdlib_path), g_project_root, "stdlib");
    ok = ok && append_macro(&command, "SERENITY_STDLIB_DIRECTORY", stdlib_path);

    if (job->kind == JOB_LLVM && g_config.llvm_available) {
        ok = ok && command_add_format(&command, "-I%s", g_config.llvm_include);
        /* Match LLVM's public preprocessor definitions without importing its C++ standard flag. */
        char *arguments[] = {g_config.llvm_config, "--cxxflags", NULL};
        char *flags = capture_process(g_config.llvm_config, arguments);
        if (flags == NULL) {
            fprintf(stderr, "%s llvm-config --cxxflags failed.\n", ZAP_ERR);
            command_destroy(&command);
            return false;
        }
        char *save = NULL;
        for (char *token = strtok_r(flags, " \t\r\n", &save);
             token != NULL; token = strtok_r(NULL, " \t\r\n", &save)) {
            if (strncmp(token, "-D", 2) == 0 || strncmp(token, "-U", 2) == 0) {
                ok = ok && command_add(&command, token);
            }
        }
        free(flags);
    }

    if (job->kind == JOB_SERENITY_MAIN) {
        char runtime_path[PATH_MAX];
        char relative_runtime_path[PATH_MAX];
        char linker_path[PATH_MAX];
        if (!path_join(relative_runtime_path, sizeof(relative_runtime_path), g_config.build_dir, "lib/libserenity_runtime.a")
            || !path_join(runtime_path, sizeof(runtime_path), g_project_root, relative_runtime_path)) ok = false;
#if SERENITY_PLATFORM_MACOS
        copy_string(linker_path, sizeof(linker_path), "/usr/bin/ld");
#else
        linker_path[0] = '\0';
#endif
        ok = ok && append_macro(&command, "SERENITY_RUNTIME_LIBRARY_PATH", runtime_path);
        ok = ok && append_macro(&command, "SERENITY_LINKER_PATH", linker_path);
        ok = ok && append_macro(&command, "SERENITY_MACOS_SDKROOT", g_config.sdk_root);
        ok = ok && append_macro(&command, "SERENITY_MACOS_SDK_VERSION", g_config.sdk_version);
        ok = ok && append_macro(&command, "SERENITY_MACOS_MIN_VERSION", SERENITY_MACOS_MIN_VERSION);
    } else if (job->kind == JOB_STDLIB_TEST) {
        char test_source[PATH_MAX];
        ok = ok && path_join(test_source, sizeof(test_source), g_project_root, "examples/stdlib_demo.sn");
        ok = ok && append_macro(&command, "SERENITY_STDLIB_TEST_SOURCE", test_source);
    } else if (job->kind == JOB_IO_TEST) {
        char test_source[PATH_MAX];
        ok = ok && path_join(test_source, sizeof(test_source), g_project_root, "examples/io_demo.sn");
        ok = ok && append_macro(&command, "SERENITY_IO_TEST_SOURCE", test_source);
    }

    ok = ok && command_add(&command, "-MMD");
    ok = ok && command_add(&command, "-MF");
    ok = ok && command_add(&command, job->depfile);
    ok = ok && command_add(&command, "-MT");
    ok = ok && command_add(&command, job->object);
    ok = ok && command_add(&command, "-c");
    ok = ok && command_add(&command, job->source);
    ok = ok && command_add(&command, "-o");
    ok = ok && command_add(&command, job->object);

    if (ok && g_config.verbose) cmd_dump(&command.cmd);
    if (ok) ok = cmd_run(&command.cmd);
    if (!ok) fprintf(stderr, "%s Compilation failed: %s\n", ZAP_ERR, job->source);
    command_destroy(&command);
    return ok;
}

static bool build_compile_jobs(void) {
    size_t pending_count = 0;
    size_t pending[SERENITY_OBJECT_CAPACITY];
    for (size_t index = 0; index < g_job_count; ++index) {
        if (!depfile_is_current(g_jobs[index].object, g_jobs[index].source, g_jobs[index].depfile)) {
            pending[pending_count++] = index;
        }
    }
    if (pending_count == 0) {
        zap_log(ZAP_INFO, "All C++ objects are up to date.");
        return true;
    }

    printf("%s Compiling %zu source file(s) with %d job(s).\n", ZAP_INFO, pending_count, g_config.jobs);
    ActiveJob active[SERENITY_ACTIVE_CAPACITY];
    size_t active_count = 0;
    size_t next_job = 0;
    bool success = true;

    while (next_job < pending_count || active_count > 0) {
        while (success && next_job < pending_count && active_count < (size_t)g_config.jobs) {
            size_t job_index = pending[next_job++];
            pid_t child = fork();
            if (child < 0) {
                perror("fork");
                success = false;
                break;
            }
            if (child == 0) _exit(build_one_object(&g_jobs[job_index]) ? 0 : 1);
            active[active_count].pid = child;
            active[active_count].job_index = job_index;
            ++active_count;
        }

        if (active_count > 0) {
            int status = 0;
            pid_t completed = wait(&status);
            if (completed < 0) {
                if (errno == EINTR) continue;
                perror("wait");
                success = false;
                break;
            }
            size_t active_index = 0;
            while (active_index < active_count && active[active_index].pid != completed) ++active_index;
            if (active_index < active_count) {
                active[active_index] = active[active_count - 1];
                --active_count;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) success = false;
        }
        if (!success) {
            for (size_t index = 0; index < active_count; ++index) (void)waitpid(active[index].pid, NULL, 0);
            break;
        }
    }

    if (success) zap_log(ZAP_INFO, "Compilation completed.");
    return success;
}

static void reset_jobs(void) { g_job_count = 0; }

static bool add_core_sources(bool with_interpreter) {
    static const char *const sources[] = {
        "src/lexer/lexer.cpp",
        "src/preprocessor/preprocessor.cpp",
        "src/parser/parser.cpp",
        "src/semantic/analyzer.cpp",
        "src/bytecode/instruction.cpp",
        "src/bytecode/module.cpp",
        "src/bytecode/generator.cpp",
        NULL
    };
    for (size_t index = 0; sources[index] != NULL; ++index) {
        if (!add_compile_job(sources[index], JOB_NORMAL)) return false;
    }
    return !with_interpreter || add_compile_job("src/interpreter/interpreter.cpp", JOB_NORMAL);
}

static bool add_aot_sources(void) {
    return add_compile_job("src/aot/llvm_emitter.cpp", JOB_NORMAL)
        && add_compile_job("src/aot/llvm_backend.cpp", JOB_LLVM);
}

static bool add_jit_sources(void) {
    return add_compile_job("src/jit/jit.cpp", JOB_LLVM);
}

static bool add_runtime_source(void) {
    return add_compile_job("src/runtime/runtime.cpp", JOB_NORMAL);
}

static bool object_for_source(const char *source, char *out, size_t capacity) {
    char depfile[PATH_MAX];
    return source_object_paths(source, out, capacity, depfile, sizeof(depfile));
}

static bool append_object(OwnedCommand *command, const char *source) {
    char object[PATH_MAX];
    return object_for_source(source, object, sizeof(object)) && command_add(command, object);
}

static bool append_llvm_link_flags(OwnedCommand *command) {
    if (!g_config.llvm_available) return true;
    char *arguments[] = {g_config.llvm_config, "--link-shared", "--ldflags", "--libs", "all", "--system-libs", NULL};
    char *flags = capture_process(g_config.llvm_config, arguments);
    if (flags == NULL) {
        fprintf(stderr, "%s llvm-config could not provide LLVM linker flags.\n", ZAP_ERR);
        return false;
    }
    char *save = NULL;
    bool ok = true;
    for (char *token = strtok_r(flags, " \t\r\n", &save);
         token != NULL; token = strtok_r(NULL, " \t\r\n", &save)) {
        ok = ok && command_add(command, token);
    }
    free(flags);
    if (ok && SERENITY_PLATFORM_MACOS && g_config.llvm_libdir[0] != '\0') {
        ok = command_add_format(command, "-Wl,-rpath,%s", g_config.llvm_libdir);
    }
    return ok;
}

static bool link_target(const char *output_name, const char *const sources[], bool use_llvm) {
    char output_directory[PATH_MAX];
    char output[PATH_MAX];
    if (!path_join(output_directory, sizeof(output_directory), g_config.build_dir, "bin")
        || !path_join(output, sizeof(output), output_directory, output_name)) return false;

    OwnedCommand command;
    command_init(&command);
    bool ok = command_add(&command, g_config.cxx);
    for (size_t index = 0; ok && sources[index] != NULL; ++index) {
        ok = append_object(&command, sources[index]);
    }
    ok = ok && command_add(&command, "-o") && command_add(&command, output);
    if (ok && use_llvm) ok = append_llvm_link_flags(&command);
    if (ok && g_config.verbose) cmd_dump(&command.cmd);
    if (ok) ok = cmd_run(&command.cmd);
    if (!ok) fprintf(stderr, "%s Linking failed: %s\n", ZAP_ERR, output_name);
    else printf("%s Built %s\n", ZAP_INFO, output);
    command_destroy(&command);
    return ok;
}

static bool build_runtime_archive(void) {
    char object[PATH_MAX];
    char depfile[PATH_MAX];
    char archive[PATH_MAX];
    if (!source_object_paths("src/runtime/runtime.cpp", object, sizeof(object), depfile, sizeof(depfile))) return false;
    if (!path_join(archive, sizeof(archive), g_config.build_dir, "lib/libserenity_runtime.a")) return false;

    struct stat object_status;
    struct stat archive_status;
    bool rebuild = stat(archive, &archive_status) != 0 || stat(object, &object_status) != 0
        || stat_mtime_ns(&object_status) > stat_mtime_ns(&archive_status);
    if (!rebuild) return true;

    OwnedCommand command;
    command_init(&command);
    bool ok = command_add(&command, g_config.ar)
        && command_add(&command, "rcs")
        && command_add(&command, archive)
        && command_add(&command, object);
    if (ok && g_config.verbose) cmd_dump(&command.cmd);
    if (ok) ok = cmd_run(&command.cmd);
    if (!ok) fprintf(stderr, "%s Could not create %s\n", ZAP_ERR, archive);
    command_destroy(&command);
    return ok;
}

static bool build_interpreter(void) {
    reset_jobs();
    if (!add_core_sources(true) || !add_compile_job("src/main/sninterp.cpp", JOB_NORMAL)) return false;
    if (!build_compile_jobs()) return false;
    static const char *const objects[] = {
        "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp", "src/parser/parser.cpp",
        "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp", "src/bytecode/module.cpp",
        "src/bytecode/generator.cpp", "src/interpreter/interpreter.cpp", "src/main/sninterp.cpp", NULL
    };
    return link_target("sninterp", objects, false);
}

static bool build_compiler(void) {
    if (!g_config.llvm_available) {
        fprintf(stderr, "%s AOT compiler requires LLVM.\n", ZAP_ERR);
        return false;
    }
    reset_jobs();
    if (!add_core_sources(false) || !add_aot_sources()
        || !add_compile_job("src/main/serenity.cpp", JOB_SERENITY_MAIN)
        || !add_runtime_source()) return false;
    if (!build_compile_jobs() || !build_runtime_archive()) return false;
    static const char *const objects[] = {
        "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp", "src/parser/parser.cpp",
        "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp", "src/bytecode/module.cpp",
        "src/bytecode/generator.cpp", "src/aot/llvm_emitter.cpp", "src/aot/llvm_backend.cpp",
        "src/main/serenity.cpp", NULL
    };
    return link_target("serenity", objects, true);
}

static bool build_jit(void) {
    if (!g_config.llvm_available) {
        fprintf(stderr, "%s snjit requires LLVM ORC.\n", ZAP_ERR);
        return false;
    }
    reset_jobs();
    if (!add_core_sources(true) || !add_jit_sources()
        || !add_compile_job("src/main/snjit.cpp", JOB_NORMAL)) return false;
    if (!build_compile_jobs()) return false;
    static const char *const objects[] = {
        "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp", "src/parser/parser.cpp",
        "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp", "src/bytecode/module.cpp",
        "src/bytecode/generator.cpp", "src/interpreter/interpreter.cpp", "src/jit/jit.cpp",
        "src/main/snjit.cpp", NULL
    };
    return link_target("snjit", objects, true);
}

static bool build_all(void) {
    if (!g_config.llvm_available) {
        if (g_config.no_llvm) {
            zap_log(ZAP_INFO, "LLVM disabled; building sninterp only.");
            return build_interpreter();
        }
        fprintf(stderr, "%s Full build requires LLVM. Use './zap build --no-llvm' for an interpreter-only build.\n", ZAP_ERR);
        return false;
    }
    return build_interpreter() && build_compiler() && build_jit();
}

static bool add_test_jobs(void) {
    static const char *const tests[] = {
        "tests/test_lexer.cpp", "tests/test_preprocessor.cpp", "tests/test_ast.cpp",
        "tests/test_parser.cpp", "tests/test_semantic.cpp", "tests/test_bytecode.cpp",
        "tests/test_interpreter.cpp", "tests/test_stdlib.cpp", "tests/test_io.cpp",
        "tests/test_aot.cpp", "tests/test_jit.cpp", NULL
    };
    for (size_t index = 0; tests[index] != NULL; ++index) {
        JobKind kind = JOB_NORMAL;
        if (strcmp(tests[index], "tests/test_stdlib.cpp") == 0) kind = JOB_STDLIB_TEST;
        else if (strcmp(tests[index], "tests/test_io.cpp") == 0) kind = JOB_IO_TEST;
        if (!add_compile_job(tests[index], kind)) return false;
    }
    if (g_config.llvm_available && !add_compile_job("tests/test_llvm_backend.cpp", JOB_NORMAL)) return false;
    return true;
}

static bool link_test_binaries(void) {
    static const char *const lexer[] = {"tests/test_lexer.cpp", "src/lexer/lexer.cpp", NULL};
    static const char *const preprocessor[] = {"tests/test_preprocessor.cpp", "src/preprocessor/preprocessor.cpp", NULL};
    static const char *const ast[] = {"tests/test_ast.cpp", "src/lexer/lexer.cpp", NULL};
    static const char *const parser[] = {"tests/test_parser.cpp", "src/lexer/lexer.cpp", "src/parser/parser.cpp", NULL};
    static const char *const semantic[] = {"tests/test_semantic.cpp", "src/lexer/lexer.cpp", "src/parser/parser.cpp", "src/semantic/analyzer.cpp", NULL};
    static const char *const bytecode[] = {
        "tests/test_bytecode.cpp", "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp",
        "src/parser/parser.cpp", "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp",
        "src/bytecode/module.cpp", "src/bytecode/generator.cpp", NULL
    };
    static const char *const interpreter[] = {
        "tests/test_interpreter.cpp", "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp",
        "src/parser/parser.cpp", "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp",
        "src/bytecode/module.cpp", "src/bytecode/generator.cpp", "src/interpreter/interpreter.cpp", NULL
    };
    static const char *const stdlib[] = {
        "tests/test_stdlib.cpp", "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp",
        "src/parser/parser.cpp", "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp",
        "src/bytecode/module.cpp", "src/bytecode/generator.cpp", "src/interpreter/interpreter.cpp", NULL
    };
    static const char *const io[] = {
        "tests/test_io.cpp", "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp",
        "src/parser/parser.cpp", "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp",
        "src/bytecode/module.cpp", "src/bytecode/generator.cpp", "src/interpreter/interpreter.cpp", NULL
    };
    static const char *const aot[] = {
        "tests/test_aot.cpp", "src/lexer/lexer.cpp", "src/parser/parser.cpp", "src/semantic/analyzer.cpp",
        "src/bytecode/instruction.cpp", "src/bytecode/module.cpp", "src/bytecode/generator.cpp",
        "src/aot/llvm_emitter.cpp", "src/aot/llvm_backend.cpp", NULL
    };
    static const char *const jit[] = {
        "tests/test_jit.cpp", "src/lexer/lexer.cpp", "src/preprocessor/preprocessor.cpp",
        "src/parser/parser.cpp", "src/semantic/analyzer.cpp", "src/bytecode/instruction.cpp",
        "src/bytecode/module.cpp", "src/bytecode/generator.cpp", "src/interpreter/interpreter.cpp",
        "src/jit/jit.cpp", NULL
    };
    static const char *const llvm_backend[] = {"tests/test_llvm_backend.cpp", "src/aot/llvm_backend.cpp", NULL};

    return link_target("serenity_lexer_tests", lexer, false)
        && link_target("serenity_preprocessor_tests", preprocessor, false)
        && link_target("serenity_ast_tests", ast, false)
        && link_target("serenity_parser_tests", parser, false)
        && link_target("serenity_semantic_tests", semantic, false)
        && link_target("serenity_bytecode_tests", bytecode, false)
        && link_target("serenity_interpreter_tests", interpreter, false)
        && link_target("serenity_stdlib_tests", stdlib, false)
        && link_target("serenity_io_tests", io, false)
        && link_target("serenity_aot_tests", aot, g_config.llvm_available)
        && link_target("serenity_jit_tests", jit, g_config.llvm_available)
        && (!g_config.llvm_available || link_target("serenity_aot_backend_tests", llvm_backend, true));
}

static bool run_test_binary(const char *name) {
    char bin_directory[PATH_MAX];
    char executable[PATH_MAX];
    if (!path_join(bin_directory, sizeof(bin_directory), g_config.build_dir, "bin")
        || !path_join(executable, sizeof(executable), bin_directory, name)) return false;

    OwnedCommand command;
    command_init(&command);
    bool ok = command_add(&command, executable);
    if (ok && g_config.verbose) cmd_dump(&command.cmd);
    if (ok) ok = cmd_run(&command.cmd);
    if (!ok) fprintf(stderr, "%s Test failed: %s\n", ZAP_ERR, name);
    command_destroy(&command);
    return ok;
}

#if SERENITY_PLATFORM_MACOS
static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *contents = malloc((size_t)length + 1);
    if (contents == NULL) { fclose(file); return NULL; }
    size_t count = fread(contents, 1, (size_t)length, file);
    fclose(file);
    contents[count] = '\0';
    return contents;
}

static char *capture_source_program(const char *executable, const char *source) {
    char *arguments[] = {(char *)executable, (char *)source, NULL};
    return capture_process(executable, arguments);
}

static bool compile_aot_smoke(const char *source, const char *output) {
    char compiler[PATH_MAX];
    if (!path_join(compiler, sizeof(compiler), g_config.build_dir, "bin/serenity")) return false;
    OwnedCommand command;
    command_init(&command);
    bool ok = command_add(&command, compiler)
        && command_add(&command, source)
        && command_add(&command, "-o")
        && command_add(&command, output)
        && command_add(&command, "-O2");
    if (ok && g_config.verbose) cmd_dump(&command.cmd);
    if (ok) ok = cmd_run(&command.cmd);
    command_destroy(&command);
    return ok;
}

static char *capture_executable(const char *executable) {
    char *arguments[] = {(char *)executable, NULL};
    return capture_process(executable, arguments);
}

static bool compare_backend_outputs(const char *source, const char *expected,
                                    bool include_jit, const char *label) {
    char interpreter[PATH_MAX];
    char jit[PATH_MAX];
    char output_directory[PATH_MAX];
    char native_output[PATH_MAX];
    if (!path_join(interpreter, sizeof(interpreter), g_config.build_dir, "bin/sninterp")
        || !path_join(jit, sizeof(jit), g_config.build_dir, "bin/snjit")
        || !path_join(output_directory, sizeof(output_directory), g_config.build_dir, "tests")
        || !path_join(native_output, sizeof(native_output), output_directory, label)) return false;

    char *interpreter_output = capture_source_program(interpreter, source);
    if (interpreter_output == NULL) {
        fprintf(stderr, "%s sninterp smoke test failed for %s\n", ZAP_ERR, source);
        return false;
    }
    if (strcmp(interpreter_output, expected) != 0) {
        fprintf(stderr, "%s sninterp output mismatch for %s\nExpected:\n%s\nActual:\n%s\n",
                ZAP_ERR, source, expected, interpreter_output);
        free(interpreter_output);
        return false;
    }

    if (include_jit) {
        char *jit_output = capture_source_program(jit, source);
        if (jit_output == NULL || strcmp(jit_output, expected) != 0) {
            fprintf(stderr, "%s snjit output mismatch or execution failure for %s\n", ZAP_ERR, source);
            if (jit_output != NULL) {
                fprintf(stderr, "Expected:\n%s\nActual:\n%s\n", expected, jit_output);
                free(jit_output);
            }
            free(interpreter_output);
            return false;
        }
        free(jit_output);
    }

    if (!compile_aot_smoke(source, native_output)) {
        fprintf(stderr, "%s AOT compilation failed for %s\n", ZAP_ERR, source);
        free(interpreter_output);
        return false;
    }
    char *native_output_text = capture_executable(native_output);
    if (native_output_text == NULL || strcmp(native_output_text, expected) != 0) {
        fprintf(stderr, "%s AOT output mismatch or execution failure for %s\n", ZAP_ERR, source);
        if (native_output_text != NULL) {
            fprintf(stderr, "Expected:\n%s\nActual:\n%s\n", expected, native_output_text);
            free(native_output_text);
        }
        free(interpreter_output);
        return false;
    }

    free(native_output_text);
    free(interpreter_output);
    printf("%s AOT%s smoke test passed: %s\n", ZAP_INFO, include_jit ? "/JIT/interpreter" : "/interpreter", label);
    return true;
}

#endif

static bool run_backend_smoke_tests(void) {
#if SERENITY_PLATFORM_MACOS
    char expected_path[PATH_MAX];
    if (!path_join(expected_path, sizeof(expected_path), g_project_root, "examples/stdlib_demo.expected")) return false;
    char *stdlib_expected = read_text_file(expected_path);
    if (stdlib_expected == NULL) {
        fprintf(stderr, "%s Cannot read %s\n", ZAP_ERR, expected_path);
        return false;
    }
    bool ok = compare_backend_outputs("examples/aot_demo.sn",
        "Serenity\n120\n7\n3.5\n4\nAOT OK\n", false, "aot_demo_native")
        && compare_backend_outputs("examples/stdlib_demo.sn", stdlib_expected, true, "stdlib_demo_native");
    free(stdlib_expected);
    return ok;
#else
    zap_log(ZAP_INFO, "Native AOT executable smoke tests are currently macOS-only.");
    return true;
#endif
}

static bool run_tests(void) {
    /* LLVM-enabled tests build all tools; the fallback run still exercises the
       complete frontend, bytecode pipeline, and interpreter without LLVM. */
    if (g_config.llvm_available) {
        if (!build_all()) return false;
    } else {
        if (!g_config.no_llvm) {
            fprintf(stderr, "%s LLVM was not found. Use './zap test --no-llvm' for the LLVM-independent tests.\n", ZAP_ERR);
            return false;
        }
        if (!build_interpreter()) return false;
    }
    reset_jobs();
    if (!add_test_jobs() || !add_aot_sources() || !add_jit_sources()
        || !build_compile_jobs() || !link_test_binaries()) return false;

    static const char *const names[] = {
        "serenity_lexer_tests", "serenity_preprocessor_tests", "serenity_ast_tests",
        "serenity_parser_tests", "serenity_semantic_tests", "serenity_bytecode_tests",
        "serenity_interpreter_tests", "serenity_stdlib_tests", "serenity_io_tests",
        "serenity_aot_tests", "serenity_jit_tests", NULL
    };
    for (size_t index = 0; names[index] != NULL; ++index) {
        if (!run_test_binary(names[index])) return false;
    }
    if (g_config.llvm_available && !run_test_binary("serenity_aot_backend_tests")) return false;
    if (g_config.llvm_available && !run_backend_smoke_tests()) return false;

    zap_log(ZAP_INFO, "All Serenity unit and enabled backend integration tests passed.");
    return true;
}

static bool build_selected_target(void) {
    if (!prepare_directories()) return false;
    switch (g_config.command) {
    case COMMAND_BUILD: return build_all();
    case COMMAND_COMPILER: return build_compiler();
    case COMMAND_INTERPRETER: return build_interpreter();
    case COMMAND_JIT: return build_jit();
    case COMMAND_TEST: return run_tests();
    case COMMAND_CLEAN:
    case COMMAND_HELP:
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    /* Resolve the running executable, not __FILE__: zap may have been compiled
       with a relative source path and launched from an unrelated directory. */
    char executable_candidate[PATH_MAX];
    char executable_path[PATH_MAX];
    char project_root[PATH_MAX];
    if (strchr(argv[0], '/') != NULL) {
        if (!copy_string(executable_candidate, sizeof(executable_candidate), argv[0])) {
            fprintf(stderr, "%s Executable path is too long.\n", ZAP_ERR);
            return EXIT_FAILURE;
        }
    } else if (!find_in_path(executable_candidate, sizeof(executable_candidate), argv[0])) {
        fprintf(stderr, "%s Could not find the Zap executable in PATH.\n", ZAP_ERR);
        return EXIT_FAILURE;
    }
    if (realpath(executable_candidate, executable_path) == NULL) {
        perror("realpath executable");
        return EXIT_FAILURE;
    }
    if (!path_parent(project_root, sizeof(project_root), executable_path)
        || chdir(project_root) != 0) {
        perror("chdir to Serenity project root");
        return EXIT_FAILURE;
    }
    /* zap_rebuild() uses argv[0] after chdir; give it an absolute path. */
    argv[0] = executable_path;
    if (getcwd(g_project_root, sizeof(g_project_root)) == NULL) {
        perror("getcwd");
        return EXIT_FAILURE;
    }

    if (!parse_arguments(argc, argv)) {
        fail_usage("Invalid command line.");
        return EXIT_FAILURE;
    }
    if (!normalize_build_directory()) {
        fprintf(stderr, "%s Invalid build directory.\n", ZAP_ERR);
        return EXIT_FAILURE;
    }
    if (g_config.command == COMMAND_HELP) {
        print_usage();
        return EXIT_SUCCESS;
    }

    /* Ensure Zap's self-rebuild uses the selected Homebrew C compiler by default. */
    if ((getenv("CC") == NULL || *getenv("CC") == '\0')
        && executable_exists("/opt/homebrew/opt/llvm/bin/clang")) {
        setenv("CC", "/opt/homebrew/opt/llvm/bin/clang", 0);
    }
    ZAP_REBUILD(argc, argv);

    if (g_config.command == COMMAND_CLEAN) {
        if (!remove_tree(g_config.build_dir)) {
            fprintf(stderr, "%s Failed to remove '%s': %s\n", ZAP_ERR, g_config.build_dir, strerror(errno));
            return EXIT_FAILURE;
        }
        printf("%s Removed %s\n", ZAP_INFO, g_config.build_dir);
        return EXIT_SUCCESS;
    }

    if (!initialize_toolchain()) return EXIT_FAILURE;
    if (g_config.llvm_available) {
        printf("%s LLVM %s (%s)\n", ZAP_INFO, g_config.llvm_version, g_config.llvm_config);
    } else {
        zap_log(ZAP_INFO, "LLVM disabled; only the interpreter target is available.");
    }

    if (!build_selected_target()) return EXIT_FAILURE;
    zap_log(ZAP_INFO, "Done.");
    return EXIT_SUCCESS;
}

static void fail_usage(const char *message) {
    fprintf(stderr, "%s %s\n", ZAP_ERR, message);
    print_usage();
}
