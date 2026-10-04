# m68k-os9-xcc and m68k-os9-ucc (design)

Status: C 3.2 mode implemented (`m68k-os9-xcc.c`, with `os9link.c` and `hostsys.c`) and tested
(see "Tests"); Ultra C mode and `.a` sources not yet.

Drop-in replacements for Microware's compiler drivers, for existing makefiles: they take the
options of Microware C 3.2's `xcc`/`cc` or of Ultra C's `xcc`, and run GCC (`m68k-elfos9-gcc`),
`rof2elf`, GNU ld and `elf2mod` instead. A makefile only needs `CC=`/`XCC=` changed.

- `m68k-os9-xcc`: Microware C 3.2's options (Ultra C's `-mode=compat`), the default.
- `m68k-os9-ucc`: Ultra C's own options (`-mode=ucc`).

One program, in C so that it builds natively for Windows too: it runs the other programs
through libiberty's `pex_one`, as GCC's driver does, and keeps what the host does differently
in `hostsys.c`. The name it's run as only sets the default mode, and `-mode=compat|c89|ucc`, as
in Ultra C's `xcc`, selects it explicitly, so `m68k-os9-xcc -mode=ucc` is `m68k-os9-ucc`. The
two option sets can't share a parser: their short options mostly mean different things
(`-a`, `-c`, `-e`, `-k`, `-m`, `-o`, `-r`, `-s`, `-t`, `-x`; see "Ultra C mode").

`m68k-os9-gcc` remains the wrapper with GCC's own options.

## Microware's files

Makefiles expect Microware's headers and libraries, which can't be shipped: the wrapper uses
the user's own copies, found as Microware's drivers find them, and converts the libraries and
objects with `rof2elf` as needed.

### Where they are found

In C 3.2 mode, as C 3.2's `xcc` does (checked with `xcc -bp`):

- headers: the `-v=<dir>` directories, in order, then `CDEF` (one directory, not a list;
  default `\os9c\defs`)
- libraries: `cstart.r` and the default libraries from `CLIB` (one directory; default
  `\os9c\lib`), which `-w=<dir>` replaces (the last one counts); `-l=<file>` adds libraries,
  as given, before the default ones
- names are looked up ignoring case (`CLIB.L`, `CSTART.R` on a DOS installation)

In Ultra C mode: `MWOS` or `-mw=<dir>` for the tree, the `-v=` directories, `-sl=` for more
library directories, `-w=` for the default libraries. Ultra C's own libraries are in
`libgen`'s format, which `rof2elf` doesn't read yet (see "Ultra C's libraries" below); until
it does, Ultra C mode links C 3.2's libraries (`CLIB`) or newlib.

`GRPUSER` goes to `elf2mod`, which takes the module's owner from it, as `l68` does.

### Converting them

Every Microware input is converted when there's no converted file or it's older than the
original: `.r` files on the command line, `-l=` libraries, and the default `cstart.r` and
libraries. Libraries are also indexed (`ranlib`). Each conversion writes a temporary file and
renames it, so parallel builds (`make -j`) never see half a file. An input that's already ELF
(see "Objects") isn't converted.

Where the converted files go:

| Setting | `DIR/cstart.r` → | `DIR/clib.l` → |
|---|---|---|
| `beside` (default) | `DIR/cstart.o` | `DIR/libclib.a` |
| `subdir` | `DIR/m68k-os9-elf/cstart.o` | `DIR/m68k-os9-elf/clib.a` |
| `cache` | `CACHE/m68k-os9-elf/DIR/cstart.o` | `CACHE/m68k-os9-elf/DIR/clib.a` |

`beside` uses names that can't clash with Microware's: Microware never uses `.o`, and the
`lib` prefix keeps a library clear of r68 assembler sources, which are `.a`; `lib<name>.a` also
lets GNU tools use the libraries directly (`-L$CLIB -lclib`). When the chosen place isn't
writable (a read-only SDK tree, say), the file goes to the cache, with a note. A ROF object
that a makefile already named `.o` would be its own `beside` target; it goes to the cache too.

