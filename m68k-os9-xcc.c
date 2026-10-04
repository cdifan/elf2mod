/* m68k-os9-xcc: Microware C 3.2's xcc/cc options, run with GCC.
 *
 * A drop-in for makefiles written for Microware C 3.2's xcc (and Ultra C's
 * xcc -mode=compat): the same options, carried out by m68k-elfos9-gcc,
 * rof2elf, GNU ld and elf2mod, with the user's own copies of Microware's
 * cstart.r and libraries (CLIB) and headers (CDEF), converted on demand.
 * See m68k-os9-xcc.md. m68k-os9-ucc is the same program for Ultra C's own
 * options (-mode=ucc), not implemented yet.
 *
 * MIT License; see elf2mod.c.
 */

#include "hostsys.h"
#include "os9link.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libiberty.h>

#define GCC	"m68k-elfos9-gcc"
#define NM	"m68k-elfos9-nm"
#define RANLIB	"m68k-elfos9-ranlib"
#define ROF2ELF	"rof2elf"
#define ELF2ROF	"elf2rof"
#define ELF2MOD	"elf2mod"

static const char *who = "m68k-os9-xcc";

/* Case-insensitive comparisons (strcasecmp isn't everywhere). */
static int ci_eq(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return *a == *b;
}

static int ci_prefix(const char *s, const char *prefix)
{
	for (; *prefix != '\0'; s++, prefix++)
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return 0;
	return 1;
}

/* ----- Options ----- */

enum kind {
	FLAG,		/* -x */
	VALUE,		/* -e=5, also -e5 */
	JOIN,		/* -dNAME, also -d=NAME */
	OPTVALUE,	/* -r, -r=DIR */
};

enum id {
	O_A, O_BG, O_BP, O_C, O_CS, O_D, O_E, O_F, O_FD, O_G, O_H, O_I, O_J, O_K,
	O_L, O_LO, O_M, O_N, O_NL, O_NV, O_O, O_Q, O_R, O_S, O_T, O_TQ, O_TO, O_TP,
	O_U, O_V, O_W, O_X, O_Z,
};

#define COMPAT	1	/* C 3.2's options (Ultra C's -mode=compat) */
#define UCOMPAT	2	/* only in Ultra C's -mode=compat */

struct option {
	const char *name;	/* lowercase, without the '-' */
	enum kind kind;
	const char *arg;	/* the value, in the usage */
	const char *help;	/* Microware's description */
	const char *does;	/* what this program does, when that differs */
	enum id id;
	int modes;
};

/* In the order of C 3.2's usage; the usage is made from this table. */
static const struct option options[] = {
	{ "a", FLAG, "", "Stop after compiling; write assembler source", "GNU as syntax, not r68", O_A, COMPAT },
	{ "bg", FLAG, "", "Make a sticky (adhesive) module", "not supported", O_BG, COMPAT },
	{ "bp", FLAG, "", "Print each command before running it", NULL, O_BP, COMPAT },
	{ "c", FLAG, "", "Comments in assembler output", "ignored", O_C, COMPAT },
	{ "cs", VALUE, "=<name>", "Link with this root psect instead of cstart.r", NULL, O_CS, COMPAT },
	{ "d", JOIN, "<name>", "Define a preprocessor macro", NULL, O_D, COMPAT },
	{ "e", VALUE, "=<n>", "Module edition", NULL, O_E, COMPAT },
	{ "f", VALUE, "=<path>", "Output file (in execution directory)", "path as given", O_F, COMPAT },
	{ "fd", VALUE, "=<path>", "Output file (in data directory)", "path as given", O_FD, COMPAT },
	{ "g", FLAG, "", "Generate source-level debug information", "elf2mod -g writes .stb, no .dbg", O_G, COMPAT },
	{ "h", FLAG, "", "Print commands without running them", NULL, O_H, UCOMPAT },
	{ "i", FLAG, "", "Link with shared C I/O module (cio)", "links cio.l before C library", O_I, COMPAT },
	{ "j", FLAG, "", "No jump table for far calls", "fails if program needs one", O_J, COMPAT },
	{ "k", VALUE, "=<n>[W|L][CW|CL][F]", "Processor: 0 for 68000, 2 for 68020; W/L for word/long data offsets,\n"
		"CW/CL for word/long code offsets; F for 68881", "L and CL: unsupported", O_K, COMPAT },
	{ "l", VALUE, "=<name>", "Also search this library", NULL, O_L, COMPAT },
	{ "lo", VALUE, "=<opts>", "Pass options on to the linker", "not supported", O_LO, COMPAT },
	{ "m", VALUE, "=<n>[K]", "Extra stack for module, in KB", "in K with or without K, as l68's -M=", O_M, COMPAT },
	{ "n", VALUE, "=<name>", "Module name (default: unsuffixed output file name)", NULL, O_N, COMPAT },
	{ "nl", FLAG, "", "Don't link default libraries", NULL, O_NL, COMPAT },
	{ "nv", FLAG, "", "No vsect directives (OS-9000 only)", "ignored", O_NV, COMPAT },
	{ "o", OPTVALUE, "[=<n>]", "Optimization level, 0 to 2", "-O<n>; -O2 by default", O_O, COMPAT },
	{ "q", FLAG, "", "No progress messages", NULL, O_Q, COMPAT },
	{ "r", OPTVALUE, "[=<dir>]", "Stop at object files (.r), in <dir> if given", "ELF in .r files; see -obj", O_R, COMPAT },
	{ "s", FLAG, "", "Leave out stack checks", NULL, O_S, COMPAT },
	{ "t", VALUE, "=<dir>", "Put temporary files in <dir>", NULL, O_T, COMPAT },
	{ "t?", FLAG, "", "List target systems and processors", NULL, O_TQ, COMPAT },
	{ "to", VALUE, "=<name>", "Target operating system", "OSK only", O_TO, COMPAT },
	{ "tp", VALUE, "=<n>[..]", "Target processor, with options", "68k, 020 (with 68881), cpu32", O_TP, COMPAT },
	{ "u", JOIN, "<name>", "Undefine a predefined macro", NULL, O_U, COMPAT },
	{ "v", VALUE, "=<dir>", "Also search <dir> for #include files", "before CDEF", O_V, COMPAT },
	{ "w", VALUE, "=<dir>", "Directory of default libraries", "replaces CLIB", O_W, COMPAT },
	{ "x", FLAG, "", "Floating point through math trap module", "-mos9math", O_X, COMPAT },
	{ "z", OPTVALUE, "[=<file>]", "Read options and file names from <file>, or standard input", NULL, O_Z, COMPAT },
};
#define NOPTIONS (sizeof options / sizeof options[0])

