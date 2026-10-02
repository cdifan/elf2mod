# elf2rof

Converts m68k ELF relocatables (`.o`) and `ar` archives (`.a`) into Microware OS-9/68000 ROF
objects (`.r`) and libraries (`.l`), so code from the
[m68k-elfos9 GCC](https://github.com/cdifan/toolchaincdi) can be linked with Microware's linker
`l68`. It's the reverse of [rof2elf](rof2elf.md).

# Usage

```
elf2rof [-k] [-n NAME] [-o OUTPUT] INPUT
```

- An ELF relocatable becomes a ROF object (`main.o` → `main.r`), one psect named after the file
  (`main`), or `NAME` with `-n`.
- An `ar` archive becomes a library of concatenated ROFs (`libgcc.a` → `libgcc.l`), one psect
  per member, named after it.
- `-k` skips archive members that can't be converted, with a warning, instead of stopping. For
  example, libgcc's soft-float members use GOT relocations.

For example, GCC code linked with Microware's `cstart.r` and C library (from your own
installation; Microware's files are not included here):

```
m68k-elfos9-gcc -c -mpcrel -ma6rel -mos9call -mbuiltin=os9call main.c
elf2rof main.o
elf2rof -k -o libgcc.l $(m68k-elfos9-gcc -print-libgcc-file-name)
l68 -o=prog cstart.r main.r -l=clib.l -l=libgcc.l -l=sys.l
```

As with rof2elf, GCC code called by, or calling, Microware C code must use Microware's calling
convention (`-mos9call`).

# Conversion

Each ELF object becomes one psect, in ROF edition 9, which the linkers of Microware C 3.2 and
Ultra C both read.

| ELF | ROF |
|---|---|
| read-only sections (`.text`, `.rodata`, ...) | code |
| writable sections (`.data`, ...) | initialized data |
| `.bss`, common symbols | uninitialized data |
| `.remote.data` / `.remote.bss` | remote initialized / uninitialized data |
| global symbols | definitions (code, data, remote data; absolute symbols as `equ`) |
| relocations to undefined symbols | external references |
| relocations to this object's code and data | local references |
| PC-relative relocations within the code | resolved, no reference |
| relocations to absolute symbols | resolved, no reference |
| `__os9_*` symbols | the module header (see below) |
| debug information, other sections | dropped |

- Relocations: `R_68K_32`, `R_68K_16`, `R_68K_8` and their PC-relative forms, and the PLT forms,
  which are PC-relative in a static link (as GNU ld treats them). The addend goes into the object
  bytes, as ROF has no addend field.
- A 4-byte PC-relative reference to an external symbol becomes, as Microware's assemblers write
  it, an external reference plus a negated reference to the psect's own code (`l68` doesn't
  compute 4-byte relative references).
- Data can only hold 32-bit absolute references; absolute references from code to code aren't
  possible (OS-9 code is position-independent).
- Not supported: GOT relocations, weak undefined symbols, section groups (COMDAT), and
  constructors and destructors (`.init_array`, `.ctors`, ...), for which `l68` has no mechanism.
- The `__os9_*` symbols of a mainline (from rof2elf, or defined by hand in assembly) give its
  module header: `__os9_tylan`, `__os9_attrev`, `__os9_edition` and `__os9_stack` (absolute),
  `__os9_entry` and `__os9_trapent` (code). Without them, the psect is a subroutine psect.
  `l68` needs one mainline among the files it links.

Like Microware's assemblers, elf2rof sorts definitions and external references by name and
writes local references in descending order of offset, and it ends every ROF with 16 zero bytes,
which `rdump` and `l68` skip. The order matters: `l68` pulls library members in the order of the
external references, and builds the module's initialized data references from the local
references in reverse.

## Large programs

`l68 -a` redirects far `bsr.w` and `lea` through its jump table, but not `bra.w`, which GCC uses
for tail calls with `-mbsrw`. For programs with more than 32K of code, compile without `-mbsrw`
(tail calls are then `lea` and `jmp`) or with `-fno-optimize-sibling-calls`. elf2mod's jump table
does handle `bra.w`.

# Testing

In [toolchaincdi](https://github.com/cdifan/toolchaincdi):

- `test/l68cmp/run.sh`: Microware C 3.2's `cstart.r`, C library and a test program go from ROF
  through rof2elf and elf2rof back to ROF; `l68` links the same module from them as from the
  originals, byte for byte.
- `make check-l68` in `test/abi-exec`: the Level 2 calling convention tests, compiled by GCC,
  converted by elf2rof (with libgcc), linked by `l68` and run as OS-9 modules in an emulator,
  including far calls through `l68`'s jump table.

The ROF format is described in the *OS-9 Assembler/Linker User Manual*, chapter 3, and in *Using
Ultra C/C++*, chapter 6. Section 6 of
[OS9-COMPAT-DESIGN.md](https://github.com/cdifan/toolchaincdi/blob/main/OS9-COMPAT-DESIGN.md)
describes the design.