### Settings

| Option | In `M68K_OS9_ELF` | Values | Default |
|---|---|---|---|
| `-elf-save=` | `save=` | `beside`, `subdir`, `cache` | `beside` |
| `-elf-dir=` | `dir=` | the subdirectory's name, also used in the cache | `m68k-os9-elf` |
| `-elf-cache=` | `cache=` | the cache's root | `${XDG_CACHE_HOME:-$HOME/.cache}` |
| `-obj=` | `obj=` | `elf`, `rof`: what `-r` writes into `.r` files | `elf` |

The environment variable takes the same values, comma-separated, for makefiles that shouldn't
change (`M68K_OS9_ELF=save=subdir,obj=rof`); options override it. Neither Microware driver
has these options: `-elf…` and `-obj=` are checked before `-e` and `-o`.

## Objects

Makefiles name their objects `.r` (`RELS/foo.r: foo.c`, then link `RELS/foo.r`), so `-r`
writes the object under that name. By default it's an ELF object in a `.r` file, which only
the wrapper links: it tells ELF from ROF by their first bytes, uses ELF objects as they are
and converts ROF ones. `-obj=rof` writes real ROF instead (through `elf2rof`), for linking
with Microware's `l68`.

The same goes for `.o` inputs, which some makefiles use for ROF objects: whatever the suffix,
the first bytes decide. Linker inputs other than `.o` and `.a` go to the linker through
`-Xlinker`, since GCC's driver would take a `.r` file for a Ratfor source.

## C 3.2 mode

