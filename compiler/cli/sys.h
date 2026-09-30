#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the purr command needs from the operating system: files, folders,
// processes and paths. Paths use forward slashes, which every OS accepts.
// Strings returned are malloc'd.

// The folder purr.exe is in.
char *sys_exe_dir(void);

bool sys_exists(const char *path);
bool sys_is_dir(const char *path);
int64_t sys_mtime(const char *path); // 0 if it doesn't exist
// Changes whenever the file does: its modification time, finer than seconds,
// and its size. 0 if it doesn't exist.
uint64_t sys_file_stamp(const char *path);
bool sys_mkdirs(const char *path);   // Creates every missing folder on the way
void sys_hide(const char *path);     // Hides a file or folder on Windows; elsewhere a leading dot does
bool sys_rename(const char *from, const char *to);
bool sys_remove(const char *path);
bool sys_remove_tree(const char *path); // A file, or a folder and everything in it
char *sys_read_file(const char *path, size_t *len);
bool sys_write_file(const char *path, const char *data, size_t len);
bool sys_write_text(const char *path, const char *text);

// Calls `found` with the name of every entry of `dir` (not "." or "..").
typedef void (*sys_entry_fn)(void *user, const char *name, bool is_dir);
void sys_list(const char *dir, sys_entry_fn found, void *user);

// Runs a program and waits for it. argv ends with NULL; argv[0] is a path or a
// name looked up on PATH. `cwd` may be NULL. Returns the exit code, or -1 if it
// couldn't start. With `quiet`, its output is discarded.
int sys_run(const char *const *argv, const char *cwd, bool quiet);

// Starts a program without waiting for it, like sys_run. NULL if it couldn't
// start.
typedef struct sys_process sys_process;
sys_process *sys_start(const char *const *argv, const char *cwd);
// Waits up to `ms` milliseconds for it to end. True once it has, with its exit
// code in `code`; the process is freed then.
bool sys_wait(sys_process *p, int ms, int *code);

uint32_t sys_pid(void);                // This process's
bool sys_process_alive(uint32_t pid); // Whether a process with that ID is running

// Reads lines typed into the terminal, in the background: sys_typed_line
// returns the latest one (without its line break) once it's whole.
void sys_read_lines(void);
bool sys_typed_line(char *out, size_t size);

// Finds a program on PATH (with the usual extensions on Windows).
char *sys_which(const char *name);

const char *sys_env(const char *name); // NULL if unset or empty
int64_t sys_now(void);                 // Seconds since 1970

// "a" + "/" + "b".
char *path_join(const char *a, const char *b);
// The folder part of a path: "a/b/c" -> "a/b".
char *path_dir(const char *path);
// The last part of a path: "a/b/c.purr" -> "c.purr".
const char *path_base(const char *path);
// The absolute form of a path, with forward slashes.
char *path_absolute(const char *path);
