/*
 * rof2elf: convert Microware OS-9/68000 ROF objects (.r) and libraries
 * (.l, concatenated ROFs) to m68k ELF relocatables and ar archives, for
 * linking with GNU ld and converting with elf2mod.
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
 * The ROF format is described in the OS-9 Assembler/Linker User Manual,
 * chapter 3, "Relocatable Object File Format".  Mapping (see
 * OS9-COMPAT-DESIGN.md in toolchaincdi, section 6):
 *
 *   code                    -> .text
 *   initialized data        -> .data         (a6-relative; the linker script
 *   uninitialized data      -> .bss           places data at -0x8000)
 *   remote init/uninit data -> .remote.data / .remote.bss
 *   debug information       -> dropped
 *   equ definitions         -> absolute symbols
 *   common definitions      -> ELF common symbols
 *   references              -> RELA relocations, R_68K_{8,16,32} or
 *                              R_68K_PC{8,16,32}; the addend is taken from
 *                              the object bytes
 *
 * A reference to an external symbol paired with a negated reference to
 * this ROF's code at the same place (S - code base + value, as cstart.r
 * uses for 32-bit PC-relative values) becomes a PC-relative relocation.
 *
 * Mainline ROFs (with a module type) also get absolute symbols carrying
 * the module header values: __os9_tylan, __os9_attrev, __os9_edition,
 * __os9_stack, and code symbols __os9_entry and __os9_trapent.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ROF reference and definition type bits.  */
#define T_KIND		0x0007	/* definition kind / reference target */
#define T_BSS		0x0000
#define T_DATA		0x0001
#define T_RBSS		0x0002
#define T_RDATA		0x0003
#define T_CODE		0x0004
#define T_EQU		0x0006
#define T_COMMON	0x0100
#define R_SIZE		0x0018	/* reference size */
#define R_BYTE		0x0008
#define R_WORD		0x0010
#define R_LONG		0x0018
#define R_LOC		0x0220	/* where the reference is */
#define R_LOC_DATA	0x0000
#define R_LOC_CODE	0x0020
#define R_LOC_RDATA	0x0200
#define R_LOC_DEBUG	0x0220
#define R_NEG		0x0040
#define R_REL		0x0080

/* ELF.  */
#define R_68K_32	1
#define R_68K_16	2
#define R_68K_8		3
#define R_68K_PC32	4
#define R_68K_PC16	5
#define R_68K_PC8	6

static const char *progname = "rof2elf";

static void
die (const char *fmt, const char *arg)
{
  fprintf (stderr, "%s: ", progname);
  fprintf (stderr, fmt, arg);
  fputc ('\n', stderr);
  exit (1);
}

static void *
xmalloc (size_t n)
{
  void *p = calloc (1, n ? n : 1);
  if (!p)
    die ("out of memory%s", "");
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
	die ("out of memory%s", "");
      memset (b->buf + b->cap, 0, cap - b->cap);
      b->cap = cap;
    }
  b->len += n;
  return at;
}

static size_t
buf_add (Buf *b, const void *p, size_t n)
{
  size_t at = buf_space (b, n);
  memcpy (b->buf + at, p, n);
  return at;
}

static void
put16 (unsigned char *p, unsigned v)
{
  p[0] = v >> 8;
  p[1] = v;
}

