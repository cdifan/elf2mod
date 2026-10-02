/*
 * elf2rof: convert m68k ELF relocatables (.o) and ar archives (.a) to
 * Microware OS-9/68000 ROF objects (.r) and libraries (.l, concatenated
 * ROFs), for linking with Microware's linker l68.
 *
 * MIT License
 *
 * Copyright (c) 2026 CD-i Fan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Written with assistance from Claude (Anthropic).
 *
 * The reverse of rof2elf; see OS9-COMPAT-DESIGN.md in toolchaincdi,
 * section 6.  Each ELF object becomes one psect (ROF edition 9):
 *
 *   .text, .rodata (all read-only sections)  -> code
 *   .data (writable sections)                -> initialized data
 *   .bss, common symbols                     -> uninitialized data
 *   .remote.data / .remote.bss               -> remote init/uninit data
 *   global symbols                           -> definitions (absolute
 *                                               symbols: equ)
 *   relocations to undefined symbols         -> external references
 *   other relocations                        -> local references, or
 *                                               resolved here (PC-relative
 *                                               within the code, absolute
 *                                               symbols)
 *
 * The addend goes into the object bytes, as ROF has no addend field.
 * The __os9_* symbols (from rof2elf, or defined by hand in a mainline)
 * give the psect's module header: __os9_tylan, __os9_attrev,
 * __os9_edition and __os9_stack (absolute), __os9_entry and
 * __os9_trapent (code).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ROF reference and definition type bits.  */
#define T_BSS		0x0000
#define T_DATA		0x0001
#define T_RBSS		0x0002
#define T_RDATA		0x0003
#define T_CODE		0x0004
#define T_EQU		0x0006
#define T_COMMON	0x0100
#define R_BYTE		0x0008
#define R_WORD		0x0010
#define R_LONG		0x0018
#define R_LOC_DATA	0x0000
#define R_LOC_CODE	0x0020
#define R_LOC_RDATA	0x0200
#define R_NEG		0x0040
#define R_REL		0x0080

/* ELF.  */
#define SHT_SYMTAB	2
#define SHT_STRTAB	3
#define SHT_RELA	4
#define SHT_NOBITS	8
#define SHT_REL		9
#define SHT_GROUP	17
#define SHF_WRITE	0x1
#define SHF_ALLOC	0x2
#define SHN_UNDEF	0
#define SHN_ABS		0xFFF1
#define SHN_COMMON	0xFFF2
#define STB_LOCAL	0
#define STB_WEAK	2
#define STT_SECTION	3
#define STT_FILE	4
#define R_68K_NONE	0
#define R_68K_32	1
#define R_68K_16	2
#define R_68K_8		3
#define R_68K_PC32	4
#define R_68K_PC16	5
#define R_68K_PC8	6
#define R_68K_PLT32	13
#define R_68K_PLT16	14
#define R_68K_PLT8	15

/* The areas of a psect.  */
enum
{
  A_CODE, A_DATA, A_BSS, A_RDATA, A_RBSS, A_NUM, A_NONE = -1
};

static const char *const areanames[A_NUM] = {
  "code", "initialized data", "uninitialized data",
  "remote initialized data", "remote uninitialized data"
};
static const unsigned areakind[A_NUM] = {
  T_CODE, T_DATA, T_BSS, T_RDATA, T_RBSS
};

static const char *progname = "elf2rof";

/* With -k, where to go on an error in an archive member.  */
static jmp_buf *skip_member;

static void
die (const char *fmt, ...)
{
  va_list ap;
  fprintf (stderr, "%s: %s", progname, skip_member ? "skipped: " : "");
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  if (skip_member)
    longjmp (*skip_member, 1);
  exit (1);
}

static void *
xmalloc (size_t n)
{
  void *p = calloc (1, n ? n : 1);
  if (!p)
    die ("out of memory");
  return p;
}

