/*
 * MIT License
 *
 * Copyright (c) 2021 Murachue
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
 */

#define PACKAGE "bfd" // https://qiita.com/yasuo-ozu/items/4d3dbef6f48808ee110c
#include <bfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

struct modheader {
	uint16_t magic;
	uint16_t sysrev;
	uint32_t size;
	uint32_t owner;
	uint32_t name;
	uint16_t accs;
	uint8_t type;
	uint8_t lang;
	uint8_t attr;
	uint8_t revs;
	uint16_t edit;
	uint32_t usage;
	uint32_t symbol;
	uint8_t reserved[14];
	uint16_t parity;

	uint32_t exec;
	uint32_t excpt;
	uint32_t mem;
	uint32_t stack;
	uint32_t idata;
	uint32_t irefs;
};

#if 1
// le->be
#define BE16(v) ((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8))
#define BE32(v) ((((v) >> 24) & 0xFF) | (((v) >> 8) & 0xFF00) | (((v) & 0xFF00) << 8) | (((v) & 0xFF) << 24))
#else
// be->be
#define BE16(v) v
#define BE32(v) v
#endif

char *opt_name;
// -1: not given; then the value of __os9_stack etc. (from a mainline ROF
// converted by rof2elf, such as cstart.r), or the default.
int opt_stack = -1;
int opt_revs = -1;
int opt_edit = -1;
int opt_stb = 0; // 1: as Microware C 3.2's l68, 2: as Ultra C's l68
const char *opt_owner = NULL;
#define DEFAULT_STACK 0x00000C00
#define DEFAULT_REVS 1
#define DEFAULT_EDIT 0

typedef struct vec {
	char *buf;
	size_t len;
	size_t cap;
} Vec;

Vec *vec_new(int initsize) {
	Vec *vec = malloc(sizeof(Vec));
	vec->buf = malloc(initsize);
	vec->len = 0;
	vec->cap = initsize;
	return vec;
}

size_t vec_space(Vec *vec, size_t len) {
	size_t prelen = vec->len;
	size_t cap = vec->cap;
	while(cap < vec->len + len) {
		cap = cap * 3 / 2;
	}
	if(cap != vec->cap) {
		vec->buf = realloc(vec->buf, cap);
		vec->cap = cap;
	}

	vec->len += len;
	return prelen;
}

size_t vec_zero(Vec *vec, size_t len) {
	size_t prelen = vec->len;
	vec_space(vec, len);
	memset(vec->buf + prelen, 0, len);
	return prelen;
}

size_t vec_append(Vec *vec, void *src, size_t len) {
	size_t prelen = vec->len;
	vec_space(vec, len);
	memcpy(vec->buf + prelen, src, len);
	return prelen;
}

void vec_free(Vec *vec) {
	free(vec->buf);
	/*
	vec->buf = NULL;
	vec->len = 0;
	vec->cap = 0;
	*/
	free(vec);
}

// A pointer in the initialized data: its offset in the data area, and
// whether it points into .text.
typedef struct idref {
	uint32_t addr;
	int text;
} IdRef;

int cmp_idref(const void *a, const void *b) {
	uint32_t x = ((const IdRef*)a)->addr, y = ((const IdRef*)b)->addr;
	return x < y ? -1 : x > y;
}

size_t make_idrefs(Vec *vec, IdRef *refs, size_t nrefs, int for_text) {
	size_t iblock = vec_space(vec, 4);
	size_t ihi = iblock;
	uint16_t hi16 = -1;
	uint16_t count = 0;
	for(size_t i = 0; i < nrefs; i++) {
		if(refs[i].text != for_text) {
			continue;
		}
		uint32_t addr = refs[i].addr;
		if(hi16 != (addr >> 16)) {
			if(0 < count) {
				uint16_t *p = (uint16_t*)(vec->buf + ihi);
				p[0] = BE16(hi16);
				p[1] = BE16(count);

				ihi = vec_space(vec, 4);
			}
			hi16 = addr >> 16;
			count = 0;
		}
		uint16_t lo16 = BE16(addr & 0xFFFF);
		vec_append(vec, &lo16, 2);
		count++;
	}

	// XXX: copy code
	if(0 < count) {
		uint16_t *p = (uint16_t*)(vec->buf + ihi);
		p[0] = BE16(hi16);
		p[1] = BE16(count);

		ihi = vec_space(vec, 4);
	}

	// end of idrefs for a section
	uint16_t *p = (uint16_t*)(vec->buf + ihi);
	p[0] = BE16(0);
	p[1] = BE16(0);

	return iblock;
}

