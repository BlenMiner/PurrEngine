#ifndef _WIN32
#define _DEFAULT_SOURCE // realpath, readlink, lstat under strict C
#endif

#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

static char *copy(const char *s)
{
    const size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) abort();
    memcpy(out, s, n + 1);
    return out;
}

static void to_forward_slashes(char *s)
{
    for (; *s; s++) {
        if (*s == '\\') *s = '/';
    }
}

char *path_join(const char *a, const char *b)
{
    const size_t na = strlen(a);
    const bool slash = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    char *out = malloc(na + strlen(b) + 2);
    if (!out) abort();
    sprintf(out, "%s%s%s", a, slash ? "/" : "", b);
    return out;
}

char *path_dir(const char *path)
{
    char *out = copy(path);
    to_forward_slashes(out);
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    else strcpy(out, ".");
    return out;
}

const char *path_base(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

char *path_absolute(const char *path)
{
#ifdef _WIN32
    char buffer[MAX_PATH * 4];
    const DWORD n = GetFullPathNameA(path, sizeof buffer, buffer, NULL);
    char *out = copy(n > 0 && n < sizeof buffer ? buffer : path);
#else
    char *resolved = realpath(path, NULL);
    char *out = resolved ? resolved : copy(path);
#endif
    to_forward_slashes(out);
    const size_t n2 = strlen(out);
    if (n2 > 1 && out[n2 - 1] == '/') out[n2 - 1] = '\0';
    return out;
}

char *sys_exe_dir(void)
{
    char buffer[4096] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, buffer, sizeof buffer);
#elif defined(__APPLE__)
    uint32_t size = sizeof buffer;
    _NSGetExecutablePath(buffer, &size);
#else
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n > 0) buffer[n] = '\0';
#endif
    char *absolute = path_absolute(buffer);
    char *dir = path_dir(absolute);
    free(absolute);
    return dir;
}

bool sys_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

bool sys_is_dir(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}

int64_t sys_mtime(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (int64_t)info.st_mtime : 0;
}

static bool make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

bool sys_mkdirs(const char *path)
{
    char *p = copy(path);
    to_forward_slashes(p);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/') continue;
        *c = '\0';
        if (!sys_exists(p)) make_dir(p);
        *c = '/';
    }
    if (!sys_exists(p)) make_dir(p);
    const bool ok = sys_is_dir(p);
    free(p);
    return ok;
}

bool sys_rename(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}

bool sys_remove(const char *path)
{
#ifdef _WIN32
    if (sys_is_dir(path)) return RemoveDirectoryA(path) != 0;
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileA(path) != 0;
#else
    return remove(path) == 0;
#endif
}

typedef struct tree_walk {
    const char *dir;
    bool ok;
} tree_walk;

static void remove_entry(void *user, const char *name, const bool is_dir)
{
    tree_walk *w = user;
    char *path = path_join(w->dir, name);
    if (is_dir) w->ok &= sys_remove_tree(path);
    else w->ok &= sys_remove(path);
    free(path);
}

bool sys_remove_tree(const char *path)
{
    if (!sys_exists(path)) return true;
    if (!sys_is_dir(path)) return sys_remove(path);
    tree_walk w = {path, true};
    sys_list(path, remove_entry, &w);
    return sys_remove(path) && w.ok;
}

char *sys_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)(size > 0 ? size : 0) + 1);
    if (!text) abort();
    const size_t n = fread(text, 1, (size_t)(size > 0 ? size : 0), f);
    fclose(f);
    text[n] = '\0';
    if (len) *len = n;
    return text;
}

bool sys_write_file(const char *path, const char *data, const size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

bool sys_write_text(const char *path, const char *text)
{
    return sys_write_file(path, text, strlen(text));
}

void sys_list(const char *dir, const sys_entry_fn found, void *user)
{
#ifdef _WIN32
    char *pattern = path_join(dir, "*");
    WIN32_FIND_DATAA entry;
    const HANDLE search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) continue;
        const bool is_dir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                         && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        found(user, entry.cFileName, is_dir);
    } while (FindNextFileA(search, &entry));
    FindClose(search);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    for (const struct dirent *entry; (entry = readdir(d));) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *path = path_join(dir, entry->d_name);
        struct stat info;
        const bool is_dir = lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
        free(path);
        found(user, entry->d_name, is_dir);
    }
    closedir(d);