static void
put32 (unsigned char *p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static void
add32 (Buf *b, uint32_t v)
{
  size_t at = buf_space (b, 4);
  put32 (b->buf + at, v);
}

static void
add8 (Buf *b, unsigned v)
{
  size_t at = buf_space (b, 1);
  b->buf[at] = v;
}

/* Input reading.  */
typedef struct
{
  const unsigned char *p, *end;
  const char *file;
} In;

static void
need (In *in, size_t n)
{
  if ((size_t) (in->end - in->p) < n)
    die ("%s: truncated ROF", in->file);
}

static unsigned
get16 (In *in)
{
  need (in, 2);
  in->p += 2;
  return (in->p[-2] << 8) | in->p[-1];
}

static uint32_t
get32 (In *in)
{
  need (in, 4);
  in->p += 4;
  return ((uint32_t) in->p[-4] << 24) | ((uint32_t) in->p[-3] << 16)
	 | (in->p[-2] << 8) | in->p[-1];
}

static uint32_t
getcount (In *in, int wide)
{
  return wide ? get32 (in) : get16 (in);
}

static char *
getname (In *in)
{
  const unsigned char *s = in->p;
  while (in->p < in->end && *in->p)
    in->p++;
  need (in, 1);
  in->p++;
  return strdup ((const char *) s);
}

/* One parsed ROF.  */
typedef struct
{
  char *name;
  unsigned type;
  uint32_t value;
} Def;

typedef struct
{
  unsigned type;
  uint32_t offset;
  int ext;		/* index of the external symbol, or -1 */
} Ref;

typedef struct
{
  char *name;
  unsigned tylan, attrev, edition;
  uint32_t statics, idata, code, stack, entry, trapent, rstatics, ridata,
	   debug;
  const unsigned char *codep, *datap, *rdatap;
  int ndefs;
  Def *defs;
  int nexts;
  char **exts;
  int nrefs;
  Ref *refs;
} Rof;

static int
parse_rof (In *in, Rof *r)
{
  int i, j, n, wide;
  unsigned series;

  memset (r, 0, sizeof *r);
  if (get32 (in) != 0xDEADFACE)
    return 0;
  r->tylan = get16 (in);
  r->attrev = get16 (in);
  if (get16 (in) != 0)
    die ("%s: ROF has assembly errors", in->file);
  /* The series (ROF edition): 9, or 0xF9 for 9.1 (from Ultra C's r68),
     whose counts of symbols and references are 32 bits instead of 16 (see
     "Relocatable Object File Format" in Using Ultra C/C++).  9.2 adds
     header fields and edition 15 is a different format.  */
  series = get16 (in);
  if (series != 9 && series != 0xF9)
    {
      char fmt[64];
      sprintf (fmt, "%%s: unsupported ROF series 0x%04X", series);
      die (fmt, in->file);
    }
  wide = series == 0xF9;
  need (in, 6);
  in->p += 6;			/* date */
  r->edition = get16 (in);
  r->statics = get32 (in);
  r->idata = get32 (in);
  r->code = get32 (in);
  r->stack = get32 (in);
  r->entry = get32 (in);
  r->trapent = get32 (in);
  r->rstatics = get32 (in);
  r->ridata = get32 (in);
  r->debug = get32 (in);
  r->name = getname (in);

  r->ndefs = getcount (in, wide);
  r->defs = xmalloc (r->ndefs * sizeof (Def));
  for (i = 0; i < r->ndefs; i++)
    {
      r->defs[i].name = getname (in);
      r->defs[i].type = get16 (in);
      r->defs[i].value = get32 (in);
    }

  need (in, r->code + r->idata + r->ridata + r->debug);
  r->codep = in->p;
  r->datap = r->codep + r->code;
  r->rdatap = r->datap + r->idata;
  in->p = r->rdatap + r->ridata + r->debug;

  /* External references: a name and a list of places.  */
  r->nexts = getcount (in, wide);
  r->exts = xmalloc (r->nexts * sizeof (char *));
  r->nrefs = 0;
  {
    const unsigned char *save = in->p;
    for (i = 0; i < r->nexts; i++)
      {
	free (getname (in));
	n = getcount (in, wide);
	need (in, n * 6);
	in->p += n * 6;
	r->nrefs += n;
      }
    n = getcount (in, wide);	/* local references */
    r->nrefs += n;
    in->p = save;
  }
  r->refs = xmalloc (r->nrefs * sizeof (Ref));
  for (i = 0, j = 0; i < r->nexts; i++)
    {
      r->exts[i] = getname (in);
      for (n = getcount (in, wide); n > 0; n--, j++)
	{
	  r->refs[j].type = get16 (in);
	  r->refs[j].offset = get32 (in);
	  r->refs[j].ext = i;
	}
    }
  for (n = getcount (in, wide); n > 0; n--, j++)
    {
      r->refs[j].type = get16 (in);
      r->refs[j].offset = get32 (in);
      r->refs[j].ext = -1;
    }

  /* Skip padding up to the next ROF.  */
  while (in->p < in->end && *in->p == 0)
    in->p++;
  return 1;
}

/* ELF output.  Sections, in this order.  */
enum
{
  S_NULL, S_TEXT, S_DATA, S_BSS, S_RDATA, S_RBSS, S_RELTEXT, S_RELDATA,
  S_RELRDATA, S_SYMTAB, S_STRTAB, S_SHSTRTAB, S_NUM
};

static const char *const secnames[S_NUM] = {
  "", ".text", ".data", ".bss", ".remote.data", ".remote.bss",
  ".rela.text", ".rela.data", ".rela.remote.data", ".symtab", ".strtab",
  ".shstrtab"
};

typedef struct
{
  Buf syms, strs, rel[3], text, data, rdata;
  int nsyms, nlocal;
  int *extsym;			/* ELF symbol of each external name */
} Elf;

static int
add_sym (Elf *e, const char *name, uint32_t value, uint32_t size,
	 unsigned info, unsigned shndx)
{
  size_t at = buf_space (&e->syms, 16);
  unsigned char *p = e->syms.buf + at;
  uint32_t nameoff = 0;
  if (name && *name)
    nameoff = buf_add (&e->strs, name, strlen (name) + 1);
  put32 (p, nameoff);
  put32 (p + 4, value);
  put32 (p + 8, size);
  p[12] = info;
  p[13] = 0;
  put16 (p + 14, shndx);
  return e->nsyms++;
}

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2
#define INFO(b, t) (((b) << 4) | (t))

/* Section symbol index for a reference target kind.  */
static int
target_section (unsigned kind)
{
  switch (kind)
    {
    case T_BSS: return S_BSS;
    case T_DATA: return S_DATA;
    case T_RBSS: return S_RBSS;
    case T_RDATA: return S_RDATA;
    case T_CODE: return S_TEXT;
    default: return -1;
    }
}

static int
loc_section (unsigned type)
{
  switch (type & R_LOC)
    {
    case R_LOC_CODE: return S_TEXT;
    case R_LOC_DATA: return S_DATA;
    case R_LOC_RDATA: return S_RDATA;
    default: return -1;		/* debug */
    }
}

static int
cmp_ref (const void *a, const void *b)
{
  const Ref *x = a, *y = b;
  int lx = loc_section (x->type), ly = loc_section (y->type);
  if (lx != ly)
    return lx - ly;
  return x->offset < y->offset ? -1 : x->offset > y->offset;
}

static void
add_rela (Elf *e, int sec, uint32_t offset, int sym, int type, int32_t addend)
{
  Buf *b = &e->rel[sec == S_TEXT ? 0 : sec == S_DATA ? 1 : 2];
  add32 (b, offset);
  add32 (b, ((uint32_t) sym << 8) | type);
  add32 (b, (uint32_t) addend);
}

/* Known equates (equ definitions), from the input and from -e files.
   References to them are folded into the object bytes, since ELF can't
   add several symbols in one relocation (e.g. Dir_+Write_ in a mode
   byte).  */
typedef struct
{
  char *name;
  uint32_t value;
} Equ;

static Equ *equs;
static int nequs, equs_sorted;

static int
cmp_equ (const void *a, const void *b)
{
  return strcmp (((const Equ *) a)->name, ((const Equ *) b)->name);
}

static void
add_equates (Rof *r)
{
  int i;
  for (i = 0; i < r->ndefs; i++)
    if ((r->defs[i].type & (T_KIND | T_COMMON)) == T_EQU)
      {
	equs = realloc (equs, (nequs + 1) * sizeof (Equ));
	equs[nequs].name = r->defs[i].name;
	equs[nequs].value = r->defs[i].value;
	nequs++;
      }
  equs_sorted = 0;
}

static Equ *
find_equate (const char *name)
{
  Equ key;
  if (!equs_sorted)
    {
      qsort (equs, nequs, sizeof (Equ), cmp_equ);
      equs_sorted = 1;
    }
  key.name = (char *) name;
  return nequs ? bsearch (&key, equs, nequs, sizeof (Equ), cmp_equ) : NULL;
}

/* Convert one ROF to an ELF relocatable in OUT.  */
static void
rof_to_elf (const char *file, Rof *r, Buf *out)
{
  Elf e;
  Buf *secbuf[S_NUM] = { 0 };
  uint32_t secsize[S_NUM] = { 0 };
  int i, k;

  memset (&e, 0, sizeof e);
  add8 (&e.strs, 0);

  buf_add (&e.text, r->codep, r->code);
  buf_add (&e.data, r->datap, r->idata);
  buf_add (&e.rdata, r->rdatap, r->ridata);

  /* Symbols: null, file, section symbols, then globals.  */
  add_sym (&e, NULL, 0, 0, 0, 0);
  add_sym (&e, r->name, 0, 0, INFO (STB_LOCAL, 4 /* STT_FILE */), SHN_ABS);
  for (i = S_TEXT; i <= S_RBSS; i++)
    add_sym (&e, NULL, 0, 0, INFO (STB_LOCAL, STT_SECTION), i);
  e.nlocal = e.nsyms;
  /* Section symbol of section S is S + 1.  */

  for (i = 0; i < r->ndefs; i++)
    {
      Def *d = &r->defs[i];
      unsigned kind = d->type & T_KIND;
      if (d->type & T_COMMON)
	add_sym (&e, d->name, 2, d->value, INFO (STB_GLOBAL, STT_OBJECT),
		 SHN_COMMON);
      else if (kind == T_EQU)
	add_sym (&e, d->name, d->value, 0, INFO (STB_GLOBAL, STT_NOTYPE),
		 SHN_ABS);
      else if (target_section (kind) >= 0)
	add_sym (&e, d->name, d->value, 0,
		 INFO (STB_GLOBAL, kind == T_CODE ? STT_FUNC : STT_OBJECT),
		 target_section (kind));
      else
	{
	  fprintf (stderr, "%s: %s: symbol %s has unknown type 0x%04x\n",
		   progname, file, d->name, d->type);
	  exit (1);
	}
    }

  if (r->tylan != 0)
    {
      add_sym (&e, "__os9_tylan", r->tylan, 0, INFO (STB_GLOBAL, 0), SHN_ABS);
      add_sym (&e, "__os9_attrev", r->attrev, 0, INFO (STB_GLOBAL, 0),
	       SHN_ABS);
      add_sym (&e, "__os9_edition", r->edition, 0, INFO (STB_GLOBAL, 0),
	       SHN_ABS);
      add_sym (&e, "__os9_stack", r->stack, 0, INFO (STB_GLOBAL, 0),
	       SHN_ABS);
      add_sym (&e, "__os9_entry", r->entry, 0, INFO (STB_GLOBAL, STT_FUNC),
	       S_TEXT);
      add_sym (&e, "__os9_trapent", r->trapent, 0,
	       INFO (STB_GLOBAL, STT_FUNC), S_TEXT);
    }

  /* Undefined symbols for the external references.  */
  e.extsym = xmalloc (r->nexts * sizeof (int));
  for (i = 0; i < r->nexts; i++)
    e.extsym[i] = add_sym (&e, r->exts[i], 0, 0, INFO (STB_GLOBAL, 0), 0);

  /* References, grouped by location.  */
  qsort (r->refs, r->nrefs, sizeof (Ref), cmp_ref);
  for (i = 0; i < r->nrefs; i = k)
    {
      Ref *pos = NULL, *neg = NULL;
      int sec = loc_section (r->refs[i].type), size, n, sym, type;
      uint32_t off = r->refs[i].offset;
      unsigned rel;
      Buf *b;
      int32_t addend;
      unsigned char *p;

      for (k = i; k < r->nrefs && loc_section (r->refs[k].type) == sec
		  && r->refs[k].offset == off; k++)
	;
      if (sec < 0)
	continue;		/* references in debug info */

      size = (r->refs[i].type & R_SIZE) == R_BYTE ? 1
	     : (r->refs[i].type & R_SIZE) == R_WORD ? 2 : 4;
      for (n = i; n < k; n++)
	if ((r->refs[n].type & R_SIZE) != (r->refs[i].type & R_SIZE))
	  goto unsupported;

      b = sec == S_TEXT ? &e.text : sec == S_DATA ? &e.data : &e.rdata;
      if (off + size > b->len)
	goto unsupported;
      p = b->buf + off;
      addend = size == 1 ? (int8_t) p[0]
	       : size == 2 ? (int16_t) ((p[0] << 8) | p[1])
	       : (int32_t) (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
			    | (p[2] << 8) | p[3]);

      /* Fold known equates into the value; at most one other term and
	 one negated code term can remain.  */
      for (n = i; n < k; n++)
	{
	  Ref *x = &r->refs[n];
	  Equ *q;
	  if (x->ext >= 0 && !(x->type & R_REL)
	      && (q = find_equate (r->exts[x->ext])) != NULL)
	    addend += (x->type & R_NEG) ? -(int32_t) q->value
					: (int32_t) q->value;
	  else if (x->type & R_NEG)
	    {
	      if (neg)
		goto unsupported;
	      neg = x;
	    }
	  else
	    {
	      if (pos)
		goto unsupported;
	      pos = x;
	    }
	}
      if (!pos && !neg)
	{
	  /* Only equates: the value is final.  */
	  for (n = size - 1; n >= 0; n--, addend >>= 8)
	    p[n] = addend;
	  continue;
	}
      if (!pos)
	goto unsupported;
      rel = pos->type & R_REL;
      if (pos->ext >= 0)
	sym = e.extsym[pos->ext];
      else if (target_section (pos->type & T_KIND) >= 0)
	sym = target_section (pos->type & T_KIND) + 1;
      else
	goto unsupported;

      if (neg)
	{
	  /* S - (this ROF's code) + value: a PC-relative value, when the
	     reference is in the code.  */
	  if (neg->ext >= 0 || (neg->type & T_KIND) != T_CODE
	      || (neg->type & R_REL) || rel || sec != S_TEXT)
	    goto unsupported;
	  if (pos->ext < 0 && (pos->type & T_KIND) == T_CODE)
	    continue;		/* code - code: a constant, already there */
	  rel = R_REL;
	  addend += off;
	}

      if (rel)
	type = size == 1 ? R_68K_PC8 : size == 2 ? R_68K_PC16 : R_68K_PC32;
      else
	type = size == 1 ? R_68K_8 : size == 2 ? R_68K_16 : R_68K_32;
      add_rela (&e, sec, off, sym, type, addend);
      memset (p, 0, size);
      continue;

    unsupported:
      fprintf (stderr, "%s: %s: unsupported reference combination at "
	       "%s+0x%x (type 0x%04x)%s\n", progname, file,
	       sec >= 0 ? secnames[sec] : ".debug", (unsigned) off,
	       r->refs[i].type,
	       nequs ? "" : "; equates from sys.l (-e) may resolve it");
      exit (1);
    }

  /* Assemble the ELF file.  */
  secbuf[S_TEXT] = &e.text;
  secbuf[S_DATA] = &e.data;
  secbuf[S_RDATA] = &e.rdata;
  secbuf[S_RELTEXT] = &e.rel[0];
  secbuf[S_RELDATA] = &e.rel[1];
  secbuf[S_RELRDATA] = &e.rel[2];
  secbuf[S_SYMTAB] = &e.syms;
  secbuf[S_STRTAB] = &e.strs;
  secsize[S_BSS] = r->statics;
  secsize[S_RBSS] = r->rstatics;

  {
    Buf shstr = { 0 };
    uint32_t nameoff[S_NUM], offset[S_NUM];
    size_t ehdr, shoff;
    unsigned char *h;

    add8 (&shstr, 0);
    for (i = 1; i < S_NUM; i++)
      nameoff[i] = buf_add (&shstr, secnames[i], strlen (secnames[i]) + 1);
    secbuf[S_SHSTRTAB] = &shstr;

    out->len = 0;
    ehdr = buf_space (out, 52);
    for (i = 1; i < S_NUM; i++)
      {
	while (out->len % 4)
	  add8 (out, 0);
	offset[i] = out->len;
	if (secbuf[i])
	  {
	    if (secbuf[i]->len)
	      buf_add (out, secbuf[i]->buf, secbuf[i]->len);
	    secsize[i] = secbuf[i]->len;
	  }
      }
    while (out->len % 4)
      add8 (out, 0);
    shoff = buf_space (out, S_NUM * 40);

    h = out->buf + ehdr;
    memcpy (h, "\177ELF\1\2\1", 7);	/* 32-bit, big-endian, version 1 */
    put16 (h + 16, 1);			/* ET_REL */
    put16 (h + 18, 4);			/* EM_68K */
    put32 (h + 20, 1);
    put32 (h + 32, shoff);
    put16 (h + 40, 52);
    put16 (h + 46, 40);
    put16 (h + 48, S_NUM);
    put16 (h + 50, S_SHSTRTAB);

    for (i = 1; i < S_NUM; i++)
      {
	unsigned char *s = out->buf + shoff + i * 40;
	uint32_t type = 1, flags = 0, link = 0, info = 0, align = 2,
		 entsize = 0;
	switch (i)
	  {
	  case S_TEXT: flags = 6; break;		/* ALLOC, EXECINSTR */
	  case S_DATA: case S_RDATA: flags = 3; break;	/* WRITE, ALLOC */
	  case S_BSS: case S_RBSS: type = 8; flags = 3; break;
	  case S_RELTEXT: case S_RELDATA: case S_RELRDATA:
	    type = 4; link = S_SYMTAB; flags = 0x40;	/* INFO_LINK */
	    info = i == S_RELTEXT ? S_TEXT : i == S_RELDATA ? S_DATA : S_RDATA;
	    align = 4; entsize = 12;
	    break;
	  case S_SYMTAB:
	    type = 2; link = S_STRTAB; info = e.nlocal; align = 4;
	    entsize = 16;
	    break;
	  case S_STRTAB: case S_SHSTRTAB: type = 3; align = 1; break;
	  }
	put32 (s, nameoff[i]);
	put32 (s + 4, type);
	put32 (s + 8, flags);
	put32 (s + 16, offset[i]);
	put32 (s + 20, secsize[i]);
	put32 (s + 24, link);
	put32 (s + 28, info);
	put32 (s + 32, align);
	put32 (s + 36, entsize);
      }
    free (shstr.buf);
  }

  free (e.syms.buf);
  free (e.strs.buf);
  free (e.text.buf);
  free (e.data.buf);
  free (e.rdata.buf);
  for (i = 0; i < 3; i++)
    free (e.rel[i].buf);
  free (e.extsym);
}

/* Read the whole file NAME.  */
static unsigned char *
read_file (const char *name, long *size)
{
  FILE *f = fopen (name, "rb");
  unsigned char *data;
  if (!f || fseek (f, 0, SEEK_END) || (*size = ftell (f)) < 0)
    die ("can't read %s", name);
  data = xmalloc (*size);
  rewind (f);
  if (fread (data, 1, *size, f) != (size_t) *size)
    die ("can't read %s", name);
  fclose (f);
  return data;
}

/* Load the equates defined in the ROF or library NAME.  */
static void
load_equates (const char *name)
{
  long size;
  In in;
  Rof r;
  in.p = read_file (name, &size);
  in.end = in.p + size;
  in.file = name;
  while (in.p < in.end)
    {
      if (!parse_rof (&in, &r))
	die ("%s: not a ROF (no sync bytes)", name);
      add_equates (&r);
    }
}

static void
usage (void)
{
  fprintf (stderr,
	   "usage: %s [-o OUTPUT] [-e EQUFILE]... INPUT\n"
	   "  Converts a ROF object (.r) to an ELF relocatable, or a ROF\n"
	   "  library (.l) to an ar archive (run ranlib on it).  OUTPUT\n"
	   "  defaults to INPUT with .o or .a.\n"
	   "  -e  also take equ definitions from EQUFILE (a ROF or library,\n"
	   "      e.g. sys.l), to fold references to them into the code\n"
	   "  -l  list the ROFs in INPUT and exit\n", progname);
  exit (2);
}

int
main (int argc, char **argv)
{
  const char *input = NULL, *output = NULL;
  int list = 0, i, nrofs = 0;
  unsigned char *data;
  long size;
  FILE *f;
  In in;
  Rof *rofs = NULL;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "-o") && i + 1 < argc)
	output = argv[++i];
      else if (!strcmp (argv[i], "-e") && i + 1 < argc)
	load_equates (argv[++i]);
      else if (!strcmp (argv[i], "-l"))
	list = 1;
      else if (argv[i][0] == '-' || input)
	usage ();
      else
	input = argv[i];
    }
  if (!input)
    usage ();

  data = read_file (input, &size);

  in.p = data;
  in.end = data + size;
  in.file = input;
  while (in.p < in.end)
    {
      rofs = realloc (rofs, (nrofs + 1) * sizeof (Rof));
      if (!parse_rof (&in, &rofs[nrofs]))
	die ("%s: not a ROF (no sync bytes)", input);
      nrofs++;
    }
  if (nrofs == 0)
    die ("%s: empty", input);
  for (i = 0; i < nrofs; i++)
    add_equates (&rofs[i]);

  if (list)
    {
      for (i = 0; i < nrofs; i++)
	printf ("%s: code=%u idata=%u data=%u rdata=%u ridata=%u debug=%u "
		"defs=%d refs=%d\n", rofs[i].name, rofs[i].code,
		rofs[i].idata, rofs[i].statics, rofs[i].rstatics,
		rofs[i].ridata, rofs[i].debug, rofs[i].ndefs, rofs[i].nrefs);
      return 0;
    }

  if (!output)
    {
      char *o = xmalloc (strlen (input) + 3);
      char *dot;
      strcpy (o, input);
      dot = strrchr (o, '.');
      if (!dot || strchr (dot, '/'))
	dot = o + strlen (o);
      strcpy (dot, nrofs > 1 ? ".a" : ".o");
      output = o;
    }

  f = fopen (output, "wb");
  if (!f)
    die ("can't write %s", output);
  if (nrofs == 1 && !(strlen (output) > 2
		      && !strcmp (output + strlen (output) - 2, ".a")))
    {
      Buf out = { 0 };
      rof_to_elf (input, &rofs[0], &out);
      fwrite (out.buf, 1, out.len, f);
    }
  else
    {
      Buf *objs = xmalloc (nrofs * sizeof (Buf));
      char **names = xmalloc (nrofs * sizeof (char *));
      Buf longnames = { 0 };
      size_t lnoff = 0;

      for (i = 0; i < nrofs; i++)
	{
	  char *n = xmalloc (strlen (rofs[i].name) + 3);
	  sprintf (n, "%s.o", rofs[i].name);
	  names[i] = n;
	  rof_to_elf (input, &rofs[i], &objs[i]);
	}
      fwrite ("!<arch>\n", 1, 8, f);
      for (i = 0; i < nrofs; i++)
	if (strlen (names[i]) >= 16)
	  {
	    buf_add (&longnames, names[i], strlen (names[i]));
	    buf_add (&longnames, "/\n", 2);
	  }
      if (longnames.len)
	{
	  char hdr[80];
	  snprintf (hdr, sizeof hdr, "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", "//",
		    "", "", "", "", longnames.len);
	  fwrite (hdr, 1, 60, f);
	  fwrite (longnames.buf, 1, longnames.len, f);
	  if (longnames.len & 1)
	    fputc ('\n', f);
	}
      for (i = 0; i < nrofs; i++)
	{
	  char nm[24], hdr[80];
	  if (strlen (names[i]) < 16)
	    snprintf (nm, sizeof nm, "%s/", names[i]);
	  else
	    {
	      snprintf (nm, sizeof nm, "/%zu", lnoff);
	      lnoff += strlen (names[i]) + 2;
	    }
	  snprintf (hdr, sizeof hdr, "%-16s%-12d%-6d%-6d%-8o%-10zu`\n", nm, 0,
		    0, 0, 0644, objs[i].len);
	  fwrite (hdr, 1, 60, f);
	  fwrite (objs[i].buf, 1, objs[i].len, f);
	  if (objs[i].len & 1)
	    fputc ('\n', f);
	}
    }
  if (fclose (f))
    die ("can't write %s", output);
  return 0;
}