// The OS-9 module CRC of the first len bytes.
uint32_t module_crc(const char *buf, size_t len) {
	uint32_t crc = 0x00FFffff;
	for(size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)(unsigned char)buf[i] << 16;
		for(size_t j = 0; j < 8; j++) {
			crc <<= 1;
			if(crc & 0x01000000) {
				crc ^= 0x00800063;
			}
		}
	}
	return ~crc & 0x00FFffff;
}

// Fill in a module header's size, name offset, parity and CRC (this
// fork: shared by the program and symbol modules).
uint32_t finish_module(Vec *vec, size_t iname, size_t icrc) {
	struct modheader *mh = (void*)(vec->buf + 0);
	mh->size = BE32(vec->len);
	mh->name = BE32(iname);
	uint16_t parity = 0xFFFF;
	for(int i = 0; i < 0x2e / 2; i++) {
		parity ^= ((uint16_t*)mh)[i];
	}
	mh->parity = parity; // don't BE16, already swapped
	uint32_t crc = module_crc(vec->buf, vec->len - 3);
	*(uint32_t*)(vec->buf + icrc) = BE32(crc);
	return crc;
}

typedef struct stbsym {
	int32_t value;
	uint16_t type;
	const char *name;
	size_t rank; // linker symbols: their order; others: after them
} StbSym;

// By value; at the same value, as l68 lists them: Ultra C's in the order
// it defines them (linker symbols first, then the others in definition
// order, which in ROFs is by name), Microware C 3.2's in reverse.
int stb_reverse;
int cmp_stbsym(const void *a, const void *b) {
	const StbSym *p = a, *q = b;
	if(p->value != q->value) return p->value < q->value ? -1 : 1;
	int c = p->rank != q->rank ? (p->rank < q->rank ? -1 : 1) : strcmp(p->name, q->name);
	return stb_reverse ? -c : c;
}

int opt(int *argc, char ***argv) {
	const struct option options[] = {
		{ "name",  required_argument, NULL, 'n' },
		{ "stack", required_argument, NULL, 's' },
		{ "revs",  required_argument, NULL, 'r' },
		{ "edit",  required_argument, NULL, 'e' },
		{ "stb",   optional_argument, NULL, 'g' },
		{ "owner", required_argument, NULL, 'u' },
		{ "help",  no_argument,       NULL, 'h' },
		{ 0, 0, 0, 0 },
	};

	for(;;) {
		switch(getopt_long(*argc, *argv, "n:s:r:e:gu:h", options, NULL)) {
		case 'n':
			opt_name = optarg;
			break;
		case 's':
			opt_stack = strtol(optarg, NULL, 0);
			break;
		case 'r':
			opt_revs = strtol(optarg, NULL, 0);
			break;
		case 'g':
			if(!optarg || !strcmp(optarg, "c32")) opt_stb = 1;
			else if(!strcmp(optarg, "ucc")) opt_stb = 2;
			else {
				printf("unknown symbol module style %s (c32 or ucc)\n", optarg);
				return 0;
			}
			break;
		case 'u':
			opt_owner = optarg;
			break;
		case 'e':
			opt_edit = strtol(optarg, NULL, 0);
			break;
		case '?':
			printf("unknown option: %c\n", optopt);
			// FALLTHROUGH
		case 'h':
			printf("usage: %s [OPTIONS] ELFFILE OUTFILE\n", *argv[0]);
			printf("  -n, --name=MODNAME     override module name, default lowercase basename of ELFFILE\n");
			printf("  -s, --stack=STACKSIZE  set stack size, default __os9_stack or 0x%X\n", DEFAULT_STACK);
			printf("  -r, --revs=REVISION    set module revision, default from __os9_attrev or %d\n", DEFAULT_REVS);
			printf("  -e, --edit=EDITION     set module edition (not used by OS), default __os9_edition or %d\n", DEFAULT_EDIT);
			printf("  -g, --stb[=STYLE]      also write a symbol module OUTFILE.stb for the debugger,\n");
			printf("                         as Microware C 3.2's l68 (c32, default) or Ultra C's (ucc)\n");
			printf("  -u, --owner=GROUP.USER set module owner, default $GRPUSER or 0.0\n");
			printf("  -h, --help             show this help\n");
			return 0;
		case -1:
			*argc -= optind;
			*argv += optind;
			return 1;
		}
	}
}

// The module owner from GROUP.USER (decimal, as l68's -gu and GRPUSER).
int parse_owner(const char *s, uint32_t *owner) {
	char *e;
	unsigned long g = strtoul(s, &e, 10), u;
	if(e == s || *e != '.') return 0;
	s = e + 1;
	u = strtoul(s, &e, 10);
	if(e == s || *e || g > 0xFFFF || u > 0xFFFF) return 0;
	*owner = g << 16 | u;
	return 1;
}

