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
- `.text` VMA must be at 0 (this can be improved...)
- `.text` must *NOT* contains relocations to .data/.bss, *except* `R_68K_16` (for referencing .data/.bss symbols relative to A6 register)
  - `R_68K_32` and `R_68K_8` to .data/.bss or absolute symbols are allowed too: their values are
    link-time constants, such as a6-relative data offsets (`move.l #sym,a0` followed by
    `adda.l a6,a0`) or equates, as in code converted from ROF by [rof2elf](rof2elf.md).
    (This fork's addition.)
  - Other relocations are not allowed
- `.data` VMA must be at -0x8000 like
  - No bias are done for `R_68K_16` in `.text` by elf2mod.
  - In other words, -0x8000 bias must be done at ELF file.
- `.data` must *NOT* contains relocations *except* 32bit direct relocations i.e. `R_68K_32`
- `.bss` must be contiguous after `.data` (usually it is.)
- `.bss` must not have CONTENTS (usually it is.)
- Relocation values must be applied but relocation itself must be left
  - in GNU ld, it can be done with `-q`