GCC options always given: `-mpcrel -ma6rel -mos9call -mbuiltin=os9call -mos9newline`, with
C 3.2's macros (GCC predefines them on m68k-elfos9), and `-std=gnu89` for K&R code. Stack
checking is on, as in C 3.2 (`-mos9stkchk`, with `cstart.r`'s `_stkcheck`), unless `-s`.

Options are case-insensitive, `=` optional where C 3.2 allows it.

| Option | Meaning in C 3.2 | Does |
|---|---|---|
| `-a` | assembler output (`.a`) | `-S`, written as `.a`, but in GNU `as` syntax, not r68's |
| `-bg` | adhesive (sticky) module | error |
| `-bp` | show the phases' command lines | prints the commands run |
| `-c` | comments in assembler output | ignored (or `-fverbose-asm`) |
| `-cs=<file>` | another root psect | that file instead of `cstart.r` |
| `-d<name>[=<value>]` | define | `-D` |
| `-e=<n>` | module edition | `elf2mod -e` |
| `-f=<path>`, `-fd=<path>` | output (`l68 -o=`, execution directory; `-O=`, data directory) | the output path, as given (no such directories on a cross build) |
| `-g` | source-level debugging (`c68 -g`, `l68 -g`) | `-g`, and `elf2mod -g` (`.stb`); no `.dbg` |
| `-h` (Ultra C compat only) | don't run the phases | prints them only |
| `-i` | link with `cio` (`cio.l` before the C library) | the same (run on a CD-i 605: `cio.l` needs `btext`, which `elf2mod` resolves) |
| `-j` | no jump table (drops `l68`'s `-a`, otherwise always given) | an error if the program needs one |
| `-k=<n>[w\|l][cw\|cl][f]` | target, offset sizes, 68881 | `0`: `-m68000`; `2`: `-m68020`; `f`: `-m68881`; `l`, `cl`: error |
| `-l=<file>` | another library | linked (converted if ROF) |
| `-lo=<opts>` | options for `l68` (put after its `-o=`) | error, unless they map onto `elf2mod`'s |
| `-m=<n>[k]` | more stack (`l68 -M=`, in K with or without the `k`) | `<n>` K added to the module's stack size |
| `-n=<name>` | module name | `elf2mod -n` |
| `-nl` | no default libraries | none (`cstart.r` still, unless `-cs=`) |
| `-nv` | OS-9000 only | ignored |
| `-o[=<n>]` | optimization (0-2; on by default) | `-O<n>`; default `-O2` |
| `-q` | quiet | quiet |
| `-r[=<dir>]` | stop at `.r` files, in `<dir>` | `-c`, written as `<dir>/<name>.r` |
| `-s` | no stack checking | no `-mos9stkchk` |
| `-t=<dir>`, `-t?` | temporary files; list targets | `TMPDIR`; lists the targets |
| `-to=<name>` | target system | `osk` only, else an error |
| `-tp=<cpu>[...]` | target processor | `68k`, `020` (with the 68881, as in C 3.2), `cpu32`; others an error |
| `-u<name>` | undefine | `-U` |
| `-v=<dir>` | header directory | `-I`, before `CDEF` |
| `-w=<dir>` | library directory | replaces `CLIB` |
| `-x` | math trap | `-mos9math`; C library as C 3.2 picks it (see below) |
| `-z[=<file>]` | options from a file | read the same way |

Default libraries, after `cstart.r`, as C 3.2's `xcc -bp` shows them (the `n` libraries go
with `c68 -t`, which calls `math.l`'s floating-point routines instead of the trap):

| Options | Libraries | Compiler |
|---|---|---|
| (default) | `clibn.l math.l sys.l` | `c68 -t` |
| `-x` | `clib.l sys.l` | `c68` |
| `-k=2` | `clib020n.l math.l sys.l` | `c68020 -t` |
| `-k=2f`, `-tp=020` | `clib020h.l math881.l sys.l` | `c68020 -t881` |
| `-k=2 -x`, `-k=2f -x` | `clib020.l sys.l` | `c68020` (the trap, even with `f`) |

The wrapper links the same ones. GCC code doesn't need `math.l`/`math881.l` (its floating
point is libgcc's or, with `-mos9math`, the math module's), but they stay for Microware
objects linked in. The default output name is the first source file's, without its suffix
(`pp.c` makes `pp`), as in C 3.2.

Source files, by suffix in any case: `.c` compiled; `.r`, `.o` and `.l` linked (converted as
needed); `.a` (r68 assembler) can't go through GNU `as`: an error, until the planned r68
translator (Motorola syntax to GNU `as`). C 3.2's `xcc` runs `.a` files through `o68` before
`r68` (`xcc -bp`: `o68 t3.a cm000354`, `r68 cm000354 -q -o=t3.r`). Both Microware drivers
reject `.s` and `.S` ("no recognized suffix"), and so does the wrapper.

## Ultra C mode

Ultra C's own options, as `m68k-os9-ucc` or with `-mode=ucc`/`c89`:

| Option | Meaning in Ultra C | Does |
|---|---|---|
| `-a[=warn\|err]`, `-bc` | ANSI or K&R source mode | `-std=c89 -pedantic[-errors]`, `-std=gnu89` |
| `-b`, `-h` | show phases, don't run them | as in C 3.2 mode |
| `-c` | constant pointers in the code area | ignored (GCC keeps constants with code) |
| `-cq` | C++ comments in C | accepted (`-std=gnu89` allows them) |
| `-cs`, `-d`, `-f`, `-fd`, `-g`, `-l`, `-u`, `-v`, `-w`, `-z` | as in C 3.2 mode | as in C 3.2 mode |
| `-cw` | warnings | `-Wall` |
| `-e=<phase>[=<dir>]` | stop after a phase | `as`: objects (`-c`); `be`, `ao`: assembler (`-S`); `fe`, `il`, `io` (I-code): error |
| `-i` | shared C library | error (not available) |
| `-j`, `-y` | I-code libraries | error |
| `-k` | no default libraries | as `-nl` |
| `-m=<phase>=<n>` | stack for a phase | ignored |
| `-mt` | multi-threading | error unless `none` |
| `-mw=<dir>` | MWOS tree | as `MWOS` |
| `-n` | layout data area | ignored |
| `-o[=<n>]` | optimization 0-7 | `0`: `-O0`, `1`-`3`: `-O1`, `4`-`7`: `-O2` |
| `-p[<mode>][=<dir>]` | preprocess | `-E` |
| `-q…`, `-edgx` | C++ | error (no C++ yet) |
| `-r` | no stack checking | no `-mos9stkchk` |
| `-s=<n>`, `-t=<n>` | space or time weight | `-Os` when space outweighs time, else ignored |
| `-sl=<dir>` | library directory | searched for `-l=` |
| `-td=<dir>` | temporary files | `TMPDIR` |
| `-to`, `-tp`, `-t?` | targets | as in C 3.2 mode (`-tp=68000`, `68020`, `68k`, ...) |
| `-x=<phases>` | skip phases | ignored |
| `-<phase>=<opt>` | pass an option to a phase | error, except `-ol=` mapped as `-lo=` |

The C library in Ultra C mode is Ultra C's own once `rof2elf` reads it (until then C 3.2's,
from `CLIB`); `-mos9newline` isn't given (ANSI `'\n'`), but the convention flags are.

