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
    .text : { *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(2); }
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