/* ----- State ----- */

enum save { SAVE_BESIDE, SAVE_SUBDIR, SAVE_CACHE };

static struct {
	/* Compiling. */
	struct args flags;	/* -D/-U/-I, in order */
	int opt;		/* -O level */
	int debug, nostack, math, comments;
	int cpu;		/* 0, 2, 32 for the CPU32 */
	int fpu;		/* the 68881 */
	struct args vdirs;	/* -v */

	/* What to do. */
	int asm_only, rel_only, quiet;
	const char *reldir;

	/* Linking. */
	const char *root;	/* -cs */
	const char *out;	/* -f, -fd */
	const char *name;	/* -n */
	const char *edition;	/* -e */
	long stack;		/* -m, in bytes */
	int cio, nojmptbl, nolibs;
	const char *libdir;	/* -w */
	struct args libs;	/* -l */

	/* Files, in order. */
	struct args files;

	/* Converted files. */
	enum save save;
	const char *elfdir, *cache;
	int rof;		/* -obj=rof */
} st = { .opt = 2, .elfdir = "m68k-os9-elf" };

static void error(const char *format, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", who);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
	os9_cleanup();
	exit(1);
}

static void note(const char *format, ...)
{
	va_list ap;

	if (st.quiet)
		return;
	fprintf(stderr, "%s: ", who);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ----- Usage ----- */

/* One entry of the usage: LEFT in a column, then TEXT; when LEFT doesn't
 * fit, TEXT goes on the next line. A line break in TEXT continues under
 * the text column. */
static void usage_line(const char *left, const char *text)
{
	enum { COLUMN = 18 };
	if (strlen(left) < COLUMN)
		printf("   %-*s ", COLUMN, left);
	else
		printf("   %s\n   %-*s ", left, COLUMN, "");
	for (const char *nl; (nl = strchr(text, '\n')) != NULL; text = nl + 1)
		printf("%.*s\n   %-*s ", (int)(nl - text), text, COLUMN, "");
	printf("%s\n", text);
}

static void usage(void)
{
	printf("%s [opts] [files] [opts]\n", who);
	printf("Options of Microware C 3.2's xcc/cc (Ultra C's -mode=compat), run with GCC:\n");
	for (size_t i = 0; i < NOPTIONS; i++) {
		const struct option *o = &options[i];
		char left[64];
		snprintf(left, sizeof left, "-%s%s", o->name, o->arg);
		for (char *p = left + 1; *p != '\0' && *p != '=' && *p != '<' && *p != '['; p++)
			*p = toupper((unsigned char)*p);
		/* What the option does here, when that differs, in square brackets. */
		char *text = concat(o->help, o->modes == UCOMPAT ? " (Ultra C)" : "",
			o->does != NULL ? " [" : "", o->does != NULL ? o->does : "",
			o->does != NULL ? "]" : "", NULL);
		usage_line(left, text);
		free(text);
	}
	printf("Options of this program:\n");
	usage_line("-mode=<mode>", "compat (C 3.2, the default here) or ucc (m68k-os9-ucc; not supported yet)");
	usage_line("-elf-save=<s>", "where files converted to ELF go: beside (default), subdir, cache");
	usage_line("-elf-dir=<name>", "subdirectory, and directory in cache (default m68k-os9-elf)");
	usage_line("-elf-cache=<dir>", "cache directory (default $XDG_CACHE_HOME or ~/.cache)");
	usage_line("-obj=<kind>", "what -r writes into .r files: elf (default) or rof (for Microware's l68)");
	printf("Files, by suffix, in any case:\n");
	usage_line(".c", "C source");
	usage_line(".r, .o", "object, ROF or ELF (ROF is converted; see -elf-save=)");
	usage_line(".l", "library: ROFs or Ultra C's libgen format (converted), or an ar archive");
	usage_line(".a", "r68 assembler source: not supported yet");
	printf("Environment:\n");
	usage_line("CDEF", "Microware's header directory");
	usage_line("CLIB", "Microware's library directory: cstart.r, clib.l ...");
	usage_line("M68K_OS9_ELF", "-elf-.../-obj= settings as save=...,dir=...,cache=...,obj=...");
	usage_line("GRPUSER", "module owner, for elf2mod: GROUP.USER, decimal (default 0.0)");
}

static void targets(void)
{
	printf("Target OS:\n   OSK            OS-9/68K\n");
	printf("Target processors (-tp=, -k=):\n");
	printf("   68k            68000/68010/68070 (-k=0)\n");
	printf("   020            68020/68030, with the 68881 (-k=2f; -k=2 without)\n");
	printf("   cpu32          CPU32\n");
}

/* ----- Settings for converted files ----- */

static void set_save(const char *value)
{
	if (ci_eq(value, "beside"))
		st.save = SAVE_BESIDE;
	else if (ci_eq(value, "subdir"))
		st.save = SAVE_SUBDIR;
	else if (ci_eq(value, "cache"))
		st.save = SAVE_CACHE;
	else
		error("unknown -elf-save=%s (beside, subdir or cache)", value);
}

static void set_obj(const char *value)
{
	if (ci_eq(value, "elf"))
		st.rof = 0;
	else if (ci_eq(value, "rof"))
		st.rof = 1;
	else
		error("unknown -obj=%s (elf or rof)", value);
}

/* M68K_OS9_ELF: save=...,dir=...,cache=...,obj=... */
static void elf_environment(void)
{
	const char *env = getenv("M68K_OS9_ELF");
	if (env == NULL || *env == '\0')
		return;
	char *copy = xstrdup(env), *next;
	for (char *item = copy; item != NULL; item = next) {
		next = strchr(item, ',');
		if (next != NULL)
			*next++ = '\0';
		if (*item == '\0')
			continue;
		char *value = strchr(item, '=');
		if (value == NULL)
			error("M68K_OS9_ELF: %s: expected name=value", item);
		*value++ = '\0';
		if (ci_eq(item, "save"))
			set_save(value);
		else if (ci_eq(item, "dir"))
			st.elfdir = xstrdup(value);
		else if (ci_eq(item, "cache"))
			st.cache = xstrdup(value);
		else if (ci_eq(item, "obj"))
			set_obj(value);
		else
			error("M68K_OS9_ELF: unknown setting %s", item);
	}
	free(copy);
}

/* ----- Parsing ----- */

static void parse(int argc, char **argv);

/* -k=<n>[W|L][CW|CL][F] */
static void set_k(const char *value)
{
	const char *p = value;
	if (*p == '0')
		st.cpu = 0;
	else if (*p == '2')
		st.cpu = 2;
	else
		error("-k=%s: the target must be 0 or 2", value);
	p++;
	st.fpu = 0;
	while (*p != '\0') {
		char c = tolower((unsigned char)*p);
		if (c == 'c') {
			char d = tolower((unsigned char)p[1]);
			if (d == 'l')
				error("-k=%s: long code offsets aren't supported", value);
			if (d != 'w')
				error("-k=%s: bad code offset size", value);
			p += 2;
		} else if (c == 'l') {
			error("-k=%s: long data offsets aren't supported", value);
		} else if (c == 'w') {
			p++;
		} else if (c == 'f') {
			if (st.cpu != 2)
				error("-k=%s: the 68881 needs the 68020", value);
			st.fpu = 1;
			p++;
		} else {
			error("-k=%s: unknown option %c", value, *p);
		}
	}
}

/* -tp=68k, 020, cpu32 (C 3.2's -tp=020 brings the 68881). */
static void set_tp(const char *value)
{
	if (ci_prefix(value, "68k")) {
		st.cpu = 0;
		st.fpu = 0;
		value += 3;
	} else if (ci_prefix(value, "020")) {
		st.cpu = 2;
		st.fpu = 1;
		value += 3;
	} else if (ci_prefix(value, "cpu32")) {
		st.cpu = 32;
		st.fpu = 0;
		value += 5;
	} else {
		error("-tp=%s: unknown processor (68k, 020 or cpu32)", value);
	}
	for (; *value != '\0'; value++) {
		char c = tolower((unsigned char)*value);
		if (c == 'f' && st.cpu == 2)
			st.fpu = 1;
		else if (c == 'l')
			error("-tp: long offsets aren't supported");
		else if (c != 'w' && c != 'c' && c != 'd')
			error("-tp: unknown option %c", *value);
	}
}

/* -z[=FILE]: options and files from FILE, or standard input. */
static void read_z(const char *path)
{
	FILE *fp = path != NULL ? fopen(path, "r") : stdin;
	if (fp == NULL)
		error("cannot read %s", path);
	struct args words = { 0 };
	args_add(&words, who);
	char word[1024];
	while (fscanf(fp, "%1023s", word) == 1)
		args_add(&words, word);
	if (fp != stdin)
		fclose(fp);
	parse(words.n, words.v);
	args_free(&words);
}

/* Sets option O with VALUE (null without one). */
static void apply(const struct option *o, const char *value)
{
	switch (o->id) {
	case O_A: st.asm_only = 1; break;
	case O_BG: error("-bg (adhesive modules) isn't supported"); break;
	case O_BP: os9_show = 1; break;
	case O_C: st.comments = 1; break;
	case O_CS: st.root = value; break;
	case O_D: args_addf(&st.flags, "-D%s", value); break;
	case O_E: st.edition = value; break;
	case O_F: case O_FD: st.out = value; break;
	case O_G: st.debug = 1; break;
	case O_H: os9_dry_run = 1; break;
	case O_I: st.cio = 1; break;
	case O_J: st.nojmptbl = 1; break;
	case O_K: set_k(value); break;
	case O_L: args_add(&st.libs, value); break;
	case O_LO: error("-lo=%s: l68 options aren't supported", value); break;
	case O_M: {
		char *end;
		long n = strtol(value, &end, 0);
		if (end == value || (*end != '\0' && !ci_eq(end, "k")) || n < 0)
			error("-m=%s: expected a size in K", value);
		/* In K, with or without the K, as l68's -M= takes it. */
		st.stack = n * 1024;
		break;
	}
	case O_N: st.name = value; break;
	case O_NL: st.nolibs = 1; break;
	case O_NV: break;
	case O_O:
		if (value == NULL)
			st.opt = 2;
		else if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0 || strcmp(value, "2") == 0)
			st.opt = *value - '0';
		else
			error("-o=%s: the level must be 0, 1 or 2", value);
		break;
	case O_Q: st.quiet = 1; break;
	case O_R: st.rel_only = 1; st.reldir = value; break;
	case O_S: st.nostack = 1; break;
	case O_T: os9_set_tempdir(value); break;
	case O_TQ: targets(); os9_cleanup(); exit(0);
	case O_TO:
		if (!ci_eq(value, "osk"))
			error("-to=%s: only OSK (OS-9/68K) is supported", value);
		break;
	case O_TP: set_tp(value); break;
	case O_U: args_addf(&st.flags, "-U%s", value); break;
	case O_V: args_add(&st.vdirs, value); break;
	case O_W: st.libdir = value; break;
	case O_X: st.math = 1; break;
	case O_Z: read_z(value); break;
	}
}