/* Growable byte buffer.  */
typedef struct
{
  unsigned char *buf;
  size_t len, cap;
} Buf;

static size_t
buf_space (Buf *b, size_t n)
{
  size_t at = b->len;
  if (b->len + n > b->cap)
    {
      size_t cap = b->cap ? b->cap : 256;
      while (cap < b->len + n)
	cap *= 2;
      b->buf = realloc (b->buf, cap);
      if (!b->buf)
	die ("out of memory");
      memset (b->buf + b->cap, 0, cap - b->cap);
      b->cap = cap;
    }
  b->len += n;
  return at;
}

static void
buf_add (Buf *b, const void *p, size_t n)
{
  size_t at = buf_space (b, n);
  if (n)
    memcpy (b->buf + at, p, n);
}

static void
add16 (Buf *b, unsigned v)
{
  size_t at = buf_space (b, 2);
  b->buf[at] = v >> 8;
  b->buf[at + 1] = v;
}

static void
add32 (Buf *b, uint32_t v)
{
  size_t at = buf_space (b, 4);
  b->buf[at] = v >> 24;
  b->buf[at + 1] = v >> 16;
  b->buf[at + 2] = v >> 8;
  b->buf[at + 3] = v;
}

static void
add_name (Buf *b, const char *s)
{
  buf_add (b, s, strlen (s) + 1);
}

static unsigned
get16 (const unsigned char *p)
{
  return (p[0] << 8) | p[1];
}

static uint32_t
get32 (const unsigned char *p)
{
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | (p[2] << 8)
	 | p[3];
}

/* One input ELF object.  */
typedef struct
{
  const char *file;		/* for messages */
  const unsigned char *p;
  size_t size;
} Obj;

static const unsigned char *
at (Obj *o, uint32_t off, uint32_t len)
{
  if (off > o->size || len > o->size - off)
    die ("%s: truncated ELF file", o->file);
  return o->p + off;
}

typedef struct
{
  const char *name;
  uint32_t nameoff, type, flags, offset, size, link, info, align, entsize;
  int area;			/* A_*, or A_NONE if not converted */
  uint32_t base;		/* offset in the area */
} Sec;

typedef struct
{
  const char *name;
  uint32_t value, size;
  unsigned bind, type, shndx;
  int area;			/* A_* if defined in an area, else A_NONE */
  uint32_t offset;		/* offset in the area */
} Sym;

/* A reference in the output.  */
typedef struct
{
  unsigned type;
  uint32_t offset;
} Ref;

typedef struct
{
  const char *name;
  int nrefs;
  Ref *refs;
} Ext;

/* Sort by name, as Microware's assembler writes definitions and external
   references (l68 then pulls library members in the same order).  */
static int
cmp_ext (const void *a, const void *b)
{
  return strcmp (((const Ext *) a)->name, ((const Ext *) b)->name);
}

/* Local references in descending order of offset, as Microware's assembler
   writes them (l68 builds the initialized data references in the reverse
   order).  */
static int
cmp_loc (const void *a, const void *b)
{
  const Ref *x = a, *y = b;
  return x->offset < y->offset ? 1 : x->offset > y->offset ? -1 : 0;
}

typedef struct
{
  const char *name;
  unsigned kind;
  uint32_t value;
} Def;

static int
cmp_def (const void *a, const void *b)
{
  return strcmp (((const Def *) a)->name, ((const Def *) b)->name);
}

