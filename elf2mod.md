# elf2mod

A utility that converts a specially-crafted m68k ELF object into a OS-9/68000 executable file.

# Usage

- Prepare a specially-crafted ELF file
  - See next section for details
- Run elf2mod, like `elf2mod some.elf CDI_SOME.APP`
  - In default, module name become basename without extension of output file, in lower case.
  - In above example, it become `cdi_some`
- Profit!

You can `elf2mod --help` to get descriptions of options.

You need to have OS-9/68000 executable knowledge...

# Specially-crafted ELF file?

- Contains `.text`, `.data` and `.bss` sections
  - Other sections are ignored, including `.text.*`, `.rodata`, etc.
  - You must meld them into above sections (using linker script.)
  - Optionally `.remote.data` and `.remote.bss` too, for remote data (see below). (This fork's
    addition.)
- `.text` may be at any VMA. (This fork's change; it had to be 0.)
- `.text` must *NOT* contains relocations to .data/.bss, *except* `R_68K_16` (for referencing .data/.bss symbols relative to A6 register)
  - `R_68K_32` and `R_68K_8` to data sections or absolute symbols are allowed too: their values
    are link-time constants, such as a6-relative data offsets (`move.l #sym,a0` followed by
    `adda.l a6,a0`, as for remote data) or equates, as in code converted from ROF by
    [rof2elf](rof2elf.md). (This fork's addition.)
  - Other relocations are not allowed
- The data area must start at VMA -0x8000, where A6 points 0x8000 into it
  - No bias are done for `R_68K_16` in `.text` by elf2mod.
  - In other words, -0x8000 bias must be done at ELF file.
  - It starts with `.data` or `.bss`, whichever comes first.
- `.data` must *NOT* contains relocations *except* 32bit direct relocations i.e. `R_68K_32`
- `.bss` must be contiguous with `.data` (usually it is.)
- `.bss` must not have CONTENTS (usually it is.)
- Relocation values must be applied but relocation itself must be left
  - in GNU ld, it can be done with `-q`

## More than 32K of data, and remote data (this fork)

- A6-relative addressing reaches 64K of data, from -0x8000 to +0x7FFF. With more than 32K, the
  data sections cross from 0xFFFFFFFF to 0, which GNU ld only accepts with
  `--no-check-sections`.
- Remote data (variables with GCC's `remote` attribute, or Microware's remote vsects converted
  by rof2elf) goes in `.remote.data` and `.remote.bss`, after the other data sections, beyond the
  64K that A6 reaches. Code addresses it as A6 plus a 32-bit offset.
- The data area then extends past 0, so place `.text` above it (e.g. at `0x400000`).
- The module's initialized data is one block, covering `.data` and `.remote.data`. Put `.bss`
  before `.data` in the linker script, so the two are adjacent and no zeros are stored for `.bss`.

For example:

```
SECTIONS {
    . = 0x400000;
    /* .rodata in the same statement: each object's constants follow its code,
       within PC-relative reach also in programs over 32K */
    .text : { *(.text .text.* .rodata .rodata.*) . = ALIGN(2); }
    . = -0x8000;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) . = ALIGN(2); }
    .data : { *(.data .data.*) . = ALIGN(2); }
    .remote.data : { *(.remote.data .remote.data.*) . = ALIGN(2); }
    .remote.bss (NOLOAD) : { *(.remote.bss .remote.bss.*) . = ALIGN(2); }
    end = .;
}
```

## Module header (this fork)

A mainline ROF converted by rof2elf (such as Microware's `cstart.r`) defines `__os9_*` symbols
with its module header values. elf2mod uses them where present: type and language
(`__os9_tylan`), attributes and revision (`__os9_attrev`), edition (`__os9_edition`), stack size
(`__os9_stack`), entry point (`__os9_entry`) and uninitialized trap entry (`__os9_trapent`).
The `--stack`, `--revs` and `--edit` options override them. Without them, the defaults are as
before: program module, object code, reentrant, revision 1, edition 0, stack 0xC00.

## Far calls: the jump table (this fork)

Calls and address loads are PC-relative with 16-bit displacements, so they reach +-32K. For
larger programs, elf2mod builds a jump table, as Microware's linker does: each target that's too
far gets an entry `jmp target` in `_jmptbl`, in the data area (relocated when OS-9 loads the
module), and each far reference is redirected through it, keeping its size:

| Instruction | Becomes |
|---|---|
| `bsr.w f` | `jsr _jmptbl+x(a6)` |
| `bra.w f` | `jmp _jmptbl+x(a6)` |
| `lea f(pc),An` | `movea.l _jmptbl+x+2(a6),An` (the real address of `f`) |
| `pea f(pc)` | `move.l _jmptbl+x+2(a6),-(sp)` |

- Link with `--noinhibit-exec` as well as `-q`: ld then reports "relocation truncated to fit"
  for the far references, but still writes the output, which elf2mod fixes.
- The linker script reserves the table at the end of `.data`, from `_jmptbl` to `_ejmptbl`, 6
  bytes per entry; elf2mod reports how many entries it needs if there aren't enough. For example,
  with the size given on the command line (`--defsym __jmptbl_size=600`):

```
    .data : {
        *(.data .data.*)
        . = ALIGN(2);
        _jmptbl = .;
        . += DEFINED(__jmptbl_size) ? __jmptbl_size : 0;
        _ejmptbl = .;
    }
```

- Conditional branches (`Bcc`, `DBcc`) and other far PC-relative references can't be redirected;
  elf2mod reports them as errors.
- `move.l` changes the condition codes where `pea` doesn't; that only matters for hand-written
  assembly that relies on them.

## Symbol modules for the debugger (this fork)

With `-g` (`--stb[=STYLE]`), elf2mod also writes a symbol module `OUTFILE.stb`, as Microware's linker
does with `l68 -g`: an OS-9 data module named `MODNAME.stb` that lists the program's global
symbols for the user-state debugger and SrcDbg. If there's a directory `STB` next to OUTFILE, the
symbol module goes there instead, as with l68. The format is described in appendix A of the
*OS-9/68000 User-State Debugger* manual:

- the module header, with its symbol field pointing to the STB header; the name
- the STB header: the format (0x0100), the program module's CRC (so the debugger can check that
  the symbols belong to the program), the offset and number of the symbol entries
- the symbol entries, by value: a 4-byte value, a 2-byte type (0 uninitialized data,
  1 initialized data, 2 uninitialized remote data, 3 initialized remote data, 4 code, 6
  absolute) and the 4-byte offset of the name.
  Code symbols are offsets in the module, data symbols a6-relative (their VMAs). Static
  symbols, `__os9_*` and `_ejmptbl` aren't listed.
- the symbols the linker defines, with type flag 0x2000: `btext` (0), `bname` (the module name),
  `etext` (the module size), `end` (the end of the data area) and `_jmptbl`
- the names, then the module CRC

The two versions of l68 write this a little differently, and `--stb=STYLE` follows either:

- `c32` (the default, as `-g`): Microware C 3.2's l68, as used for CD-i. The type flag 0x2000
  also marks the symbols that other psects refer to; elf2mod sets it on the symbols that
  relocations refer to. Symbols with the same value are listed in reverse order of definition
  (here: by name, descending).
- `ucc`: Ultra C's l68. It also defines `_btext`, `_bname`, `_etext`, `_bdata`/`bdata` (the start
  of the data area) and `_enddata`, and doesn't flag referenced symbols. Symbols with the same
  value are listed linker symbols first, then by name. Its symbol modules have the header parity
  at offset 0x28 instead of 0x2E; elf2mod uses 0x2E, as for other modules (OS-9's check passes
  either way).

For the same input, the result matches l68's symbol module. The test `test/l68cmp` in
[toolchaincdi](https://github.com/cdifan/toolchaincdi) checks this with both linkers: it links
the same ROFs with l68 and with rof2elf, GNU ld and elf2mod. The program modules are then the
same as well, apart from the order of the data relocation table and the CRCs.

## Module owner (this fork)

The module owner (group.user) is `--owner` (`-u`), otherwise the `GRPUSER` environment variable as
with l68, otherwise 0.0. l68 uses 1.0 when `GRPUSER` isn't set.
