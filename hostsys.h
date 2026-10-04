/* hostsys.h: what m68k-os9-xcc needs from the host system, for POSIX and
 * Windows: running programs (through libiberty's pex_one), file times,
 * renaming over a file, directories, finding a file regardless of case,
 * absolute paths, the temporary directory. Paths are allocated with
 * libiberty's xmalloc, which exits when out of memory.
 *
 * MIT License; see elf2mod.c.
 */

#ifndef HOSTSYS_H
#define HOSTSYS_H

#include <stddef.h>
#include <time.h>

/* Runs ARGV[0] (looked up on PATH) with ARGV, a null-terminated list, and
 * waits for it. OUT and ERR, if not null, are files its standard output and
 * standard error go to (created or truncated). Returns its exit status, or -1
 * if it could not be run (reported). */
int host_run(const char *const argv[], const char *out, const char *err);

/* The modification time of PATH, or (time_t)-1 if it doesn't exist. */
time_t host_mtime(const char *path);

/* Renames FROM to TO, replacing TO if it exists. Returns 0, or -1. */
int host_replace(const char *from, const char *to);

/* Creates directory PATH and the ones above it that are missing. Returns 0,
 * or -1. */
int host_mkdirs(const char *path);

/* Tests whether files can be created in directory PATH (it must exist). */
int host_writable_dir(const char *path);

/* Looks for NAME in directory DIR regardless of case. Returns the path of the
 * file found (allocated), or null. */
char *host_find_nocase(const char *dir, const char *name);

/* The absolute form of PATH, which needn't exist (allocated), with '/' as the
 * separator. */
char *host_abspath(const char *path);

/* The directory for temporary files: TMPDIR, TMP or TEMP, else the system's
 * (allocated, without a trailing separator). */
char *host_tempdir(void);

/* A number for unique temporary names: the process ID. */
long host_pid(void);

/* Whether character C separates path components. */
int host_is_sep(int c);

#endif
