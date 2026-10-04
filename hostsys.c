/* hostsys.c: the host system functions of hostsys.h, for POSIX and Windows.
 * Programs are run through libiberty's pex_one, as GCC's driver runs its
 * passes, which handles both (and Windows' command line quoting).
 *
 * MIT License; see elf2mod.c.
 */

#include "hostsys.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libiberty.h>

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <windows.h>

int host_is_sep(int c)
{
	return c == '/' || c == '\\';
}

/* pex_one's status is the exit code. */
static int exit_code(int status)
{
	return status;
}

int host_replace(const char *from, const char *to)
{
	return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

static int make_dir(const char *path)
{
	return _mkdir(path);
}

int host_writable_dir(const char *path)
{
	return _access(path, 2) == 0;
}

char *host_find_nocase(const char *dir, const char *name)
{
	/* Windows file names are case-insensitive already. */
	char *path = concat(dir, "/", name, NULL);
	if (_access(path, 0) != 0) {
		free(path);
		return NULL;
	}
	return path;
}

#else /* POSIX */

#include <dirent.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

int host_is_sep(int c)
{
	return c == '/';
}

/* pex_one's status is a wait status. */
static int exit_code(int status)
{
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int host_replace(const char *from, const char *to)
{
	return rename(from, to);
}

static int make_dir(const char *path)
{
	return mkdir(path, 0777);
}

int host_writable_dir(const char *path)
{
	return access(path, W_OK) == 0;
}

char *host_find_nocase(const char *dir, const char *name)
{
	DIR *d = opendir(dir);
	if (d == NULL)
		return NULL;
	char *path = NULL;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcasecmp(e->d_name, name) == 0) {
			path = concat(dir, "/", e->d_name, NULL);
			break;
		}
	}
	closedir(d);
	return path;
}

#endif

int host_run(const char *const argv[], const char *out, const char *err)
{
	int status, error;

	fflush(stdout);
	fflush(stderr);
	/* pex_one doesn't imply PEX_LAST: without it, OUT would be a pipeline
	 * temporary, removed again, and no OUT would swallow the output. */
	const char *message = pex_one(PEX_SEARCH | PEX_LAST, argv[0], (char *const *)argv, argv[0],
		out, err, &status, &error);
	if (message != NULL) {
		fprintf(stderr, "cannot run %s: %s%s%s\n", argv[0], message,
			error != 0 ? ": " : "", error != 0 ? strerror(error) : "");
		return -1;
	}
	return exit_code(status);
}

time_t host_mtime(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return (time_t)-1;
	return st.st_mtime;
}

int host_mkdirs(const char *path)
{
	char *copy = xstrdup(path);
	/* Create each prefix ending at a separator, then the whole path; past a
	 * Windows drive letter, and leaving out existing directories. */
	for (char *p = copy + 1; ; p++) {
		if (*p == '\0' || host_is_sep(*p)) {
			char c = *p;
			*p = '\0';
			if (!(p > copy && p[-1] == ':') && host_mtime(copy) == (time_t)-1 &&
				make_dir(copy) != 0 && errno != EEXIST) {
				free(copy);
				return -1;
			}
			*p = c;
			if (c == '\0')
				break;
		}
	}
	free(copy);
	return 0;
}

char *host_abspath(const char *path)
{
	char *full = lrealpath(path);
	if (full == NULL || (full[0] != '/' && !(full[0] != '\0' && full[1] == ':'))) {
		/* lrealpath gives the path back unchanged when it doesn't exist. */
		char *cwd = getpwd();
		char *joined = host_is_sep(*path) ? xstrdup(path) : concat(cwd, "/", path, NULL);
		free(full);
		full = joined;
	}
	for (char *p = full; *p != '\0'; p++)
		if (*p == '\\')
			*p = '/';
	return full;
}

char *host_tempdir(void)
{
	/* choose_tmpdir checks TMPDIR, TMP and TEMP, then the usual places, and
	 * ends its result with a separator. */
	char *dir = xstrdup(choose_tmpdir());
	size_t length = strlen(dir);
	while (length > 1 && host_is_sep(dir[length - 1]))
		dir[--length] = '\0';
	return dir;
}

long host_pid(void)
{
#ifdef _WIN32
	return (long)GetCurrentProcessId();
#else
	return (long)getpid();
#endif
}