### Ultra C's libraries

Done (2026-10-04): `rof2elf` reads the libraries `libgen` makes, format type 1 (the 68000's,
with ROF edition 9 or 9.1 psects); see `rof2elf.md` for the format and what the manual leaves
out. Each psect becomes one member of the `ar` archive, as for a ROF library, and no Microware
tool is involved in converting. Converted on demand like any other Microware library, with the
same settings.

Checked by hand, on the user's own copies (no test target yet): libraries `libgen` made from
ROFs of both editions convert to members byte-identical to the ROFs' own conversions; all 43
`libgen` libraries of the Ultra C SDK convert (`conv_lib.l`, `sys_clib.l`, `sys_csl.l` with
`-e sys.l`); a program Ultra C compiled, linked by GNU ld with the converted `acstart.r`,
`clib.l`, `os_lib.l` and `sys.l`, runs on a CD-i 605 like Ultra C's own link of it. Ultra C's
link line (`xcc -b`): `l68 -t=os9_68k acstart.r -o=… -f=orowoe -x=2 -b=2 -l=clib.l
-l=os_lib.l -l=sys.l prog.r`. Its startup needs the linker-defined `_enddata` (C 3.2's `end`),
which Ultra C mode's linker script must define. A test target, with `LIBGEN=…` skipped when
there's no `libgen`, is still to do.

## Usage message

`-?` (and no arguments) prints the usage of the current mode, complete: every option the mode
accepts, in Microware's order for that mode (C 3.2's `xcc -?` in C 3.2 mode, Ultra C's in Ultra C
mode, `-mode=compat`'s with `-h` and `-z`), each with what the wrapper does when that differs from
Microware's driver, in square brackets: ignored, not supported, or how it's mapped. Then the
wrapper's own options (`-mode=`, `-elf-save=`, `-elf-dir=`, `-elf-cache=`, `-obj=`), the files it
takes by suffix, and the environment it reads (`CDEF`, `CLIB` in C 3.2 mode; `MWOS` in Ultra C mode;
`M68K_OS9_ELF`, `GRPUSER`), with their formats but not their values, so the usage doesn't grow with
them. Lines aren't wrapped; one with a long option starts its text on the next line. `-t?` lists the
targets the mode accepts, as Microware's drivers do.

The message is generated from the same table of options as the parser, so the two can't
differ; a test checks that every option in the table appears in the usage of its modes, and
that each option the usage shows is accepted.

## Unsupported options

Options without an effect on the program (`-c`, `-nv`, `-q`, weights, phase stack sizes) are
ignored silently. Options asking for something the wrapper can't produce (adhesive modules,
all-long offsets, I-code, C++, OS-9000) are errors: the wrapper names the option and stops
with status 1, rather than build a different program than the makefile expects.

## Linking

Compiled and converted objects, the libraries and `cstart.r`'s conversion (with its `__os9_*`
module header symbols) are linked by GNU ld (`-q`, a linker script like
`test/l68cmp/mod.lds`), then made into a module by `elf2mod` (`-n`, `-e`, `-s`, `-g` from the
options). A program with more than 32 KB of code is linked again with the jump table `elf2mod`
asks for, as `m68k-os9-gcc` does.