/* This program's own options; returns whether ARG was one. */
static int own_option(const char *arg)
{
	static const char *const names[] = { "mode=", "elf-save=", "elf-dir=", "elf-cache=", "obj=" };
	size_t i;
	for (i = 0; i < sizeof names / sizeof names[0]; i++)
		if (ci_prefix(arg + 1, names[i]))
			break;
	if (i == sizeof names / sizeof names[0])
		return 0;
	const char *value = arg + 1 + strlen(names[i]);
	switch (i) {
	case 0:
		if (ci_eq(value, "ucc") || ci_eq(value, "c89"))
			error("-mode=%s: Ultra C's own options aren't implemented yet", value);
		if (!ci_eq(value, "compat"))
			error("unknown -mode=%s (compat, c89 or ucc)", value);
		break;
	case 1: set_save(value); break;
	case 2: st.elfdir = value; break;
	case 3: st.cache = value; break;
	case 4: set_obj(value); break;
	}
	return 1;
}

/* Matches option ARG (with its '-'); the longest name that fits wins. */
static void option(const char *arg)
{
	const char *text = arg + 1;
	const struct option *best = NULL;
	const char *value = NULL, *candidate;
	size_t bestlen = 0;

	if (strcmp(text, "?") == 0) {
		usage();
		os9_cleanup();
		exit(0);
	}
	if (own_option(arg))
		return;

	for (size_t i = 0; i < NOPTIONS; i++) {
		const struct option *o = &options[i];
		size_t len = strlen(o->name);
		if (len <= bestlen || !ci_prefix(text, o->name))
			continue;
		const char *rest = text + len;
		switch (o->kind) {
		case FLAG:
			if (*rest != '\0')
				continue;
			candidate = NULL;
			break;
		case VALUE:
		case JOIN:
			if (*rest == '=')
				rest++;
			if (*rest == '\0')
				continue;
			candidate = rest;
			break;
		case OPTVALUE:
		default:
			candidate = *rest == '=' ? rest + 1 : *rest != '\0' ? rest : NULL;
			if (candidate != NULL && *candidate == '\0')
				candidate = NULL;
			break;
		}
		best = o;
		bestlen = len;
		value = candidate;
	}
	if (best == NULL)
		error("unknown option %s (%s -? for the usage)", arg, who);
	apply(best, value);
}