/* Which area a section goes to.  */
static int
section_area (Obj *o, Sec *s)
{
  if (s->type == SHT_GROUP)
    die ("%s: section groups (COMDAT) are not supported", o->file);
  if (!(s->flags & SHF_ALLOC))
    return A_NONE;
  if (!strcmp (s->name, ".init_array") || !strcmp (s->name, ".fini_array")
      || !strcmp (s->name, ".preinit_array") || !strcmp (s->name, ".ctors")
      || !strcmp (s->name, ".dtors") || !strncmp (s->name, ".ctors.", 7)
      || !strncmp (s->name, ".dtors.", 7)
      || !strncmp (s->name, ".init_array.", 12)
      || !strncmp (s->name, ".fini_array.", 12))
    {
      if (s->size)
	die ("%s: %s: constructors and destructors are not supported by l68",
	     o->file, s->name);
      return A_NONE;
    }
  if (!strncmp (s->name, ".remote.bss", 11))
    return A_RBSS;
  if (!strncmp (s->name, ".remote.data", 12))
    return A_RDATA;
  if (s->type == SHT_NOBITS)
    return A_BSS;
  if (s->flags & SHF_WRITE)
    return A_DATA;
  return A_CODE;
}

/* Store V in SIZE bytes at P, checking that it fits (signed if SIGNED_ONLY,
   else signed or unsigned).  */
static void
store (Obj *o, unsigned char *p, int size, int32_t v, int signed_only,
       const char *what, uint32_t where)
{
  int64_t lo = size == 1 ? -128 : size == 2 ? -32768 : INT32_MIN;
  int64_t hi = size == 1 ? (signed_only ? 127 : 255)
	       : size == 2 ? (signed_only ? 32767 : 65535) : UINT32_MAX;
  if (size < 4 && (v < lo || v > hi))
    die ("%s: %s at 0x%x: value %d doesn't fit in %d byte%s", o->file, what,
	 (unsigned) where, (int) v, size, size > 1 ? "s" : "");
  if (size == 4)
    {
      p[0] = v >> 24;
      p[1] = v >> 16;
      p += 2;
    }
  if (size >= 2)
    *p++ = v >> 8;
  *p = v;
}

