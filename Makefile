# MIT License
#
# Copyright (c) 2021 Murachue
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

BFDPATH=/usr/x86_64-pc-linux-gnu/m68k-elfos9
DESTDIR=/usr
EXE=elf2mod
EXE_ROF2ELF=rof2elf
EXE_ELF2ROF=elf2rof

CC=gcc
CFLAGS=-g -I$(BFDPATH)/include -Wall -Wextra
LDFLAGS=
LIBS=$(BFDPATH)/lib/libbfd.a -liberty -lz -ldl
INSTALL=install
RM=rm

.PHONY: all clean

all: $(EXE) $(EXE_ROF2ELF) $(EXE_ELF2ROF)

install: $(EXE) $(EXE_ROF2ELF) $(EXE_ELF2ROF)
	$(INSTALL) -s $(EXE) $(DESTDIR)/bin/$(EXE)
	$(INSTALL) -s $(EXE_ROF2ELF) $(DESTDIR)/bin/$(EXE_ROF2ELF)
	$(INSTALL) -s $(EXE_ELF2ROF) $(DESTDIR)/bin/$(EXE_ELF2ROF)

clean:
	-$(RM) $(EXE) $(EXE_ROF2ELF) $(EXE_ELF2ROF)

$(EXE): elf2mod.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

# rof2elf and elf2rof need no BFD.
$(EXE_ROF2ELF): rof2elf.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(EXE_ELF2ROF): elf2rof.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