static void parse(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] != '\0')
			option(argv[i]);
		else
			args_add(&st.files, argv[i]);
	}
}

/* ----- Paths ----- */

static const char *suffix(const char *path)
{
	const char *base = lbasename(path);
	const char *dot = strrchr(base, '.');
	return dot != NULL ? dot : base + strlen(base);
}

/* PATH's base name without its suffix (allocated); lowercase if LOWER. */
static char *stem(const char *path, int lower)
{
	const char *base = lbasename(path);
	const char *dot = strrchr(base, '.');
	size_t length = dot != NULL ? (size_t)(dot - base) : strlen(base);
	char *s = xmalloc(length + 1);
	memcpy(s, base, length);
	s[length] = '\0';
	if (lower)
		for (char *p = s; *p != '\0'; p++)
			*p = tolower((unsigned char)*p);
	return s;
}

/* The directory part of absolute path PATH (allocated). */
static char *dirpart(const char *path)
{
	const char *base = lbasename(path);
	size_t length = base - path;
	while (length > 1 && host_is_sep(path[length - 1]))
		length--;
	char *d = xmalloc(length + 1);
	memcpy(d, path, length);
	d[length] = '\0';
	return d;
}

/* Whether file PATH starts with BYTES. */
static int starts_with(const char *path, const char *bytes, size_t length)
{
	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
		return 0;
	char buffer[16];
	size_t got = fread(buffer, 1, length, fp);
	fclose(fp);
	return got == length && memcmp(buffer, bytes, length) == 0;
}