/* Convert the ELF object O to a ROF psect named NAME, appended to OUT.  */
static void
elf_to_rof (Obj *o, const char *name, Buf *out)
{
  const unsigned char *eh = at (o, 0, 52);
  uint32_t shoff, i, j, nsecs, nsyms = 0;
  Sec *secs;
  Sym *syms = NULL;
  Buf area[A_NUM] = { { 0 } };
  uint32_t asize[A_NUM] = { 0 };
  Ext *exts = NULL;
  int nexts = 0, nlocs = 0, ndefs = 0;
  Ref *locs = NULL;
  uint32_t tylan = 0, attrev = 0, edition = 0, stack = 0, entry = 0,
	   trapent = 0xFFFFFFFF;
  Def *defs = NULL;
  time_t now;
  struct tm *tm;
  const char *sde;

  if (memcmp (eh, "\177ELF", 4) || eh[4] != 1 || eh[5] != 2)
    die ("%s: not a 32-bit big-endian ELF file", o->file);
  if (get16 (eh + 16) != 1 || get16 (eh + 18) != 4)
    die ("%s: not an m68k ELF relocatable", o->file);
  shoff = get32 (eh + 32);
  nsecs = get16 (eh + 48);
  if (get16 (eh + 46) != 40)
    die ("%s: unexpected section header size", o->file);

  /* Sections.  */
  secs = xmalloc (nsecs * sizeof (Sec));
  for (i = 0; i < nsecs; i++)
    {
      const unsigned char *sh = at (o, shoff + i * 40, 40);
      secs[i].type = get32 (sh + 4);
      secs[i].flags = get32 (sh + 8);
      secs[i].offset = get32 (sh + 16);
      secs[i].size = get32 (sh + 20);
      secs[i].link = get32 (sh + 24);
      secs[i].info = get32 (sh + 28);
      secs[i].align = get32 (sh + 32);
      secs[i].entsize = get32 (sh + 36);
      secs[i].nameoff = get32 (sh);
    }
  {
    uint32_t shstrndx = get16 (eh + 50);
    Sec *ss;
    if (shstrndx >= nsecs)
      die ("%s: bad section name table", o->file);
    ss = &secs[shstrndx];
    for (i = 0; i < nsecs; i++)
      {
	const char *n = (const char *) at (o, ss->offset, ss->size);
	if (secs[i].nameoff >= ss->size
	    || !memchr (n + secs[i].nameoff, 0, ss->size - secs[i].nameoff))
	  die ("%s: bad section name", o->file);	/* also unterminated */
	secs[i].name = n + secs[i].nameoff;
      }
  }

  /* Lay out the areas: each section at its alignment (at least 2, as
     l68 places psects at even addresses).  */
  for (i = 1; i < nsecs; i++)
    {
      Sec *s = &secs[i];
      uint32_t align;
      s->area = section_area (o, s);
      if (s->area == A_NONE)
	continue;
      align = s->align > 1 ? s->align : 1;
      asize[s->area] = (asize[s->area] + align - 1) & ~(align - 1);
      s->base = asize[s->area];
      asize[s->area] += s->size;
      if (s->area == A_CODE || s->area == A_DATA || s->area == A_RDATA)
	{
	  if (area[s->area].len < s->base)
	    buf_space (&area[s->area], s->base - area[s->area].len);
	  if (s->type != SHT_NOBITS)
	    buf_add (&area[s->area], at (o, s->offset, s->size), s->size);
	  else
	    buf_space (&area[s->area], s->size);
	}
    }

  /* Symbols; common symbols go to the uninitialized data.  */
  for (i = 1; i < nsecs; i++)
    if (secs[i].type == SHT_SYMTAB)
      {
	Sec *s = &secs[i], *st;
	const char *strs;
	if (syms)
	  die ("%s: more than one symbol table", o->file);
	if (s->link >= nsecs || s->entsize != 16)
	  die ("%s: bad symbol table", o->file);
	st = &secs[s->link];
	strs = (const char *) at (o, st->offset, st->size);
	nsyms = s->size / 16;
	syms = xmalloc (nsyms * sizeof (Sym));
	for (j = 0; j < nsyms; j++)
	  {
	    const unsigned char *e = at (o, s->offset + j * 16, 16);
	    Sym *y = &syms[j];
	    uint32_t nm = get32 (e);
	    if (nm >= st->size || !memchr (strs + nm, 0, st->size - nm))
	      die ("%s: bad symbol name", o->file);	/* also unterminated */
	    y->name = strs + nm;
	    y->value = get32 (e + 4);
	    y->size = get32 (e + 8);
	    y->bind = e[12] >> 4;
	    y->type = e[12] & 0xF;
	    y->shndx = get16 (e + 14);
	    y->area = A_NONE;
	    /* Common symbols (SHN_COMMON) become ROF common definitions,
	       which l68 merges across psects; they stay out of the areas,
	       and references to them are by name.  */
	    if (y->shndx != SHN_UNDEF && y->shndx != SHN_ABS
		     && y->shndx < nsecs && secs[y->shndx].area != A_NONE)
	      {
		y->area = secs[y->shndx].area;
		y->offset = secs[y->shndx].base + y->value;
	      }
	  }
      }

  /* Every area has an even size.  */
  for (i = 0; i < A_NUM; i++)
    {
      asize[i] = (asize[i] + 1) & ~1u;
      if (i == A_CODE || i == A_DATA || i == A_RDATA)
	if (area[i].len < asize[i])
	  buf_space (&area[i], asize[i] - area[i].len);
    }

  /* Relocations.  */
  for (i = 1; i < nsecs; i++)
    {
      Sec *rs = &secs[i], *s;
      if (rs->type == SHT_REL)
	die ("%s: %s: REL relocations are not supported", o->file, rs->name);
      if (rs->type != SHT_RELA)
	continue;
      if (rs->info >= nsecs || rs->link >= nsecs || rs->entsize != 12)
	die ("%s: %s: bad relocation section", o->file, rs->name);
      s = &secs[rs->info];
      if (s->area == A_NONE)
	continue;		/* debug information, discarded sections */
      if (s->area == A_BSS || s->area == A_RBSS)
	die ("%s: relocations in %s", o->file, s->name);
      for (j = 0; j < rs->size / 12; j++)
	{
	  const unsigned char *e = at (o, rs->offset + j * 12, 12);
	  uint32_t roff = get32 (e), info = get32 (e + 4);
	  int32_t addend = (int32_t) get32 (e + 8);
	  unsigned type = info & 0xFF, symi = info >> 8;
	  int la = s->area, size, rel;
	  uint32_t lo = s->base + roff;
	  unsigned loc = la == A_CODE ? R_LOC_CODE
			 : la == A_RDATA ? R_LOC_RDATA : R_LOC_DATA;
	  unsigned char *p;
	  Sym *y;
	  Ref r;

	  switch (type)
	    {
	    case R_68K_NONE:
	      continue;
	    case R_68K_32: size = 4; rel = 0; break;
	    case R_68K_16: size = 2; rel = 0; break;
	    case R_68K_8: size = 1; rel = 0; break;
	    /* Calls through the PLT are PC-relative in a static link, as GNU
	       ld treats them.  */
	    case R_68K_PC32: case R_68K_PLT32: size = 4; rel = 1; break;
	    case R_68K_PC16: case R_68K_PLT16: size = 2; rel = 1; break;
	    case R_68K_PC8: case R_68K_PLT8: size = 1; rel = 1; break;
	    default:
	      die ("%s: %s+0x%x: unsupported relocation type %u", o->file,
		   s->name, (unsigned) roff, type);
	    }
	  if (roff > s->size || (uint32_t) size > s->size - roff)
	    die ("%s: %s: relocation outside the section", o->file, s->name);
	  if (symi >= nsyms && symi != 0)
	    die ("%s: %s: bad symbol in relocation", o->file, s->name);
	  if (la != A_CODE && (size != 4 || rel))
	    die ("%s: %s+0x%x: only 32-bit absolute references are possible "
		 "in data", o->file, s->name, (unsigned) roff);
	  p = area[la].buf + lo;
	  y = symi ? &syms[symi] : NULL;
	  r.offset = lo;

	  if (y && (y->shndx == SHN_UNDEF || y->shndx == SHN_COMMON))
	    {
	      /* An external reference; the addend goes into the bytes.  */
	      int k;
	      if (y->bind == STB_WEAK)
		die ("%s: weak undefined symbol %s: not supported by l68",
		     o->file, y->name);
	      r.type = loc | (size == 1 ? R_BYTE : size == 2 ? R_WORD : R_LONG)
		       | (rel ? R_REL : 0);
	      if (rel && size == 4)
		{
		  /* l68 doesn't compute 4-byte relative references; like
		     Microware's assembler, add a negated reference to this
		     psect's code instead: S - code + (A - offset).  */
		  Ref n;
		  store (o, p, size, addend - (int32_t) lo, 0, y->name, lo);
		  r.type &= ~R_REL;
		  n.type = T_CODE | loc | R_LONG | R_NEG;
		  n.offset = lo;
		  locs = realloc (locs, (nlocs + 1) * sizeof (Ref));
		  locs[nlocs++] = n;
		}
	      else
		store (o, p, size, addend, rel, y->name, lo);
	      for (k = 0; k < nexts && strcmp (exts[k].name, y->name); k++)
		;
	      if (k == nexts)
		{
		  exts = realloc (exts, (nexts + 1) * sizeof (Ext));
		  exts[k].name = y->name;
		  exts[k].nrefs = 0;
		  exts[k].refs = NULL;
		  nexts++;
		}
	      exts[k].refs = realloc (exts[k].refs,
				      (exts[k].nrefs + 1) * sizeof (Ref));
	      exts[k].refs[exts[k].nrefs++] = r;
	    }
	  else if (!y || y->shndx == SHN_ABS)
	    {
	      /* A constant: resolved here.  */
	      if (rel)
		die ("%s: %s+0x%x: PC-relative reference to an absolute "
		     "value", o->file, s->name, (unsigned) roff);
	      store (o, p, size, (y ? y->value : 0) + addend, 0,
		     y ? y->name : "constant", lo);
	    }
	  else if (y->area == A_NONE)
	    die ("%s: %s+0x%x: reference to %s, in a section that isn't "
		 "converted", o->file, s->name, (unsigned) roff, y->name);
	  else
	    {
	      int32_t v = y->offset + addend;
	      if (rel)
		{
		  /* PC-relative within the code: resolved here.  */
		  if (y->area != A_CODE || la != A_CODE)
		    die ("%s: %s+0x%x: PC-relative reference to %s", o->file,
			 s->name, (unsigned) roff, areanames[y->area]);
		  store (o, p, size, v - (int32_t) lo, 1, y->name, lo);
		  continue;
		}
	      if (la == A_CODE && y->area == A_CODE)
		die ("%s: %s+0x%x: absolute reference to code from code "
		     "(not position-independent)", o->file, s->name,
		     (unsigned) roff);
	      /* A local reference; the target's offset goes into the
		 bytes.  */
	      store (o, p, size, v, 0, y->name, lo);
	      r.type = areakind[y->area] | loc
		       | (size == 1 ? R_BYTE : size == 2 ? R_WORD : R_LONG);
	      locs = realloc (locs, (nlocs + 1) * sizeof (Ref));
	      locs[nlocs++] = r;
	    }
	}
    }

  /* Definitions: the global symbols; __os9_* give the header.  */
  for (j = 1; j < nsyms; j++)
    {
      Sym *y = &syms[j];
      uint32_t value;
      unsigned kind;
      if (y->bind == STB_LOCAL || y->shndx == SHN_UNDEF
	  || y->type == STT_SECTION || y->type == STT_FILE)
	continue;
      if (!strncmp (y->name, "__os9_", 6))
	{
	  uint32_t code = y->area == A_CODE ? y->offset : 0;
	  if (!strcmp (y->name, "__os9_tylan"))
	    tylan = y->value;
	  else if (!strcmp (y->name, "__os9_attrev"))
	    attrev = y->value;
	  else if (!strcmp (y->name, "__os9_edition"))
	    edition = y->value;
	  else if (!strcmp (y->name, "__os9_stack"))
	    stack = y->value;
	  else if (!strcmp (y->name, "__os9_entry"))
	    entry = code;
	  else if (!strcmp (y->name, "__os9_trapent"))
	    trapent = code;
	  continue;
	}
      if (y->shndx == SHN_ABS)
	{
	  kind = T_EQU;
	  value = y->value;
	}
      else if (y->shndx == SHN_COMMON)
	{
	  kind = T_COMMON | T_BSS;	/* the value is the size */
	  value = y->size;
	}
      else if (y->area != A_NONE)
	{
	  kind = areakind[y->area];
	  value = y->offset;
	}
      else
	die ("%s: %s is defined in a section that isn't converted", o->file,
	     y->name);
      defs = realloc (defs, (ndefs + 1) * sizeof (Def));
      defs[ndefs].name = y->name;
      defs[ndefs].kind = kind;
      defs[ndefs].value = value;
      ndefs++;
    }

  if (ndefs)
    qsort (defs, ndefs, sizeof (Def), cmp_def);
  if (nexts)
    qsort (exts, nexts, sizeof (Ext), cmp_ext);
  if (nlocs)
    qsort (locs, nlocs, sizeof (Ref), cmp_loc);
  if (ndefs > 0xFFFF || nexts > 0xFFFF || nlocs > 0xFFFF)
    die ("%s: too many symbols or references for ROF edition 9", o->file);

  /* The ROF.  */
  sde = getenv ("SOURCE_DATE_EPOCH");
  now = sde ? (time_t) strtoll (sde, NULL, 10) : time (NULL);
  tm = sde ? gmtime (&now) : localtime (&now);
  add32 (out, 0xDEADFACE);
  add16 (out, tylan);
  add16 (out, attrev);
  add16 (out, 0);		/* assembly valid */
  add16 (out, 9);		/* series: ROF edition 9 */
  {
    unsigned char d[6] = { tm->tm_year, tm->tm_mon + 1, tm->tm_mday,
			   tm->tm_hour, tm->tm_min, tm->tm_sec };
    buf_add (out, d, 6);
  }
  add16 (out, edition);
  add32 (out, asize[A_BSS]);
  add32 (out, asize[A_DATA]);
  add32 (out, asize[A_CODE]);
  add32 (out, stack);
  add32 (out, entry);
  add32 (out, trapent);
  add32 (out, asize[A_RBSS]);
  add32 (out, asize[A_RDATA]);
  add32 (out, 0);		/* debug information */
  add_name (out, name);
  add16 (out, ndefs);
  for (i = 0; i < (uint32_t) ndefs; i++)
    {
      add_name (out, defs[i].name);
      add16 (out, defs[i].kind);
      add32 (out, defs[i].value);
    }
  buf_add (out, area[A_CODE].buf, asize[A_CODE]);
  buf_add (out, area[A_DATA].buf, asize[A_DATA]);
  buf_add (out, area[A_RDATA].buf, asize[A_RDATA]);
  add16 (out, nexts);
  for (i = 0; i < (uint32_t) nexts; i++)
    {
      int k;
      if (exts[i].nrefs > 0xFFFF)
	die ("%s: too many references to %s for ROF edition 9", o->file,
	     exts[i].name);
      add_name (out, exts[i].name);
      add16 (out, exts[i].nrefs);
      for (k = 0; k < exts[i].nrefs; k++)
	{
	  add16 (out, exts[i].refs[k].type);
	  add32 (out, exts[i].refs[k].offset);
	}
    }
  add16 (out, nlocs);
  for (i = 0; i < (uint32_t) nlocs; i++)
    {
      add16 (out, locs[i].type);
      add32 (out, locs[i].offset);
    }
  /* Microware's assemblers end every ROF with 16 zero bytes, which the
     manuals don't describe; rdump and l68 skip them, so the ROFs in a
     library must have them.  */
  buf_space (out, 16);
}

