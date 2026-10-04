/* os9link.c: argument vectors, running commands, and linking a program into
 * an OS-9 module with elf2mod; see os9link.h.
 *
 * MIT License; see elf2mod.c.
 */

#include "os9link.h"
#include "hostsys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libiberty.h>

int os9_show, os9_dry_run;

void args_add(struct args *a, const char *arg)
{
	if (a->n + 2 > a->size) {
		a->size = a->size ? 2 * a->size : 16;
		a->v = xrealloc(a->v, a->size * sizeof *a->v);
	}
	a->v[a->n++] = xstrdup(arg);
	a->v[a->n] = NULL;
}

void args_addf(struct args *a, const char *format, ...)
{
	va_list ap;
	char *arg;

	va_start(ap, format);
	if (vasprintf(&arg, format, ap) < 0)
		arg = NULL;
	va_end(ap);
	if (arg == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	args_add(a, arg);
	free(arg);
}

void args_append(struct args *a, const struct args *more)
{
	for (int i = 0; i < more->n; i++)
		args_add(a, more->v[i]);
}

void args_free(struct args *a)
{
	for (int i = 0; i < a->n; i++)
		free(a->v[i]);
	free(a->v);
	a->v = NULL;
	a->n = a->size = 0;
}

/* Shows ARGV as C 3.2's xcc -bp does: the command line in quotes. */
static void show(const struct args *argv)
{
	putchar('"');
	for (int i = 0; i < argv->n; i++)
		printf(i ? " %s" : "%s", argv->v[i]);
	puts("\"");
	fflush(stdout);
}

int os9_run(const struct args *argv, const char *out, const char *err)
{
	if (os9_show || os9_dry_run)
		show(argv);
	if (os9_dry_run)
		return 0;
	return host_run((const char *const *)argv->v, out, err);
}

static struct args temps;
static char *dir;

void os9_set_tempdir(const char *d)
{
	free(dir);
	dir = host_abspath(d);
}

char *os9_temp(const char *suffix)
{
	static int count;

	if (dir == NULL)
		dir = host_tempdir();
	char *path = xmalloc(strlen(dir) + strlen(suffix) + 64);
	sprintf(path, "%s/m68k-os9-%ld-%d%s", dir, host_pid(), ++count, suffix);
	args_add(&temps, path);
	return path;
}

void os9_cleanup(void)
{
	for (int i = 0; i < temps.n; i++)
		remove(temps.v[i]);
	args_free(&temps);
}

/* Copies file PATH to standard error. */
static void cat_stderr(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return;
	char line[1024];
	while (fgets(line, sizeof line, fp) != NULL)
		fputs(line, stderr);
	fclose(fp);
}

/* Whether the linker's messages in PATH are all about truncated relocations
 * (code too large for 16-bit calls), and there are some. */
static int only_truncated(const char *path)
{
	static const char *const harmless[] = {
		"relocation truncated to fit", "additional relocation overflows omitted",
		": in function", "DWARF error", "ld returned 1 exit status",
	};
	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return 0;
	char line[1024];
	int truncated = 0, other = 0;
	while (fgets(line, sizeof line, fp) != NULL) {
		size_t i;
		for (i = 0; i < sizeof harmless / sizeof harmless[0]; i++)
			if (strstr(line, harmless[i]) != NULL)
				break;
		if (i == 0)
			truncated = 1;
		else if (i == sizeof harmless / sizeof harmless[0] && line[strspn(line, " \t\r\n")] != '\0')
			other = 1;
	}
	fclose(fp);
	return truncated && !other;
}

/* Links with the jump table size TABLE (in bytes; -1 for a link that must
 * succeed as it is). ERR gets the linker's messages. */
static int link_elf(struct os9link *l, long table, const char *err)
{
	struct args argv = { 0 };
	args_add(&argv, l->gcc);
	args_append(&argv, &l->link);
	args_add(&argv, "-o");
	args_add(&argv, l->elf);
	if (table >= 0) {
		args_add(&argv, "-Wl,--noinhibit-exec");
		args_addf(&argv, "-Wl,--defsym,__jmptbl_size=%ld", table);
	}
	int status = os9_run(&argv, NULL, err);
	args_free(&argv);
	return status;
}

/* Runs elf2mod on L->elf; OUT, if not null, gets its output. */
static int make_module(struct os9link *l, const char *out)
{
	struct args argv = { 0 };
	args_add(&argv, l->elf2mod);
	args_add(&argv, "-n");
	args_add(&argv, l->name);
	args_append(&argv, &l->mod);
	args_add(&argv, l->elf);
	args_add(&argv, l->module);
	int status = os9_run(&argv, out, NULL);
	args_free(&argv);
	return status;
}

/* The jump table size, in bytes, that elf2mod asks for in its output PATH:
 * "N far calls need a jump table of M entries (B bytes)"; -1 if none. */
static long table_size(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return -1;
	char line[1024];
	long bytes = -1;
	while (bytes < 0 && fgets(line, sizeof line, fp) != NULL) {
		const char *p = strstr(line, "jump table of ");
		long entries;
		if (p != NULL && sscanf(p, "jump table of %ld entries (%ld bytes)", &entries, &bytes) != 2)
			bytes = -1;
	}
	fclose(fp);
	return bytes;
}

int os9_link(struct os9link *l)
{
	char *log = os9_temp(".log");
	int status = link_elf(l, -1, log);
	if (os9_dry_run)
		return make_module(l, NULL);
	if (status == 0) {
		cat_stderr(log);
		return make_module(l, NULL);
	}
	if (!only_truncated(log)) {
		cat_stderr(log);
		return status;
	}
	if (l->no_jmptbl) {
		fprintf(stderr, "%s: the code is too large for 16-bit calls and needs a jump table, "
			"which -j forbids\n", l->who);
		return 1;
	}

	/* Link anyway, and let elf2mod say how large a table it needs. */
	char *out = os9_temp(".out");
	link_elf(l, 0, os9_temp(".log"));
	make_module(l, out);
	long table = table_size(out);
	if (table < 0) {
		cat_stderr(log);
		return status;
	}
	if (link_elf(l, table, os9_temp(".log")) != 0) {
		cat_stderr(log);
		return status;
	}
	return make_module(l, NULL);
}