/* ----- Converting Microware files ----- */

/* The cache's directory for absolute directory DIR (allocated). */
static char *cache_dir(const char *dir)
{
	const char *root = st.cache;
	char *home = NULL;
	if (root == NULL) {
		const char *xdg = getenv("XDG_CACHE_HOME");
		if (xdg != NULL && *xdg != '\0')
			root = xdg;
		else {
			const char *h = getenv("HOME");
			if (h == NULL)
				h = getenv("USERPROFILE");
			if (h == NULL)
				h = ".";
			root = home = concat(h, "/.cache", NULL);
		}
	}
	/* The directory's path below it, without a drive's colon. */
	char *below = xstrdup(dir);
	for (char *p = below; *p != '\0'; p++)
		if (*p == ':')
			*p = '_';
	const char *start = below;
	while (host_is_sep(*start))
		start++;
	char *path = concat(root, "/", st.elfdir, "/", start, NULL);
	free(below);
	free(home);
	return path;
}

/* Where the ELF form of ROF object or library INPUT goes (allocated). */
static char *converted_path(const char *input, int library)
{
	char *abs = host_abspath(input);
	char *dir = dirpart(abs);
	char *name = stem(abs, 1);
	char *file = library ? concat(st.save == SAVE_BESIDE ? "lib" : "", name, ".a", NULL)
		: concat(name, ".o", NULL);
	char *target = NULL;

	if (st.save == SAVE_BESIDE && host_writable_dir(dir)) {
		target = concat(dir, "/", file, NULL);
		/* A ROF object some makefile named .o would be its own target. */
		if (ci_eq(target, abs)) {
			free(target);
			target = NULL;
		}
	}
	else if (st.save == SAVE_SUBDIR) {
		char *sub = concat(dir, "/", st.elfdir, NULL);
		if ((host_mtime(sub) != (time_t)-1 || (host_writable_dir(dir) && host_mkdirs(sub) == 0)) &&
			host_writable_dir(sub))
			target = concat(sub, "/", file, NULL);
		free(sub);
	}
	if (target == NULL) {
		char *cache = cache_dir(dir);
		if (st.save == SAVE_SUBDIR || (st.save == SAVE_BESIDE && !host_writable_dir(dir)))
			note("%s isn't writable; converting %s into %s", dir, lbasename(input), cache);
		if (host_mkdirs(cache) != 0)
			error("cannot create %s", cache);
		if (library && st.save == SAVE_BESIDE) {
			free(file);
			file = concat(name, ".a", NULL);
		}
		target = concat(cache, "/", file, NULL);
		free(cache);
	}
	free(abs);
	free(dir);
	free(name);
	free(file);
	return target;
}

