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

int opt(int *argc, char ***argv) {
	const struct option options[] = {
		{ "name",  required_argument, NULL, 'n' },
		{ "stack", required_argument, NULL, 's' },
		{ "revs",  required_argument, NULL, 'r' },
		{ "edit",  required_argument, NULL, 'e' },
		{ "help",  no_argument,       NULL, 'h' },
		{ 0, 0, 0, 0 },
	};

	for(;;) {
		switch(getopt_long(*argc, *argv, "n:s:r:e:h", options, NULL)) {
		case 'n':
			opt_name = optarg;
			break;
		case 's':
			opt_stack = strtol(optarg, NULL, 0);
			break;
		case 'r':
			opt_revs = strtol(optarg, NULL, 0);
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
			printf("  -h, --help             show this help\n");
			return 0;
		case -1:
			*argc -= optind;
			*argv += optind;
			return 1;
		}
	}
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

	if(!opt_name) {
		opt_name = strdup(argv[0]);
		{
			char *p = strrchr(opt_name, '.');
			if(p) { *p = '\0'; }
		}
		{
			char *p = strrchr(opt_name, '/');
			if(p) {
				memmove(opt_name, p + 1, strlen(p + 1) + 1);
			}
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
	for(long i = 0; i < ntrels; i++) {
		arelent *rel = trels[i];
		asection *ssec = rel->sym_ptr_ptr[0]->section;
		int constant = bfd_is_abs_section(ssec) || ssec == data || ssec == bss || (ssec && (ssec == rdata || ssec == rbss));
		if(!(ssec == text || rel->howto->type == 2 /* R_68K_16(%a6) */
		     || ((rel->howto->type == 1 /* R_68K_32 */ || rel->howto->type == 3 /* R_68K_8 */) && constant))) {
			printf(".text inter-section relocation not allowed: %s %08lx %s@%s+%08lx\n", rel->howto->name, rel->address, rel->sym_ptr_ptr[0]->name, rel->sym_ptr_ptr[0]->section->name, rel->addend);
			return 1;
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
			drefs[ndrefs].addr = DOFF(bfd_section_vma(sec)) + rel->address;
			drefs[ndrefs].text = rel->sym_ptr_ptr[0]->section == text;
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
	mh->owner = BE32(0x00000000); // uid=0 gid=0
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
	uint32_t crc = 0x00FFffff;
	for(size_t i = 0; i < vec->len - 3; i++) {
		crc ^= (uint32_t)(unsigned char)vec->buf[i] << 16;
		for(size_t j = 0; j < 8; j++) {
			crc <<= 1;
			if(crc & 0x01000000) {
				crc ^= 0x00800063;
			}
		}
	}
	crc = ~crc & 0x00FFffff;
	*(uint32_t*)(vec->buf + icrc) = BE32(crc);

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
