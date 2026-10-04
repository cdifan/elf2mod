# rof2elf

Converts Microware OS-9/68000 ROF objects (`.r`) and libraries (`.l`) into m68k ELF relocatables
and `ar` archives, so they can be linked with GNU `ld` together with code from the
[m68k-elfos9 GCC](https://github.com/cdifan/toolchaincdi), and turned into an OS-9 module with
[elf2mod](elf2mod.md).

# Usage

```
rof2elf [-o OUTPUT] [-e EQUFILE]... INPUT
rof2elf -l INPUT
```

- A single ROF object becomes an ELF relocatable (`cstart.r` → `cstart.o`).
- A library becomes an `ar` archive (`clib.l` → `clib.a`), with one member per psect. Run
  `ranlib` on it before linking. Both kinds of library are read: several ROFs concatenated
  (sometimes just one), as Microware C 3.2's and Ultra C's driver libraries are, and the
  format Ultra C's `libgen` makes (type 1, the 68000's), as Ultra C's `clib.l`, `os_lib.l` and
  most of its other libraries are. A psect converts to the same ELF object from either.
- `-e EQUFILE` takes `equ` definitions from another ROF or library, usually `sys.l`, and folds
  references to them into the code. This is needed when code adds several equates in one place
  (e.g. `Dir_+Write_` in a mode byte), which an ELF relocation can't express. An equate
  defined with different values (Microware C 3.2's and Ultra C's `sys.l` differ, e.g. `DEVSIZ`)
  is reported, and the first definition wins; a folded value must fit its field.
- `-l` lists the ROFs in `INPUT`.

For example, with Microware's C library (from your own installation; Microware's files are not
included here):

```
rof2elf -e sys.l cstart.r
rof2elf -e sys.l clib.l && m68k-elfos9-ranlib clib.a
m68k-elfos9-gcc -c -mpcrel -ma6rel -mos9call -mbuiltin=os9call main.c
m68k-elfos9-ld -q -T os9.lds -o prog.elf cstart.o main.o clib.a
elf2mod prog.elf prog
```

GCC code called by, or calling, Microware C code must use Microware's calling convention
(`-mos9call`, or the `os9call` attribute); `-mbuiltin=os9call` makes `memcpy`, `strlen` and the
other C library functions GCC knows use it too, as `clib` defines them. The linker script must
define the symbols Microware's linker provides, such as `end`.

# Conversion

| ROF | ELF |
|---|---|
| code | `.text` |
| initialized data | `.data` |
| uninitialized data | `.bss` |
| remote initialized / uninitialized data | `.remote.data` / `.remote.bss` |
| debug information | dropped |
| global definitions | global symbols; `equ` definitions become absolute symbols (so do `set` definitions, which the manual documents, though Microware's assemblers don't export `set` labels), common definitions ELF common symbols |
| external and local references | RELA relocations: `R_68K_8`, `R_68K_16`, `R_68K_32`, or `R_68K_PC8`, `R_68K_PC16`, `R_68K_PC32` for relative references; the addend is taken from the object bytes |

- Data references are a6-relative. They come out right with a linker script that places `.data`
  at -0x8000, as elf2mod requires.
- An external reference paired with a negated reference to the same ROF's code, at the same
  place, is a 32-bit PC-relative value (`cstart.r` calls through `jsr (pc,dN.l)` this way). It
  becomes an `R_68K_PC32` relocation.
- Other combinations of references at one place are rejected, unless only equates are involved.
- A mainline ROF (one with a module type, like `cstart.r`) also gets absolute symbols holding its
  module header values: `__os9_tylan`, `__os9_attrev`, `__os9_edition` and `__os9_stack`, and code
  symbols `__os9_entry` and `__os9_trapent` (only when the ROF has a trap entry, not
  0xFFFFFFFF), which elf2mod uses for the module header.

The ROF format is described in the *OS-9 Assembler/Linker User Manual*, chapter 3, "Relocatable
Object File Format", and in *Using Ultra C/C++*, chapter 6. rof2elf reads ROF edition 9, from
Microware C 3.2's assembler, and 9.1 (series 0xF9), from Ultra C's r68 for the 68000, whose
counts of symbols and references are 32 bits. It rejects other editions (9.2, which adds header
fields, and 15).

`libgen`'s library format is described in *Using Ultra C/C++*, chapter 9, "Library Format
Created by libgen": a header, a hash table of the global definitions, the definitions, a string
table, a table of psects, the references to symbols the library defines and to others, then each
psect's code, initialized data and references. rof2elf reads type 1 (ROF edition 9 or 9.1
psects; type 3 is for edition 15). What the manual leaves out, as `libgen` from the Ultra C 2.5
SDK writes it:

- the header is 46 bytes: type 1 has the 4 reserved bytes the manual lists only for type 3
- a psect entry has a 4-byte field more than listed, after the stack size; it stays zero (r68
  psects with different stack sizes, editions and entry points: only the stack size is kept;
  `libgen` refuses mainline psects, so a library never needs a module header)
- reference entries are 16 bytes, with a 4-byte count; list indices count entries, and -1 ends
  a list
- a psect's local references are a count (2 or 4 bytes, by its ROF edition) and the
  references, as in a ROF
- `libgen` lists a psect's references to library symbols before the others; rof2elf sorts them
  by name, as r68 writes them, so the ELF object is the same as from the ROF

Checked with libraries made by `libgen` from ROFs of both editions, whose members are
byte-identical to the ROFs' conversions; all 43 `libgen` libraries of the SDK convert (three
need `-e sys.l`); and a program Ultra C compiled, linked with GNU `ld` from the converted
`acstart.r`, `clib.l`, `os_lib.l` and `sys.l`, runs on a CD-i 605 as Ultra C's own link of it
does. Ultra C's startup uses the linker-defined `_enddata` (Microware C 3.2's `end`), which the
linker script must define too.

Section 6 of
[OS9-COMPAT-DESIGN.md](https://github.com/cdifan/toolchaincdi/blob/main/OS9-COMPAT-DESIGN.md)
describes the design.