That only helps calls between source files, which the linker resolves. Within one source file
with more than 32 KB of code, GNU `as` resolves the PC-relative calls itself and fails ("value
of -65864 too large for field of 2 bytes"); such a file has to be split.

## Tests

On the CD-i 605 of CD-i Emulator, through `cdirun`, with C 3.2's `CLIB` and `CDEF`; each
program ran and printed what it should:

- a K&R `hello.c` with Microware's `printf` and `double`: the default build, `-x` (the code
  has `trap #15`), `-r=RELS` then linking the ELF `.r`, `-obj=rof -r=RELS` then linking the ROF
  `.r`, and `-m=8 -e=3 -n=greet` (stack `$2C00`, as `l68 -M=8` makes it; edition 3; module
  `greet`)
- a program of six source files with 124 KB of code: 483 far references through a jump table
  of 211 entries; with `-j` an error instead
- `hello.c` with `-i` (`cio.l`, whose `iobinit` points to `btext`), a program calling
  `_errmsg` (which finds the module's name through `btext`; it prints `errm: …`), and the
  124 KB program calling `_errmsg`, whose `btext` is beyond 32K and goes through the jump table

Without running: `-a` (GNU `as` source), `-k=2f` (`-m68020 -m68881`, `clib020h.l`,
`math881.l`), the `beside` and `subdir` settings on a copy of the libraries, ROF objects named
`hello.o` and `HELLO2.R`, the conversion cache reused on a second build.

## The other front ends (planned)

In this order (decided 2026-10-04): `m68k-os9-l68`, `m68k-os9-libgen`, `m68k-os9-ucc` (Ultra C
mode, above), `m68k-os9-gcc` rewritten in C on `os9link`, then an r68 translator (Motorola
syntax to GNU `as`, with `os9`/`tcall` and macros).

### m68k-os9-libgen

For makefiles that build libraries. Microware C 3.2 has no librarian: a library is ROFs
concatenated (`merge a.r b.r >lib.l` on OS-9, `copy /b` with the DOS kit), so C 3.2 makefiles
need no front end, only `.r` files that concatenate meaningfully. Ultra C's makefiles run
`libgen -c -o=lib.l a.r b.r` (`-c` create, `-o=` output, `-e=` edition, `-z[=<file>]` file
names from a file or stdin, `-b=` buffer size; listings `-l`, `-le`, `-li`, `-ll`, `-ln`, `-lu`,
`-p=<psect>`, `-f=` for their output). GCC users make archives with `ar rcs lib.a a.o b.o`
(`s` writes the index, as `ranlib` does), through the `m68k-os9-ar` link.

`m68k-os9-libgen` takes `libgen`'s options and writes, by `-obj=` (and `M68K_OS9_ELF`), as
`m68k-os9-xcc -r` does:

- `-obj=elf` (the default): an `ar` archive with its index, under the name the makefile gives
  (`lib.l`). `m68k-os9-xcc` and `m68k-os9-l68` tell `ar` from ROF by the first bytes, so it
  links as is.
- `-obj=rof`: concatenated ROFs (`elf2rof` on ELF members, ROF inputs as they are), which both
  Microware linkers read: C 3.2's `l68`, and Ultra C's, which links the SDK's driver libraries
  in that form. `libgen`'s own format isn't written (decided 2026-10-04: concatenated ROFs for
  now); doing so would need `libgen`'s hash function for the global definition table, which the
  manual doesn't give but libraries `libgen` makes could reveal.

Inputs: ELF `.o`/`.r`, ROF `.r` (converted with `rof2elf` for an ELF archive), and libraries of
any kind (`ar`, concatenated ROFs, `libgen`'s), taken member by member. Listings map onto what
exists: `-ln` onto `nm`, `-l`/`-lu` onto the member names, `-le` onto `rof2elf -l`'s sizes;
`-ll` and `-li` only approximately.

## Open questions

- An r68 for `.a` sources: the planned translator, or Ultra C's `r68.exe` through WSL, when the
  user has it.
- newlib instead of Microware's libraries, e.g. `-libc=newlib`, for code that doesn't need
  Microware's library.
