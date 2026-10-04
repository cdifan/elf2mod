# elf2mod

Utilities for converting between m68k ELF and OS-9/68000 object and executable files, based on
Murachue's elf2mod.

This is a fork of [murachue/elf2mod](https://github.com/murachue/elf2mod), made for the OS-9
compatibility work in [cdifan/toolchaincdi](https://github.com/cdifan/toolchaincdi). elf2mod
itself is Murachue's, with extensions; rof2elf and elf2rof are new.

# Tools

- [elf2mod](elf2mod.md): converts a specially-crafted m68k ELF object into an OS-9/68000
  executable file.
- [rof2elf](rof2elf.md): converts Microware ROF objects (`.r`) and libraries (`.l`) into m68k ELF
  relocatables and `ar` archives, for linking with GNU `ld`.
- [elf2rof](elf2rof.md): converts m68k ELF relocatables and `ar` archives into ROF objects and
  libraries, for linking with Microware's `l68`.
- `m68k-os9-gcc`: compiles and links an OS-9 program into a module in one step, with newlib's OS-9
  runtime (`m68k-elfos9-gcc -specs=os9.specs`, from cdifan's newlib) and elf2mod:

  ```
  m68k-os9-gcc -Os -o hello hello.c        # hello.elf, then the module hello
  m68k-os9-gcc -Os -o hello hello.c -Wm,-g # also the symbol module hello.stb
  ```

  It takes GCC's options, and elf2mod's after `-Wm,` (comma-separated). A program with more than
  32 KB of code is linked again with the jump table elf2mod asks for. On OS-9, `malloc` has 16 KB
  plus what the shell's `#` modifier adds (`hello #64k`); see `os9.ld` in newlib's
  `libgloss/m68k` for the module's defaults.

# Build and install

- Pre-requisite: BFD for m68k-elfos9 is installed. (binutils with `--enable-install-libbfd`)
  - Only elf2mod needs it; rof2elf and elf2rof need just a C compiler.
- Tweak `BFDPATH` in Makefile (if you are not on linux-amd64)
- `make && make install`
  - you can specify DESTDIR= to set install prefix.
	- `make DESTDIR=/usr install` installs elf2mod, rof2elf, elf2rof and m68k-os9-gcc in `/usr/bin`,
	  with `m68k-os9-` links to the `m68k-elfos9-` binutils (`ar`, `as`, `ld`, ...)
- Profit!

# License

MIT