static unsigned char *
read_file (const char *name, long *size)
{
  FILE *f = fopen (name, "rb");
  unsigned char *data;
  if (!f)
    die ("can't open %s", name);
  fseek (f, 0, SEEK_END);
  *size = ftell (f);
  fseek (f, 0, SEEK_SET);
  data = xmalloc (*size);
  if (fread (data, 1, *size, f) != (size_t) *size)
    die ("can't read %s", name);
  fclose (f);
  return data;
}

/* The psect name for FILE: its base name without the extension.  */
static char *
psect_name (const char *file)
{
  const char *b = strrchr (file, '/');
  char *n, *dot;
  n = strdup (b ? b + 1 : file);
  dot = strrchr (n, '.');
  if (dot && dot != n)
    *dot = '\0';
  return n;
}

/* Convert the archive member O, appending to OUT; with KEEP_GOING, skip it
   (with the warning from die) if it can't be converted.  Return 1 if it
   was converted.  */
static int
convert_member (Obj *o, Buf *out, int keep_going)
{
  jmp_buf jb;
  size_t len = out->len;
  if (keep_going && setjmp (jb))
    {
      skip_member = NULL;
      /* Drop the partial ROF; buf_space hands out zeroed space.  */
      memset (out->buf + len, 0, out->len - len);
      out->len = len;
      return 0;
    }
  skip_member = keep_going ? &jb : NULL;
  elf_to_rof (o, psect_name (o->file), out);
  skip_member = NULL;
  return 1;
}