/* The library directory's sys.l, whose equates rof2elf folds into the code
 * (-e); null if there's no library directory or no sys.l in it. */
static const char *sys_l(void)
{
	static int looked;
	static char *path;

	if (!looked) {
		looked = 1;
		const char *dir = st.libdir != NULL ? st.libdir : getenv("CLIB");
		if (dir != NULL && *dir != '\0')
			path = host_find_nocase(dir, "sys.l");
	}
	return path;
}

/* INPUT as ELF: itself if it is (an object, or an ar archive), else its
 * conversion, made or remade when older than INPUT or sys.l (allocated). */
static char *as_elf(const char *input, int library)
{
	if (host_mtime(input) == (time_t)-1)
		error("cannot find %s", input);
	if (starts_with(input, "\177ELF", 4) || starts_with(input, "!<arch>\n", 8))
		return xstrdup(input);

	char *target = converted_path(input, library);
	time_t made = host_mtime(target);
	if (made != (time_t)-1 && made >= host_mtime(input) &&
		(sys_l() == NULL || made >= host_mtime(sys_l())))
		return target;

	/* Convert into a temporary file beside the target, then rename it, so
	 * that parallel runs never see half a file. */
	char *temp = xmalloc(strlen(target) + 32);
	sprintf(temp, "%s.%ld.tmp", target, host_pid());
	struct args argv = { 0 };
	args_add(&argv, ROF2ELF);
	if (sys_l() != NULL) {
		args_add(&argv, "-e");
		args_add(&argv, sys_l());
	}
	args_add(&argv, "-o");
	args_add(&argv, temp);
	args_add(&argv, input);
	int status = os9_run(&argv, NULL, NULL);
	args_free(&argv);
	if (status == 0 && library) {
		args_add(&argv, RANLIB);
		args_add(&argv, temp);
		status = os9_run(&argv, NULL, NULL);
		args_free(&argv);
	}
	if (!os9_dry_run && (status != 0 || host_replace(temp, target) != 0)) {
		remove(temp);
		error("cannot convert %s", input);
	}
	free(temp);
	return target;
}

/* ----- Microware's libraries ----- */

/* The library directory: -w, else CLIB. */
static const char *libdir(void)
{
	const char *dir = st.libdir != NULL ? st.libdir : getenv("CLIB");
	if (dir == NULL || *dir == '\0')
		error("CLIB isn't set: it names the directory of Microware's cstart.r and libraries");
	return dir;
}

/* NAME (e.g. "clib.l") in the library directory, regardless of case
 * (allocated). */
static char *libfile(const char *name)
{
	char *path = host_find_nocase(libdir(), name);
	if (path == NULL)
		error("cannot find %s in %s", name, libdir());
	return path;
}

/* The default libraries, as C 3.2's xcc picks them. */
static void default_libraries(struct args *libs)
{
	const char *clib, *math;
	if (st.cpu == 2 && st.math)
		clib = "clib020.l", math = NULL;
	else if (st.cpu == 2 && st.fpu)
		clib = "clib020h.l", math = "math881.l";
	else if (st.cpu == 2)
		clib = "clib020n.l", math = "math.l";
	else if (st.math)
		clib = "clib.l", math = NULL;
	else
		clib = "clibn.l", math = "math.l";

	if (st.cio)
		args_add(libs, libfile("cio.l"));
	args_add(libs, libfile(clib));
	if (math != NULL)
		args_add(libs, libfile(math));
	args_add(libs, libfile("sys.l"));
}

/* ----- Compiling ----- */