#endif
}

const char *sys_env(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] ? value : NULL;
}

int64_t sys_now(void)
{
    return (int64_t)time(NULL);
}

// ---------------------------------------------------------------------------
// Programs

#ifdef _WIN32

// One argument as the program's CommandLineToArgvW will read it back.
static void append_arg(char **out, size_t *len, size_t *cap, const char *arg)
{
#define PUT(c)                                                                                                   \
    do {                                                                                                         \
        if (*len + 2 >= *cap) {                                                                                  \
            *cap = *cap * 2 + 64;                                                                                \
            *out = realloc(*out, *cap);                                                                          \
            if (!*out) abort();                                                                                  \
        }                                                                                                        \
        (*out)[(*len)++] = (c);                                                                                  \
    } while (0)
    if (*len > 0) PUT(' ');
    if (arg[0] && !strpbrk(arg, " \t\n\v\"")) {
        for (const char *p = arg; *p; p++) PUT(*p);
        (*out)[*len] = '\0';
        return;
    }
    PUT('"');
    for (const char *p = arg;; p++) {
        int slashes = 0;
        while (*p == '\\') {
            slashes++;
            p++;
        }
        if (*p == '\0') {
            for (int i = 0; i < slashes * 2; i++) PUT('\\');
            break;
        }
        if (*p == '"') {
            for (int i = 0; i < slashes * 2 + 1; i++) PUT('\\');
        } else {
            for (int i = 0; i < slashes; i++) PUT('\\');
        }
        PUT(*p);
    }
    PUT('"');
    (*out)[*len] = '\0';
#undef PUT
}

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t k = strlen(suffix);
    return n >= k && _stricmp(s + n - k, suffix) == 0;
}

int sys_run(const char *const *argv, const char *cwd, const bool quiet)
{
    char *line = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (int i = 0; argv[i]; i++) append_arg(&line, &len, &cap, argv[i]);

    // Batch files run through cmd.
    char *command = line;
    if (ends_with(argv[0], ".bat") || ends_with(argv[0], ".cmd")) {
        command = malloc(len + 32);
        if (!command) abort();
        sprintf(command, "cmd.exe /d /s /c \"%s\"", line);
    }

    STARTUPINFOA startup;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    HANDLE null_file = INVALID_HANDLE_VALUE;
    if (quiet) {
        SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
        null_file = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = null_file;
        startup.hStdError = null_file;
    }
    PROCESS_INFORMATION process;
    const BOOL started = CreateProcessA(NULL, command, NULL, NULL, quiet, 0, NULL, cwd, &startup, &process);
    if (command != line) free(command);
    free(line);
    if (null_file != INVALID_HANDLE_VALUE) CloseHandle(null_file);
    if (!started) return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return (int)code;
}

#else

int sys_run(const char *const *argv, const char *cwd, const bool quiet)
{
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) _exit(127);
        if (quiet) {
            const int null_file = open("/dev/null", O_WRONLY);
            if (null_file >= 0) {
                dup2(null_file, 1);
                dup2(null_file, 2);
            }
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
    return 1;
}

#endif

char *sys_which(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return NULL;
#ifdef _WIN32
    const char separator = ';';
    const char *const extensions[] = {"", ".exe", ".bat", ".cmd"};
#else
    const char separator = ':';
    const char *const extensions[] = {""};
#endif
    const char *start = path;
    for (;;) {
        const char *end = strchr(start, separator);
        const size_t n = end ? (size_t)(end - start) : strlen(start);
        if (n > 0) {
            char dir[4096];
            snprintf(dir, sizeof dir, "%.*s", (int)n, start);
            for (size_t e = 0; e < sizeof extensions / sizeof extensions[0]; e++) {
                char candidate[4200];
                snprintf(candidate, sizeof candidate, "%s/%s%s", dir, name, extensions[e]);
                if (sys_exists(candidate) && !sys_is_dir(candidate)) {
                    char *out = copy(candidate);
                    to_forward_slashes(out);
                    return out;
                }
            }
        }
        if (!end) return NULL;
        start = end + 1;
    }
}