static void
usage (void)
{
  fprintf (stderr,
	   "usage: elf2rof [-k] [-n NAME] [-o OUTPUT] INPUT\n"
	   "  INPUT   an m68k ELF relocatable (.o) or an ar archive of them (.a)\n"
	   "  -o      the output: a ROF (default INPUT.r) or, from an archive, a\n"
	   "          library of ROFs (default INPUT.l)\n"
	   "  -n      the psect name (default: INPUT's base name; for an\n"
	   "          archive, each member's)\n"
	   "  -k      skip archive members that can't be converted, with a\n"
	   "          warning\n");
  exit (2);
}

int
main (int argc, char **argv)
{
  const char *input = NULL, *output = NULL, *name = NULL;
  unsigned char *data;
  long size;
  int i, archive, keep_going = 0;
  Buf out = { 0 };
  FILE *f;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "-o") && i + 1 < argc)
	output = argv[++i];
      else if (!strcmp (argv[i], "-n") && i + 1 < argc)
	name = argv[++i];
      else if (!strcmp (argv[i], "-k"))
	keep_going = 1;
      else if (argv[i][0] == '-' || input)
	usage ();
      else
	input = argv[i];
    }
  if (!input)
    usage ();

  data = read_file (input, &size);
  archive = size >= 8 && !memcmp (data, "!<arch>\n", 8);

  if (!archive)
    {
      Obj o = { input, data, size };
      elf_to_rof (&o, name ? name : psect_name (input), &out);
    }
  else
    {
      /* An ar archive: each ELF member becomes a ROF in the library.  */
      long pos = 8;
      const char *longnames = NULL;
      long nlong = 0;
      int n = 0;
      if (name)
	die ("-n is for a single object, not an archive");
      while (pos + 60 <= size)
	{
	  const unsigned char *h = data + pos;
	  char field[17], *mname;
	  long msize;
	  memcpy (field, h + 48, 10);
	  field[10] = '\0';
	  msize = strtol (field, NULL, 10);
	  if (memcmp (h + 58, "`\n", 2) || msize < 0 || pos + 60 + msize > size)
	    die ("%s: bad archive member header", input);
	  memcpy (field, h, 16);
	  field[16] = '\0';
	  if (!strncmp (field, "// ", 3) || !strcmp (field, "//              "))
	    {
	      longnames = (const char *) h + 60;
	      nlong = msize;
	    }
	  else if (field[0] == '/' && (field[1] == ' ' || !strncmp (field,
								   "/SYM64/", 7)))
	    ;			/* symbol table */
	  else
	    {
	      Obj o;
	      if (field[0] == '/')
		{
		  long off = strtol (field + 1, NULL, 10);
		  const char *e;
		  if (!longnames || off < 0 || off >= nlong)
		    die ("%s: bad long member name", input);
		  e = memchr (longnames + off, '/', nlong - off);
		  if (!e)
		    die ("%s: bad long member name", input);
		  mname = xmalloc (e - (longnames + off) + 1);
		  memcpy (mname, longnames + off, e - (longnames + off));
		}
	      else
		{
		  char *e = strchr (field, '/');
		  if (!e)
		    e = strchr (field, ' ');
		  if (e)
		    *e = '\0';
		  mname = strdup (field);
		}
	      o.file = mname;
	      o.p = h + 60;
	      o.size = msize;
	      n += convert_member (&o, &out, keep_going);
	    }
	  pos += 60 + msize + (msize & 1);
	}
      if (n == 0)
	die ("%s: no objects in the archive", input);
    }

  if (!output)
    {
      char *o = xmalloc (strlen (input) + 3);
      char *dot;
      strcpy (o, input);
      dot = strrchr (o, '.');
      if (!dot || strchr (dot, '/'))
	dot = o + strlen (o);
      strcpy (dot, archive ? ".l" : ".r");
      output = o;
    }
  f = fopen (output, "wb");
  if (!f || fwrite (out.buf, 1, out.len, f) != out.len || fclose (f))
    die ("can't write %s", output);
  return 0;
}
