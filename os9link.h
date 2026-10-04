/* os9link.h: what m68k-os9-xcc and m68k-os9-gcc share: argument vectors,
 * running commands (shown or only printed), and linking a program into an
 * OS-9 module, with elf2mod's jump table when the code needs one.
 *
 * MIT License; see elf2mod.c.
 */

#ifndef OS9LINK_H
#define OS9LINK_H

/* A growing, null-terminated argument vector. */
struct args {
	char **v;
	int n, size;
};

void args_add(struct args *a, const char *arg);
void args_addf(struct args *a, const char *format, ...);
void args_append(struct args *a, const struct args *more);
void args_free(struct args *a);

/* How commands run: shown before they run (C 3.2's -bp), or only shown
 * (Ultra C's -h). */
extern int os9_show, os9_dry_run;

/* Runs ARGV (see host_run), showing it as asked. Returns its exit status. */
int os9_run(const struct args *argv, const char *out, const char *err);

/* A temporary file name, unique to this process, ending in SUFFIX
 * (allocated); removed by os9_cleanup. They go to DIR if set (C 3.2's -t=),
 * else to the host's temporary directory. */
char *os9_temp(const char *suffix);
void os9_set_tempdir(const char *dir);
void os9_cleanup(void);

struct os9link {
	const char *gcc;	/* the driver, e.g. m68k-elfos9-gcc */
	const char *elf2mod;
	struct args link;	/* the driver's link arguments, without -o */
	const char *elf;	/* the linked ELF file */
	const char *module;	/* the module made from it */
	const char *name;	/* its module name */
	struct args mod;	/* more elf2mod options (-e, -s, -g, ...) */
	int no_jmptbl;		/* fail rather than use a jump table */
	const char *who;	/* the program, for messages */
};

/* Links L->link into L->elf; when the code is too large for 16-bit calls
 * (the link fails with truncated relocations only), links again with as
 * large a jump table as elf2mod asks for. Then makes L->module. Returns 0,
 * or nonzero (reported). */
int os9_link(struct os9link *l);

#endif