int main(int argc, char *argv[]) {
	if(!opt(&argc, &argv)) {
		return 1;
	}

	if(!argv[0]) {
		puts("ELFFILE not specified");
		return 1;
	}

	if(!argv[1]) {
		puts("OUTFILE not specified");
		return 1;
	}

	// The owner: --owner, else GRPUSER as with l68, else 0.0.
	uint32_t owner = 0;
	if(!opt_owner) opt_owner = getenv("GRPUSER");
	if(opt_owner && *opt_owner && !parse_owner(opt_owner, &owner)) {
		printf("bad owner %s, expected GROUP.USER\n", opt_owner);
		return 1;
	}

	if(!opt_name) {
		opt_name = strdup(argv[0]);
		{
			char *p = strrchr(opt_name, '/');
			if(p) {
				memmove(opt_name, p + 1, strlen(p + 1) + 1);
			}
		}
		{
			char *p = strrchr(opt_name, '.'); // after the directory (this fork)
			if(p) { *p = '\0'; }
		}
		// XXX: isn't there something like strlower()??
		for(char *p = opt_name; *p; p++) {
			*p = tolower(*p);
		}
	}

	// validate
	// open
	bfd *abfd = bfd_openr(argv[0], "elf32-m68k"); // target?
	if(!abfd) {
		bfd_perror("cannot openr (not elf32-m68k?)");
		return 1;
	}
	bfd_check_format(abfd, bfd_object); // ensure it is object, not archive nor corefile

	// get entry and required sections
	bfd_vma entry = bfd_get_start_address(abfd);

	asection *text = bfd_get_section_by_name(abfd, ".text");
	if(!text) {
		bfd_perror(".text not found");
		return 1;
	}
	// .text may be at any VMA: everything is computed relative to it.
	// (With remote data, the data area reaches above 0, so .text must be
	// placed above it.)
	bfd_size_type tsize = bfd_section_size(text);
	bfd_vma tvma = bfd_section_vma(text);
	if(tsize % 2) {
		puts(".text size not aligned to word (2bytes)");
		return 1;
	}

	asection *data = bfd_get_section_by_name(abfd, ".data");
	if(!data) {
		bfd_perror(".data not found");
		return 1;
	}
	bfd_size_type dsize = bfd_section_size(data);
	bfd_vma dvma = bfd_section_vma(data); // the data area's base, see below
	if(dsize % 2) {
		puts(".data size not aligned to word (2bytes)");
		return 1;
	}

	asection *bss = bfd_get_section_by_name(abfd, ".bss");
	if(!bss) {
		bfd_perror(".bss not found");
		return 1;
	}
	bfd_size_type bsize = bfd_section_size(bss);

	// Remote data (optional): after .bss, beyond the 64K a6 window.
	asection *rdata = bfd_get_section_by_name(abfd, ".remote.data");
	asection *rbss = bfd_get_section_by_name(abfd, ".remote.bss");
	bfd_size_type rdsize = rdata ? bfd_section_size(rdata) : 0;
	bfd_size_type rbsize = rbss ? bfd_section_size(rbss) : 0;
	if(rdsize % 2) {
		puts(".remote.data size not aligned to word (2bytes)");
		return 1;
	}

	// The data area starts at the lowest data section (.data, or .bss if
	// the linker script puts it first) and ends with the last one.
	// Addresses are compared as signed 32-bit values and offsets computed
	// modulo 2^32: the area starts at -0x8000, and with more than 32K of
	// data, sections cross from 0xFFFFFFFF to 0 (ld --no-check-sections).
	if((int32_t)bfd_section_vma(bss) < (int32_t)dvma) dvma = bfd_section_vma(bss);
#define DOFF(vma) ((uint32_t)((vma) - dvma))
	uint32_t dend = DOFF(bfd_section_vma(data)) + dsize;
	if(DOFF(bfd_section_vma(bss)) + bsize > dend) dend = DOFF(bfd_section_vma(bss)) + bsize;
	if(rdsize && DOFF(bfd_section_vma(rdata)) + rdsize > dend) dend = DOFF(bfd_section_vma(rdata)) + rdsize;
	if(rbsize && DOFF(bfd_section_vma(rbss)) + rbsize > dend) dend = DOFF(bfd_section_vma(rbss)) + rbsize;
	// The initialized data is one block covering .data and, if present,
	// .remote.data; whatever lies between (usually .bss) is stored as
	// zeros, so a linker script with remote initialized data should put
	// .bss before .data.
	uint32_t idstart = DOFF(bfd_section_vma(data)), idend = idstart + dsize;
	if(rdsize) {
		if(DOFF(bfd_section_vma(rdata)) < idstart) idstart = DOFF(bfd_section_vma(rdata));
		if(DOFF(bfd_section_vma(rdata)) + rdsize > idend) idend = DOFF(bfd_section_vma(rdata)) + rdsize;
	}
	uint32_t idsize = idend - idstart;

	// relocations (with symbols required by bfd)
	// symtab not required directly but required for relocs
	long symsz = bfd_get_symtab_upper_bound(abfd);
	asymbol **syms = malloc(symsz);
	long nsyms = bfd_canonicalize_symtab(abfd, syms);

	// Module header values from a mainline ROF converted by rof2elf.
	asymbol *s_tylan = NULL, *s_attrev = NULL, *s_edition = NULL, *s_stack = NULL, *s_entry = NULL, *s_trapent = NULL;
	for(long i = 0; i < nsyms; i++) {
		const char *n = bfd_asymbol_name(syms[i]);
		if(!strcmp(n, "__os9_tylan")) s_tylan = syms[i];
		else if(!strcmp(n, "__os9_attrev")) s_attrev = syms[i];
		else if(!strcmp(n, "__os9_edition")) s_edition = syms[i];
		else if(!strcmp(n, "__os9_stack")) s_stack = syms[i];
		else if(!strcmp(n, "__os9_entry")) s_entry = syms[i];
		else if(!strcmp(n, "__os9_trapent")) s_trapent = syms[i];
	}
	if(s_entry) {
		entry = bfd_asymbol_value(s_entry);
	}

	// .text relocs: no relocations to .data/.bss allowed, except
	// R_68K_16 (a6-relative), and R_68K_32/R_68K_8 to .data/.bss or
	// absolute symbols: their values are link-time constants (a6-relative
	// data offsets, equates), as in code converted from ROF by rof2elf.
	long trelsz = bfd_get_reloc_upper_bound(abfd, text);
	arelent **trels = malloc(trelsz);
	long ntrels = bfd_canonicalize_reloc(abfd, text, trels, syms);
	char *refd = calloc(nsyms + 1, 1); // symbols that relocations refer to (for -g)
	for(long i = 0; i < ntrels; i++) {
		arelent *rel = trels[i];
		asection *ssec = rel->sym_ptr_ptr[0]->section;
		if(rel->sym_ptr_ptr >= syms && rel->sym_ptr_ptr < syms + nsyms) refd[rel->sym_ptr_ptr - syms] = 1;
		if(bfd_is_und_section(ssec)) {
			printf("undefined symbol %s (.text+%08lx)\n", rel->sym_ptr_ptr[0]->name, rel->address);
			return 1;
		}
		int constant = bfd_is_abs_section(ssec) || ssec == data || ssec == bss || (ssec && (ssec == rdata || ssec == rbss));
		if(!(ssec == text || rel->howto->type == 2 /* R_68K_16(%a6) */
		     || ((rel->howto->type == 1 /* R_68K_32 */ || rel->howto->type == 3 /* R_68K_8 */) && constant))) {
			printf(".text inter-section relocation not allowed: %s %08lx %s@%s+%08lx\n", rel->howto->name, rel->address, rel->sym_ptr_ptr[0]->name, rel->sym_ptr_ptr[0]->section->name, rel->addend);
			return 1;
		}
		// The value must fit its field: a6-relative 16 bits (more than 64K
		// of a6 data), 8 bits, and short branches (bra.s).  ld only warns
		// about some of these with --noinhibit-exec, and not at all about
		// R_68K_16.  Far PC16 references are handled by the jump table.
		{
			int64_t v = (int64_t)(int32_t)(bfd_asymbol_value(rel->sym_ptr_ptr[0]) + rel->addend);
			int bad = 0;
			switch(rel->howto->type) {
			case 2: /* R_68K_16 */ bad = v < -0x8000 || v > 0xFFFF || (ssec != text && !bfd_is_abs_section(ssec) && v > 0x7FFF); break;
			case 3: /* R_68K_8 */ bad = v < -0x80 || v > 0xFF; break;
			case 6: /* R_68K_PC8 */ v -= (int64_t)(int32_t)(tvma + rel->address); bad = v < -0x80 || v > 0x7F; break;
			}
			if(bad) {
				printf("%s at .text+%08lx doesn't fit: %s%+ld = %lld\n", rel->howto->name, rel->address, rel->sym_ptr_ptr[0]->name, (long)rel->addend, (long long)v);
				return 1;
			}
		}
	}

	// .data and .remote.data relocs: only R_68K_32 is allowed
	asection *dsecs[2] = { data, rdsize ? rdata : NULL };
	IdRef *drefs = NULL;
	size_t ndrefs = 0;
	for(int s = 0; s < 2; s++) {
		asection *sec = dsecs[s];
		if(!sec) continue;
		long drelsz = bfd_get_reloc_upper_bound(abfd, sec);
		arelent **drels = malloc(drelsz);
		long ndrels = bfd_canonicalize_reloc(abfd, sec, drels, syms);
		drefs = realloc(drefs, (ndrefs + (ndrels > 0 ? ndrels : 0) + 1) * sizeof(IdRef));
		for(long i = 0; i < ndrels; i++) {
			arelent *rel = drels[i];
			if(rel->howto->type != 1 /* R_68K_32 */) {
				printf("%s relocation other than R_68K_32 not allowed: %s %08lx %s@%s+%08lx\n", sec->name, rel->howto->name, rel->address, rel->sym_ptr_ptr[0]->name, rel->sym_ptr_ptr[0]->section->name, rel->addend);
				return 1;
			}
			if(rel->sym_ptr_ptr >= syms && rel->sym_ptr_ptr < syms + nsyms) refd[rel->sym_ptr_ptr - syms] = 1;
			// What the pointer points to: code or data, relocated when OS-9
			// loads the module; an absolute value (an equate) or an undefined
			// weak symbol (0, as ld resolved it) stays as it is.
			asymbol *s = rel->sym_ptr_ptr[0];
			asection *ssec = s->section;
			if(bfd_is_abs_section(ssec) || (bfd_is_und_section(ssec) && (s->flags & BSF_WEAK))) {
				continue;
			}
			if(bfd_is_und_section(ssec)) {
				printf("undefined symbol %s (%s+%08lx)\n", s->name, sec->name, rel->address);
				return 1;
			}
			if(!(ssec == text || ssec == data || ssec == bss || (ssec && (ssec == rdata || ssec == rbss)))) {
				printf("%s pointer to an unexpected section: %s@%s\n", sec->name, s->name, ssec->name);
				return 1;
			}
			drefs[ndrefs].addr = DOFF(bfd_section_vma(sec)) + rel->address;
			drefs[ndrefs].text = ssec == text;
			ndrefs++;
		}
	}
	qsort(drefs, ndrefs, sizeof(IdRef), cmp_idref);

	// generate
	Vec *vec = vec_new(0x1000);

	vec_zero(vec, sizeof(struct modheader));

	size_t iname = vec_append(vec, opt_name, strlen(opt_name) + 1);
	// pad to align to word
	vec_zero(vec, (strlen(opt_name) + 1) % 2);

	size_t itext = vec_space(vec, tsize);
	if(!bfd_get_section_contents(abfd, text, vec->buf + itext, 0, tsize)) {
		bfd_perror("could not read .text");
		return 1;
	}

	struct {
		uint32_t off;
		uint32_t len;
	} idatahdr = { BE32(idstart), BE32(idsize) };
	size_t iidatahdr = vec_append(vec, &idatahdr, sizeof(idatahdr));
	size_t iidata = vec_zero(vec, idsize);
	if(!bfd_get_section_contents(abfd, data, vec->buf + iidata + DOFF(bfd_section_vma(data)) - idstart, 0, dsize)) {
		bfd_perror("could not read .data");
		return 1;
	}
	if(rdsize && !bfd_get_section_contents(abfd, rdata, vec->buf + iidata + DOFF(bfd_section_vma(rdata)) - idstart, 0, rdsize)) {
		bfd_perror("could not read .remote.data");
		return 1;
	}

	// Jump table (this fork): a PC-relative call or address load whose
	// target is beyond +-32K (ld --noinhibit-exec left the value
	// truncated) goes through an entry of _jmptbl, which the linker script
	// reserves in .data, up to _ejmptbl: "jmp target" ($4EF9 and a 32-bit
	// address, relocated at load time). The instruction keeps its size:
	//   bsr.w f       -> jsr _jmptbl+x(a6)
	//   bra.w f       -> jmp _jmptbl+x(a6)
	//   lea f(pc),An  -> movea.l _jmptbl+x+2(a6),An   (the address of f)
	//   pea f(pc)     -> move.l _jmptbl+x+2(a6),-(sp)
	// a6-relative displacements equal VMAs, since the data area starts at
	// -0x8000 with A6 0x8000 into it.
	{
		bfd_vma jt = 0, ejt = 0;
		int have_jt = 0, have_ejt = 0;
		for(long i = 0; i < nsyms; i++) {
			const char *n = bfd_asymbol_name(syms[i]);
			if(!strcmp(n, "_jmptbl")) { jt = bfd_asymbol_value(syms[i]); have_jt = 1; }
			else if(!strcmp(n, "_ejmptbl")) { ejt = bfd_asymbol_value(syms[i]); have_ejt = 1; }
		}
		bfd_vma *targets = NULL;
		size_t ntargets = 0, nfar = 0;
		for(long i = 0; i < ntrels; i++) {
			arelent *rel = trels[i];
			if(rel->howto->type != 5 /* R_68K_PC16 */ && rel->howto->type != 14 /* R_68K_PLT16 */) {
				continue;
			}
			bfd_vma target = bfd_asymbol_value(rel->sym_ptr_ptr[0]) + rel->addend;
			int32_t disp = (int32_t)(target - (tvma + rel->address));
			if(-0x8000 <= disp && disp <= 0x7FFF) {
				continue;
			}
			nfar++;
			if(rel->address < 2) {
				printf("far PC-relative reference at .text+%08lx can't be patched\n", rel->address);
				return 1;
			}
			unsigned char *ins = (unsigned char*)vec->buf + itext + rel->address - 2;
			uint16_t op = (ins[0] << 8) | ins[1];
			uint16_t newop;
			int extra;
			if(rel->address >= 4) {
				uint16_t prev = (ins[-2] << 8) | ins[-1];
				if((prev & 0xFFBF) == 0x4CBA || (prev & 0xFF3F) == 0x083A) {
					printf("far PC-relative reference at .text+%08lx (instruction %04x %04x) can't be patched: target %s\n", rel->address, prev, op, rel->sym_ptr_ptr[0]->name);
					return 1;
				}
			}
			if(op == 0x6100) { newop = 0x4EAE; extra = 0; }		// bsr.w -> jsr d(a6)
			else if(op == 0x6000) { newop = 0x4EEE; extra = 0; }		// bra.w -> jmp d(a6)
			else if((op & 0xF1FF) == 0x41FA) { newop = 0x206E | (op & 0x0E00); extra = 2; }	// lea d(pc),An -> movea.l d(a6),An
			else if(op == 0x487A) { newop = 0x2F2E; extra = 2; }		// pea d(pc) -> move.l d(a6),-(sp)
			else {
				printf("far PC-relative reference at .text+%08lx (instruction %04x) can't be patched: target %s\n", rel->address, op, rel->sym_ptr_ptr[0]->name);
				return 1;
			}
			size_t k;
			for(k = 0; k < ntargets && targets[k] != target; k++)
				;
			if(k == ntargets) {
				targets = realloc(targets, (ntargets + 1) * sizeof(bfd_vma));
				targets[ntargets++] = target;
			}
			bfd_vma evma = jt + 6 * k;
			int32_t d = (int32_t)(evma + extra);
			if(d < -0x8000 || 0x7FFF < d) {
				printf("_jmptbl entry %zu is out of a6's reach\n", k);
				return 1;
			}
			ins[0] = newop >> 8; ins[1] = newop;
			ins[2] = (uint16_t)d >> 8; ins[3] = (uint16_t)d;
		}
		if(ntargets) {
			size_t cap = have_jt && have_ejt && (int32_t)(ejt - jt) >= 0 ? (size_t)(int32_t)(ejt - jt) / 6 : 0;
			if(!have_jt || !have_ejt || ntargets > cap) {
				printf("%zu far calls need a jump table of %zu entries (%zu bytes): reserve _jmptbl to _ejmptbl in .data (now %zu entries)\n", nfar, ntargets, ntargets * 6, have_jt ? cap : 0);
				return 1;
			}
			// the entries, with their addresses relocated like pointers
			drefs = realloc(drefs, (ndrefs + ntargets) * sizeof(IdRef));
			for(size_t k = 0; k < ntargets; k++) {
				uint32_t eoff = DOFF(jt + 6 * k);
				unsigned char *e = (unsigned char*)vec->buf + iidata + eoff - idstart;
				if(eoff < idstart || eoff + 6 > idend) {
					puts("_jmptbl must be inside .data");
					return 1;
				}
				e[0] = 0x4E; e[1] = 0xF9;
				uint32_t t = targets[k];
				e[2] = t >> 24; e[3] = t >> 16; e[4] = t >> 8; e[5] = t;
				drefs[ndrefs].addr = eoff + 2;
				drefs[ndrefs].text = 1;
				ndrefs++;
			}
			qsort(drefs, ndrefs, sizeof(IdRef), cmp_idref);
			printf("_jmptbl: %zu of %zu entries used, for %zu far references\n", ntargets, cap, nfar);
		}
		free(targets);
	}

	// fix pointer in .data to 0-based
	for(size_t i = 0; i < ndrefs; i++) {
		uint32_t *p = (void*)(vec->buf + iidata + drefs[i].addr - idstart);
		uint32_t v = BE32(*p);
		if(drefs[i].text) {
			*p = BE32(v - tvma + itext); // .text is offsetted by module header etc.
		} else {
			// we expect .data, .bss and remote data are contiguous
			*p = BE32(v - dvma);
		}
	}

	// idrefs
	size_t iidrefs = make_idrefs(vec, drefs, ndrefs, 1);
	make_idrefs(vec, drefs, ndrefs, 0);

	// crc, with pad on head
	size_t icrc = vec_zero(vec, 4);

	struct modheader *mh = (void*)(vec->buf + 0);
	mh->magic = BE16(0x4AFC);
	mh->sysrev = BE16(0x0001);
	mh->size = BE32(vec->len);
	mh->owner = BE32(owner);
	mh->name = BE32(iname);
	mh->accs = BE16(0x0555); // r-xr-xr-x
	uint16_t tylan = s_tylan ? bfd_asymbol_value(s_tylan) : 0x0101;
	uint16_t attrev = s_attrev ? bfd_asymbol_value(s_attrev) : 0x8000 | DEFAULT_REVS;
	mh->type = tylan >> 8; // prgm by default
	mh->lang = tylan & 0xFF; // objct by default
	mh->attr = attrev >> 8; // reentrant by default
	mh->revs = opt_revs >= 0 ? opt_revs : (attrev & 0xFF);
	mh->edit = BE16(opt_edit >= 0 ? opt_edit : s_edition ? (int)bfd_asymbol_value(s_edition) : DEFAULT_EDIT); // any
	mh->usage = BE32(0); // reserved
	mh->symbol = BE32(0); // reserved
	uint16_t parity = 0xFFFF;
	for(int i = 0; i < 0x2e / 2; i++) {
		parity ^= ((uint16_t*)mh)[i];
	}
	mh->parity = parity; // don't BE16, already swapped

	mh->exec = BE32(itext + entry - tvma);
	mh->excpt = BE32(s_trapent ? itext + bfd_asymbol_value(s_trapent) - tvma : 0); // uninitialized trap entry
	mh->mem = BE32(dend);
	mh->stack = BE32(opt_stack >= 0 ? opt_stack : s_stack ? (int)bfd_asymbol_value(s_stack) : DEFAULT_STACK);
	mh->idata = BE32(iidatahdr);
	mh->irefs = BE32(iidrefs);

	// update crc
	uint32_t crc = module_crc(vec->buf, vec->len - 3);
	*(uint32_t*)(vec->buf + icrc) = BE32(crc);

	// Symbol module (this fork, -g): an OS-9 data module named after the
	// program module plus ".stb", as Microware's linker writes with -g
	// (format in the OS-9/68000 User-State Debugger manual, appendix A):
	// the STB format number, the program module's CRC, then the global
	// symbols sorted by value (code: offsets in the module; data: a6-
	// relative, i.e. their VMAs), then their names. The header's symbol
	// field points to the STB header. The symbols the linker defines are
	// added, flagged 0x2000: Microware C 3.2's l68 defines btext, bname,
	// etext, end and _jmptbl, and also flags the symbols that other psects
	// refer to (here: that relocations refer to); Ultra C's l68 also
	// defines _btext, _bname, _etext, _bdata, bdata and _enddata.
	if(opt_stb) {
		bfd_vma jmptbl = dvma + dend;
		for(long i = 0; i < nsyms; i++) {
			if(!strcmp(bfd_asymbol_name(syms[i]), "_jmptbl")) jmptbl = bfd_asymbol_value(syms[i]);
		}
		const StbSym ucclinker[] = {
			{ (int32_t)dvma, 0x2001, "_bdata", 0 }, { (int32_t)dvma, 0x2001, "bdata", 1 },
			{ (int32_t)(dvma + dend), 0x2001, "_enddata", 2 }, { (int32_t)(dvma + dend), 0x2001, "end", 3 },
			{ (int32_t)jmptbl, 0x2001, "_jmptbl", 4 },
			{ 0, 0x2004, "_btext", 5 }, { 0, 0x2004, "btext", 6 },
			{ (int32_t)iname, 0x2004, "_bname", 7 }, { (int32_t)iname, 0x2004, "bname", 8 },
			{ (int32_t)vec->len, 0x2004, "_etext", 9 }, { (int32_t)vec->len, 0x2004, "etext", 10 },
		};
		const StbSym c32linker[] = {
			{ (int32_t)(dvma + dend), 0x2001, "end", 0 },
			{ (int32_t)jmptbl, 0x2001, "_jmptbl", 1 },
			{ 0, 0x2004, "btext", 2 },
			{ (int32_t)iname, 0x2004, "bname", 3 },
			{ (int32_t)vec->len, 0x2004, "etext", 4 },
		};
		const StbSym *linker = opt_stb == 2 ? ucclinker : c32linker;
		const size_t nlinker = opt_stb == 2 ? sizeof(ucclinker) / sizeof(ucclinker[0]) : sizeof(c32linker) / sizeof(c32linker[0]);
		const char *const reserved[] = { "_bdata", "bdata", "_enddata", "end", "_jmptbl", "_btext", "btext", "_bname", "bname", "_etext", "etext", "_ejmptbl" };
		stb_reverse = opt_stb == 1;
		StbSym *ss = malloc((nsyms + nlinker) * sizeof(StbSym));
		memcpy(ss, linker, nlinker * sizeof(StbSym));
		size_t nss = nlinker;
		for(long i = 0; i < nsyms; i++) {
			asymbol *s = syms[i];
			asection *sec = s->section;
			uint16_t type;
			if(!(s->flags & BSF_GLOBAL) || !strncmp(bfd_asymbol_name(s), "__os9_", 6)) continue;
			size_t k, nres = sizeof(reserved) / sizeof(reserved[0]);
			for(k = 0; k < nres && strcmp(bfd_asymbol_name(s), reserved[k]); k++);
			if(k < nres) continue; // the linker's: as above, or not listed
			if(sec == text) type = 4;
			else if(sec == data) type = 1;
			else if(sec == bss) type = 0;
			else if(sec && sec == rdata) type = 3; // initialized remote, as l68
			else if(sec && sec == rbss) type = 2;
			else if(bfd_is_abs_section(sec)) type = 6;
			else continue;
			ss[nss].value = type == 4 ? (int32_t)(itext + bfd_asymbol_value(s) - tvma) : (int32_t)bfd_asymbol_value(s);
			ss[nss].type = type | (opt_stb == 1 && refd[i] ? 0x2000 : 0);
			ss[nss].name = bfd_asymbol_name(s);
			ss[nss].rank = nlinker;
			nss++;
		}
		qsort(ss, nss, sizeof(StbSym), cmp_stbsym);

		Vec *sv = vec_new(0x1000);
		vec_zero(sv, 0x30); // data module header
		char *sname = malloc(strlen(opt_name) + 5);
		sprintf(sname, "%s.stb", opt_name);
		size_t isname = vec_append(sv, sname, strlen(sname) + 1);
		while(sv->len % 2) vec_zero(sv, 1); // word fields must be at even offsets
		size_t istb = vec_zero(sv, 2 + 4 + 4 + 4);
		while(sv->len % 16) vec_zero(sv, 1);
		size_t ient = vec_zero(sv, nss * 10);
		unsigned char *h = (unsigned char*)sv->buf + istb;
		h[0] = 0x01; h[1] = 0x00; // STB format 0x0100
		h[2] = crc >> 24; h[3] = crc >> 16; h[4] = crc >> 8; h[5] = crc;
		h[6] = ient >> 24; h[7] = ient >> 16; h[8] = ient >> 8; h[9] = ient;
		h[10] = nss >> 24; h[11] = nss >> 16; h[12] = nss >> 8; h[13] = nss;
		for(size_t i = 0; i < nss; i++) {
			size_t iname = vec_append(sv, (void*)ss[i].name, strlen(ss[i].name) + 1);
			unsigned char *e = (unsigned char*)sv->buf + ient + i * 10;
			uint32_t v = ss[i].value;
			e[0] = v >> 24; e[1] = v >> 16; e[2] = v >> 8; e[3] = v;
			e[4] = ss[i].type >> 8; e[5] = ss[i].type;
			e[6] = iname >> 24; e[7] = iname >> 16; e[8] = iname >> 8; e[9] = iname;
		}
		while(sv->len % 2) vec_zero(sv, 1);
		size_t iscrc = vec_zero(sv, 4);

		struct modheader *sh = (void*)(sv->buf + 0);
		sh->symbol = BE32(istb);
		sh->magic = BE16(0x4AFC);
		sh->sysrev = BE16(0x0001);
		sh->owner = BE32(owner);
		sh->accs = BE16(0x0555);
		sh->type = 4; // data
		sh->lang = 0;
		sh->attr = 0x80;
		sh->revs = mh->revs;
		sh->edit = mh->edit;
		finish_module(sv, isname, iscrc);

		// OUTFILE.stb, or in the STB directory next to OUTFILE if there's one
		// (as l68).
		char *sfile = malloc(strlen(argv[1]) + 9);
		const char *base = strrchr(argv[1], '/');
		base = base ? base + 1 : argv[1];
		sprintf(sfile, "%.*sSTB", (int)(base - argv[1]), argv[1]);
		struct stat st;
		if(!stat(sfile, &st) && S_ISDIR(st.st_mode)) sprintf(sfile, "%.*sSTB/%s.stb", (int)(base - argv[1]), argv[1], base);
		else sprintf(sfile, "%s.stb", argv[1]);
		FILE *sf = fopen(sfile, "w+b");
		if(!sf || !fwrite(sv->buf, sv->len, 1, sf)) {
			perror("could not write symbol module");
			return 1;
		}
		fclose(sf);
	}

	// write
	FILE *fp = fopen(argv[1], "w+b");
	if(!fp) {
		perror("could not open output file");
		return 1;
	}
	if(!fwrite(vec->buf, vec->len, 1, fp)) {
		perror("could not write output file");
		return 1;
	}
	fclose(fp);

	return 0;
}
