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
- A library (several ROFs concatenated) becomes an `ar` archive (`clib.l` → `clib.a`), with one
  member per ROF. Run `ranlib` on it before linking.
- `-e EQUFILE` takes `equ` definitions from another ROF or library, usually `sys.l`, and folds
  references to them into the code. This is needed when code adds several equates in one place
  (e.g. `Dir_+Write_` in a mode byte), which an ELF relocation can't express.
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
| global definitions | global symbols; `equ` definitions become absolute symbols, common definitions ELF common symbols |
| external and local references | RELA relocations: `R_68K_8`, `R_68K_16`, `R_68K_32`, or `R_68K_PC8`, `R_68K_PC16`, `R_68K_PC32` for relative references; the addend is taken from the object bytes |

- Data references are a6-relative. They come out right with a linker script that places `.data`
  at -0x8000, as elf2mod requires.
- An external reference paired with a negated reference to the same ROF's code, at the same
  place, is a 32-bit PC-relative value (`cstart.r` calls through `jsr (pc,dN.l)` this way). It
  becomes an `R_68K_PC32` relocation.
- Other combinations of references at one place are rejected, unless only equates are involved.
- A mainline ROF (one with a module type, like `cstart.r`) also gets absolute symbols holding its
  module header values: `__os9_tylan`, `__os9_attrev`, `__os9_edition` and `__os9_stack`, and code
  symbols `__os9_entry` and `__os9_trapent`, which elf2mod uses for the module header.

The ROF format is described in the *OS-9 Assembler/Linker User Manual*, chapter 3, "Relocatable
Object File Format", and in *Using Ultra C/C++*, chapter 6. rof2elf reads ROF edition 9, from
Microware C 3.2's assembler, and 9.1 (series 0xF9), from Ultra C's r68 for the 68000, whose
counts of symbols and references are 32 bits. It rejects other editions (9.2, which adds header
fields, and 15). Ultra C's libraries are in a different container format, not supported yet.
Section 6 of
[OS9-COMPAT-DESIGN.md](https://github.com/cdifan/toolchaincdi/blob/main/OS9-COMPAT-DESIGN.md)
describes the design.