/* GCC's options for C 3.2's code generation. */
static void compile_flags(struct args *a)
{
	args_add(a, "-mpcrel");
	args_add(a, "-ma6rel");
	args_add(a, "-mos9call");
	args_add(a, "-mbuiltin=os9call");
	args_add(a, "-mos9newline");
	args_add(a, "-std=gnu89");
	args_add(a, "-w");
	if (!st.nostack)
		args_add(a, "-mos9stkchk");
	if (st.cpu == 2)
		args_add(a, "-m68020");
	else if (st.cpu == 32)
		args_add(a, "-mcpu=cpu32");
	else
		args_add(a, "-m68000");
	if (st.fpu)
		args_add(a, "-m68881");
	else
		args_add(a, "-msoft-float");
	if (st.math && !st.fpu)
		args_add(a, "-mos9math");
	args_addf(a, "-O%d", st.opt);
	if (st.debug)
		args_add(a, "-g");
	if (st.comments && st.asm_only)
		args_add(a, "-fverbose-asm");
	args_append(a, &st.flags);
	for (int i = 0; i < st.vdirs.n; i++)
		args_addf(a, "-I%s", st.vdirs.v[i]);
	const char *cdef = getenv("CDEF");
	if (cdef != NULL && *cdef != '\0')
		args_addf(a, "-I%s", cdef);
}

/* Compiles C file SOURCE: to OUTPUT, as assembler (-a), an object, or a
 * ROF (-obj=rof). */
static void compile(const char *source, const char *output, int assembler, int rof)
{
	struct args argv = { 0 };
	args_add(&argv, GCC);
	compile_flags(&argv);
	args_add(&argv, assembler ? "-S" : "-c");
	args_add(&argv, "-o");
	char *object = rof ? os9_temp(".o") : (char *)output;
	args_add(&argv, object);
	args_add(&argv, source);
	if (!st.quiet && !os9_show)
		printf("'%s'\n", source);
	if (os9_run(&argv, NULL, NULL) != 0)
		error("compiling %s failed", source);
	args_free(&argv);

	if (rof) {
		char *name = stem(source, 0);
		args_add(&argv, ELF2ROF);
		args_add(&argv, "-n");
		args_addf(&argv, "%s_c", name);
		args_add(&argv, "-o");
		args_add(&argv, output);
		args_add(&argv, object);
		free(name);
		if (os9_run(&argv, NULL, NULL) != 0)
			error("converting %s to ROF failed", source);
		args_free(&argv);
	}
}

/* ----- Linking ----- */

/* The linker script, laid out as elf2mod expects (as test/l68cmp/mod.lds):
 * code above the data area, data a6-relative from -0x8000, and elf2mod's
 * jump table (--defsym __jmptbl_size=N). */
static const char script[] =
	"OUTPUT_FORMAT(\"elf32-m68k\")\n"
	"OUTPUT_ARCH(m68k)\n"
	"PROVIDE (__jmptbl_size = 0);\n"
	"SECTIONS {\n"
	"    . = 0x400000;\n"
	"    .text : { *(.text .text.* .rodata .rodata.*) . = ALIGN(2); }\n"
	"    /* l68's symbols for the module's start, name and end: elf2mod sets\n"
	"       every reference to them, so these values are only placeholders. */\n"
	"    PROVIDE (btext = ADDR(.text)); PROVIDE (_btext = ADDR(.text));\n"
	"    PROVIDE (bname = ADDR(.text)); PROVIDE (_bname = ADDR(.text));\n"
	"    PROVIDE (etext = ADDR(.text)); PROVIDE (_etext = ADDR(.text));\n"
	"    . = -0x8000;\n"
	"    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) . = ALIGN(2); }\n"
	"    .data : { *(.data .data.*) . = ALIGN(2); _jmptbl = .; . += __jmptbl_size; _ejmptbl = .; }\n"
	"    .remote.data : { *(.remote.data .remote.data.*) . = ALIGN(2); }\n"
	"    .remote.bss (NOLOAD) : { *(.remote.bss .remote.bss.*) . = ALIGN(2); }\n"
	"    end = .;\n"
	"    PROVIDE (_enddata = .);\n"
	"}\n";

static char *write_script(void)
{
	char *path = os9_temp(".ld");
	FILE *fp = fopen(path, "w");
	if (fp == NULL || fputs(script, fp) < 0 || fclose(fp) != 0)
		error("cannot write %s", path);
	return path;
}

/* The stack size of the root psect's module header (__os9_stack), or 0. */
static long root_stack(const char *root)
{
	char *out = os9_temp(".nm");
	struct args argv = { 0 };
	args_add(&argv, NM);
	args_add(&argv, root);
	int status = os9_run(&argv, out, NULL);
	args_free(&argv);
	if (status != 0)
		return 0;
	FILE *fp = fopen(out, "r");
	if (fp == NULL)
		return 0;
	char line[256];
	long stack = 0;
	while (fgets(line, sizeof line, fp) != NULL) {
		unsigned long value;
		char type, name[128];
		if (sscanf(line, "%lx %c %127s", &value, &type, name) == 3 && strcmp(name, "__os9_stack") == 0)
			stack = (long)value;
	}
	fclose(fp);
	return stack;
}

