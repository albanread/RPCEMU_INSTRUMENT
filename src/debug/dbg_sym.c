/*
  RPCEmu - An Acorn system emulator

  ELF symbol tables, so addresses have names.

  The compiler links at a known base and keeps a real ELF beside the flat
  image it loads into the machine, so symbol values line up with guest
  addresses directly. Nothing here has to guess where code ended up, because
  the base was chosen rather than discovered.

  Only what a debugger needs is read: the section headers, .symtab and its
  string table. 32-bit little-endian ELF only, which is what an ARM RISC OS
  image is.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "dbg.h"

#define SYM_MAX		8192
#define SYM_NAME_MAX	128

/* How far past a symbol with no recorded size it is still credible to say
   an address belongs to it, when nothing follows it in the table. */
#define SYM_UNSIZED_MAX	0x1000

#define SHT_SYMTAB	2
#define STT_FUNC	2
#define STT_OBJECT	1
#define STT_NOTYPE	0

typedef struct {
	char		name[SYM_NAME_MAX];
	uint32_t	addr;
	uint32_t	size;
} Symbol;

static Symbol symbols[SYM_MAX];
static int symbol_count;

static uint16_t
read16(const uint8_t *p)
{
	return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t
read32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int
symbol_compare(const void *a, const void *b)
{
	const Symbol *sa = a;
	const Symbol *sb = b;

	if (sa->addr < sb->addr) {
		return -1;
	}
	if (sa->addr > sb->addr) {
		return 1;
	}

	return 0;
}

void
dbg_sym_clear(void)
{
	symbol_count = 0;
}

int
dbg_sym_count(void)
{
	return symbol_count;
}

int
dbg_sym_load(const char *path, uint32_t bias, const char **error)
{
	uint8_t *image = NULL;
	long size;
	FILE *f;
	uint32_t shoff;
	uint16_t shentsize, shnum;
	int loaded = 0;
	int i;

	*error = NULL;

	f = fopen(path, "rb");
	if (f == NULL) {
		*error = "cannot open the file";
		return -1;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		*error = "cannot size the file";
		return -1;
	}
	size = ftell(f);
	rewind(f);

	if (size < 52) {
		fclose(f);
		*error = "too small to be an ELF file";
		return -1;
	}

	image = malloc((size_t) size);
	if (image == NULL) {
		fclose(f);
		*error = "out of memory";
		return -1;
	}
	if (fread(image, 1, (size_t) size, f) != (size_t) size) {
		free(image);
		fclose(f);
		*error = "short read";
		return -1;
	}
	fclose(f);

	if (memcmp(image, "\177ELF", 4) != 0) {
		free(image);
		*error = "not an ELF file";
		return -1;
	}
	if (image[4] != 1 || image[5] != 1) {
		free(image);
		*error = "not 32-bit little-endian ELF";
		return -1;
	}

	shoff     = read32(image + 32);
	shentsize = read16(image + 46);
	shnum     = read16(image + 48);

	if (shoff == 0 || shnum == 0 ||
	    (uint64_t) shoff + (uint64_t) shnum * shentsize > (uint64_t) size)
	{
		free(image);
		*error = "bad section headers";
		return -1;
	}

	dbg_sym_clear();

	for (i = 0; i < (int) shnum; i++) {
		const uint8_t *sh = image + shoff + (size_t) i * shentsize;
		const uint32_t type = read32(sh + 4);
		uint32_t offset, sect_size, link, entsize;
		const uint8_t *strtab;
		uint32_t strtab_size;
		uint32_t entries, e;

		if (type != SHT_SYMTAB) {
			continue;
		}

		offset    = read32(sh + 16);
		sect_size = read32(sh + 20);
		link      = read32(sh + 24);
		entsize   = read32(sh + 36);

		if (entsize == 0 || link >= shnum ||
		    (uint64_t) offset + sect_size > (uint64_t) size)
		{
			continue;
		}

		{
			const uint8_t *strsh = image + shoff +
			                       (size_t) link * shentsize;
			const uint32_t stroff = read32(strsh + 16);

			strtab_size = read32(strsh + 20);
			if ((uint64_t) stroff + strtab_size > (uint64_t) size) {
				continue;
			}
			strtab = image + stroff;
		}

		entries = sect_size / entsize;
		for (e = 0; e < entries && symbol_count < SYM_MAX; e++) {
			const uint8_t *sym = image + offset + (size_t) e * entsize;
			const uint32_t name_off = read32(sym);
			const uint32_t value = read32(sym + 4);
			const uint32_t sym_size = read32(sym + 8);
			const uint8_t info = sym[12];
			const uint8_t stype = info & 0xf;
			Symbol *out;

			if (name_off == 0 || name_off >= strtab_size) {
				continue;
			}
			if (stype != STT_FUNC && stype != STT_OBJECT &&
			    stype != STT_NOTYPE)
			{
				continue;
			}
			if (value == 0) {
				continue;
			}

			/* ARM mapping symbols ($a, $d, $t) mark where code
			   changes between ARM, data and Thumb. They sit at the
			   start of every function, so keeping them would hide
			   every real name behind a "$a". */
			if (((const char *) (strtab + name_off))[0] == '$') {
				continue;
			}

			out = &symbols[symbol_count++];
			snprintf(out->name, sizeof(out->name), "%s",
			         (const char *) (strtab + name_off));
			out->addr = value + bias;
			out->size = sym_size;
			loaded++;
		}
	}

	free(image);

	if (loaded == 0) {
		*error = "no symbols found";
		return -1;
	}

	/* Sorted by address so the nearest-symbol lookup is a binary search. */
	qsort(symbols, (size_t) symbol_count, sizeof(symbols[0]), symbol_compare);

	return loaded;
}

int
dbg_sym_lookup(const char *name, uint32_t *addr)
{
	int i;

	if (name == NULL) {
		return 0;
	}

	for (i = 0; i < symbol_count; i++) {
		if (strcmp(symbols[i].name, name) == 0) {
			if (addr != NULL) {
				*addr = symbols[i].addr;
			}
			return 1;
		}
	}

	return 0;
}

const char *
dbg_sym_at(uint32_t addr, uint32_t *offset)
{
	int low = 0;
	int high = symbol_count - 1;
	int best = -1;

	while (low <= high) {
		const int mid = low + (high - low) / 2;

		if (symbols[mid].addr <= addr) {
			best = mid;
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}

	if (best < 0) {
		return NULL;
	}

	if (symbols[best].size != 0) {
		/* A sized symbol describes only its own extent. */
		if (addr >= symbols[best].addr + symbols[best].size) {
			return NULL;
		}
	} else {
		/* An unsized symbol runs to the next one. Without that bound
		   the last symbol in the table would claim every address above
		   it, reporting nonsense like "$a+4227825636" for a ROM
		   address. */
		const uint32_t limit = (best + 1 < symbol_count) ?
		    symbols[best + 1].addr : symbols[best].addr + SYM_UNSIZED_MAX;

		if (addr >= limit) {
			return NULL;
		}
	}

	if (offset != NULL) {
		*offset = addr - symbols[best].addr;
	}

	return symbols[best].name;
}

int
dbg_sym_get(int index, const char **name, uint32_t *addr, uint32_t *size)
{
	if (index < 0 || index >= symbol_count) {
		return 0;
	}

	if (name != NULL) { *name = symbols[index].name; }
	if (addr != NULL) { *addr = symbols[index].addr; }
	if (size != NULL) { *size = symbols[index].size; }

	return 1;
}
