#ifndef _WIN32
#define _DEFAULT_SOURCE // lstat and readlink under strict C
#endif

#include "folders.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

static bool ends_with(const char *name, const char *extension)
{
    const size_t n = strlen(name);
    const size_t e = strlen(extension);
    if (n <= e) return false;
#ifdef _WIN32
    return _stricmp(name + n - e, extension) == 0; // Windows names ignore case
#else
    return strcmp(name + n - e, extension) == 0;
#endif
}

// `folder` + `name` + `suffix`, malloc'd.
static char *join(const char *folder, const char *name, const char *suffix)
{
    const size_t len = strlen(folder) + strlen(name) + strlen(suffix) + 1;
    char *path = malloc(len);
    if (!path) abort();
    snprintf(path, len, "%s%s%s", folder, name, suffix);
    return path;
}

void folder_find(const char *folder, const char *extension, const folder_file_fn found, void *user)
{
#ifdef _WIN32
    char *pattern = join(folder, "*", "");
    WIN32_FIND_DATAA entry;
    const HANDLE search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = entry.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; // A link
            char *sub = join(folder, name, "/");
            folder_find(sub, extension, found, user);
            free(sub);
        } else if (ends_with(name, extension)) {
            char *path = join(folder, name, "");
            found(user, path);
            free(path);
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
#else
    DIR *dir = opendir(folder);
    if (!dir) return;
    for (const struct dirent *entry; (entry = readdir(dir));) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char *path = join(folder, name, "");
        struct stat info;
        if (lstat(path, &info) == 0) {
            if (S_ISDIR(info.st_mode)) {
                char *sub = join(folder, name, "/");
                folder_find(sub, extension, found, user);
                free(sub);
            } else if (S_ISREG(info.st_mode) && ends_with(name, extension)) {
                found(user, path);
            }
        }
        free(path);
    }
    closedir(dir);
#endif
}

char *folder_of_program(void)
{
    char path[4096] = "";
#ifdef _WIN32
    const DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n == sizeof path) return NULL;
#elif defined(__APPLE__)
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return NULL;
#else
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return NULL;
    path[n] = '\0';
#endif
    char *slash = NULL;
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
        if (*p == '/') slash = p;
    }
    if (!slash) return NULL;
    slash[1] = '\0';
    return join(path, "", "");
}