/* Adds input file PATH to the link. The driver would take a .r file for a
 * Ratfor source, so whatever isn't .o or .a goes to the linker directly. */
static void add_input(struct args *link, const char *path)
{
	const char *ext = strrchr(lbasename(path), '.');
	if (ext == NULL || (strcmp(ext, ".o") != 0 && strcmp(ext, ".a") != 0))
		args_add(link, "-Xlinker");
	args_add(link, path);
}

static void link_module(struct args *objects, const char *output)
{
	struct os9link l = { .gcc = GCC, .elf2mod = ELF2MOD, .who = who };
	char *root = as_elf(st.root != NULL ? st.root : libfile("cstart.r"), 0);

	struct args libs = { 0 };
	for (int i = 0; i < st.libs.n; i++)
		args_add(&libs, st.libs.v[i]);
	if (!st.nolibs)
		default_libraries(&libs);

	args_add(&l.link, "-mos9call");
	args_add(&l.link, "-nostdlib");
	args_add(&l.link, "-Wl,-q");
	args_add(&l.link, "-Wl,--no-check-sections");
	args_add(&l.link, "-T");
	args_add(&l.link, write_script());
	add_input(&l.link, root);
	for (int i = 0; i < objects->n; i++)
		add_input(&l.link, objects->v[i]);
	args_add(&l.link, "-Wl,--start-group");
	for (int i = 0; i < libs.n; i++) {
		char *lib = as_elf(libs.v[i], 1);
		add_input(&l.link, lib);
		free(lib);
	}
	args_add(&l.link, "-lgcc");
	args_add(&l.link, "-Wl,--end-group");

	char *elf = concat(output, ".elf", NULL);
	char *name = st.name != NULL ? xstrdup(st.name) : stem(output, 0);
	l.elf = elf;
	l.module = output;
	l.name = name;
	l.no_jmptbl = st.nojmptbl;
	if (st.edition != NULL) {
		args_add(&l.mod, "-e");
		args_add(&l.mod, st.edition);
	}
	if (st.stack > 0) {
		args_add(&l.mod, "-s");
		args_addf(&l.mod, "%ld", root_stack(root) + st.stack);
	}
	if (st.debug)
		args_add(&l.mod, "-g");

	if (!st.quiet && !os9_show)
		printf("linking %s\n", output);
	if (os9_link(&l) != 0)
		error("linking %s failed", output);

	free(root);
	free(elf);
	free(name);
	args_free(&libs);
	args_free(&l.link);
	args_free(&l.mod);
}

/* ----- Main ----- */

int main(int argc, char **argv)
{
	const char *base = lbasename(argv[0]);
	if (strstr(base, "ucc") != NULL)
		error("Ultra C's own options (m68k-os9-ucc, -mode=ucc) aren't implemented yet");

	elf_environment();
	if (argc < 2) {
		usage();
		return 0;
	}
	parse(argc, argv);
	if (st.files.n == 0)
		error("no files (%s -? for the usage)", who);

	struct args objects = { 0 };
	const char *first = NULL;
	for (int i = 0; i < st.files.n; i++) {
		const char *file = st.files.v[i];
		const char *sfx = suffix(file);
		if (ci_eq(sfx, ".c")) {
			char *name = stem(file, 0);
			if (first == NULL)
				first = xstrdup(name);
			if (st.asm_only) {
				char *out = concat(name, ".a", NULL);
				compile(file, out, 1, 0);
				free(out);
			} else if (st.rel_only) {
				const char *dir = st.reldir != NULL ? st.reldir : ".";
				char *out = concat(dir, "/", name, ".r", NULL);
				compile(file, out, 0, st.rof);
				free(out);
			} else {
				char *out = os9_temp(".o");
				compile(file, out, 0, 0);
				args_add(&objects, out);
			}
			free(name);
		} else if (ci_eq(sfx, ".r") || ci_eq(sfx, ".o")) {
			/* ROF or ELF, whatever the suffix: some makefiles name
			 * ROF files .o. */
			if (first == NULL)
				first = stem(file, 0);
			if (!st.asm_only && !st.rel_only) {
				char *object = as_elf(file, 0);
				args_add(&objects, object);
				free(object);
			}
		} else if (ci_eq(sfx, ".l")) {
			args_add(&st.libs, file);
		} else if (ci_eq(sfx, ".a")) {
			error("%s: assembler sources (r68) aren't supported yet", file);
		} else {
			error("%s: no recognized suffix", file);
		}
	}

	if (!st.asm_only && !st.rel_only) {
		if (first == NULL && st.out == NULL)
			error("nothing to link");
		link_module(&objects, st.out != NULL ? st.out : first);
	}

	args_free(&objects);
	os9_cleanup();
	return 0;
}
