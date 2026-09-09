/*
  RPCEmu - An Acorn system emulator

  An ARM disassembler, so the debugger can say what the machine is doing.

  Without one, everything the instrumentation reports is a hex word: a
  breakpoint stops at a number, the trace ring is a column of numbers, and a
  fault names an instruction nobody can read. That is a poor way to find out
  why a compiler emitted the wrong thing, which is what this whole exercise
  is for.

  ARMv4 with the ARMv5 additions the StrongARM does not have but ROM code
  sometimes contains anyway. Syntax follows the ARM architecture reference
  rather than any one assembler, with RISC OS's ampersand for hexadecimal
  because the people reading it write addresses that way.

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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dbg.h"

/* "al" is never printed: an unconditional instruction is the common case and
   spelling it out on every line costs more than it explains. "nv" was
   deprecated before the StrongARM and means something else entirely on
   ARMv5, which is why it is decoded separately rather than as a condition. */
static const char *const CONDS[16] = {
	"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
	"hi", "ls", "ge", "lt", "gt", "le", "", "nv"
};

static const char *const REGS[16] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
	"r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
};

static const char *const DP_OPS[16] = {
	"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
	"tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"
};

static const char *const SHIFTS[4] = { "lsl", "lsr", "asr", "ror" };

/** Somewhere to build a line without counting characters by hand. */
typedef struct {
	char	*at;
	size_t	left;
} Buf;

static void
bput(Buf *b, const char *format, ...)
{
	va_list ap;
	int n;

	if (b->left <= 1) {
		return;
	}

	va_start(ap, format);
	n = vsnprintf(b->at, b->left, format, ap);
	va_end(ap);

	if (n < 0) {
		return;
	}

	if ((size_t) n >= b->left) {
		n = (int) (b->left - 1);
	}

	b->at += n;
	b->left -= (size_t) n;
}

/** A signed offset, written the way a person reads it. */
static void
bput_offset(Buf *b, int up, uint32_t value)
{
	bput(b, "#%s&%x", up ? "" : "-", (unsigned) value);
}

/**
 * The shifter operand of a data processing instruction.
 *
 * The four encodings that mean something other than they appear to are all
 * here: LSR #0 and ASR #0 mean 32 rather than nothing, ROR #0 is RRX, and a
 * register shift takes its amount from the bottom byte of Rs.
 */
static void
shifter_operand(Buf *b, uint32_t op)
{
	const uint32_t rm = op & 0xf;
	const uint32_t type = (op >> 5) & 3;

	if (op & (1u << 25)) {
		const uint32_t imm = op & 0xff;
		const uint32_t rot = ((op >> 8) & 0xf) * 2;
		const uint32_t value = (rot == 0)
		    ? imm
		    : ((imm >> rot) | (imm << (32 - rot)));

		bput(b, "#&%x", (unsigned) value);
		return;
	}

	if (op & (1u << 4)) {
		bput(b, "%s, %s %s", REGS[rm], SHIFTS[type], REGS[(op >> 8) & 0xf]);
		return;
	}

	{
		const uint32_t amount = (op >> 7) & 0x1f;

		if (amount == 0) {
			switch (type) {
			case 0:	bput(b, "%s", REGS[rm]); return;		/* LSL #0 */
			case 1:	bput(b, "%s, lsr #32", REGS[rm]); return;
			case 2:	bput(b, "%s, asr #32", REGS[rm]); return;
			default: bput(b, "%s, rrx", REGS[rm]); return;
			}
		}

		bput(b, "%s, %s #%u", REGS[rm], SHIFTS[type], (unsigned) amount);
	}
}

/** The register list of a block transfer, with runs collapsed. */
static void
register_list(Buf *b, uint32_t op)
{
	int first = 1;
	int i = 0;

	bput(b, "{");

	while (i < 16) {
		int run;

		if ((op & (1u << i)) == 0) {
			i++;
			continue;
		}

		run = i;
		while (run < 15 && (op & (1u << (run + 1))) != 0) {
			run++;
		}

		bput(b, "%s%s", first ? "" : ", ", REGS[i]);
		first = 0;

		/* A run of three or more is worth collapsing; two is not, because
		   "r4, r5" is no longer than "r4-r5" and reads better. */
		if (run >= i + 2) {
			bput(b, "-%s", REGS[run]);
		} else if (run == i + 1) {
			bput(b, ", %s", REGS[run]);
		}

		i = run + 1;
	}

	bput(b, "}");
}

/** Addressing mode 2: the operand of LDR and STR. */
static void
address_mode_2(Buf *b, uint32_t op)
{
	const uint32_t rn = (op >> 16) & 0xf;
	const int pre = (op >> 24) & 1;
	const int up = (op >> 23) & 1;
	const int writeback = (op >> 21) & 1;

	/* An offset of zero is the common case and saying "#&0" on every
	   plain load costs a reader more than it tells them. */
	if (pre && !writeback && (op & (1u << 25)) == 0 && (op & 0xfff) == 0) {
		bput(b, "[%s]", REGS[rn]);
		return;
	}

	bput(b, "[%s", REGS[rn]);

	if (!pre) {
		bput(b, "], ");
	} else {
		bput(b, ", ");
	}

	if (op & (1u << 25)) {
		bput(b, "%s%s", up ? "" : "-", REGS[op & 0xf]);

		if (((op >> 4) & 0xff) != 0) {
			const uint32_t type = (op >> 5) & 3;
			const uint32_t amount = (op >> 7) & 0x1f;

			if (amount == 0 && type == 3) {
				bput(b, ", rrx");
			} else if (amount != 0 || type != 0) {
				bput(b, ", %s #%u", SHIFTS[type],
				     (unsigned) (amount == 0 ? 32 : amount));
			}
		}
	} else {
		bput_offset(b, up, op & 0xfff);
	}

	if (pre) {
		bput(b, "]%s", writeback ? "!" : "");
	}
}

/** Addressing mode 3: halfword and signed byte transfers. */
static void
address_mode_3(Buf *b, uint32_t op)
{
	const uint32_t rn = (op >> 16) & 0xf;
	const int pre = (op >> 24) & 1;
	const int up = (op >> 23) & 1;
	const int immediate = (op >> 22) & 1;
	const int writeback = (op >> 21) & 1;

	if (pre && !writeback && immediate &&
	    (((op >> 4) & 0xf0) | (op & 0xf)) == 0) {
		bput(b, "[%s]", REGS[rn]);
		return;
	}

	bput(b, "[%s", REGS[rn]);
	bput(b, pre ? ", " : "], ");

	if (immediate) {
		bput_offset(b, up, ((op >> 4) & 0xf0) | (op & 0xf));
	} else {
		bput(b, "%s%s", up ? "" : "-", REGS[op & 0xf]);
	}

	if (pre) {
		bput(b, "]%s", writeback ? "!" : "");
	}
}

/**
 * Disassemble one ARM instruction.
 *
 * @param addr   Where the instruction lives, for working out branch targets
 * @param op     The instruction word
 * @param out    Receives the text
 * @param length Bytes available in out
 */
void
dbg_disasm(uint32_t addr, uint32_t op, char *out, size_t length)
{
	Buf b = { out, length };
	const uint32_t cond = op >> 28;
	const char *c;

	if (length == 0) {
		return;
	}

	out[0] = '\0';

	/* The condition field being all ones is not a condition on ARMv5: it
	   marks a separate instruction space. On the StrongARM none of those
	   exist, so saying so beats decoding them as if they did. */
	/* The condition field being all ones is not a condition: it marks a
	   separate instruction space introduced with ARMv5. The StrongARM this
	   emulates would refuse to execute any of it, but a disassembler's job
	   is to say what the bytes mean, and BLX is common enough in a RISC OS
	   5 ROM that calling it undefined would be the less useful answer. */
	if (cond == 0xf) {
		if ((op & 0xfe000000u) == 0xfa000000u) {
			const int32_t offset = (int32_t) ((op & 0x00ffffffu) << 8) >> 6;
			const uint32_t half = (op >> 23) & 2;
			const uint32_t target = addr + 8 + (uint32_t) offset + half;

			bput(&b, "blx &%08x", (unsigned) target);
			return;
		}

		bput(&b, "undefined ; &%08x", (unsigned) op);
		return;
	}

	c = CONDS[cond];

	/* BX, before the data processing block that would otherwise claim it. */
	if ((op & 0x0ffffff0u) == 0x012fff10u) {
		bput(&b, "bx%s %s", c, REGS[op & 0xf]);
		return;
	}

	/* Multiplies sit inside the data processing space and are told apart
	   only by bits 7:4 being 1001, which is why they come first. */
	if ((op & 0x0fc000f0u) == 0x00000090u) {
		const int accumulate = (op >> 21) & 1;
		const char *s = ((op >> 20) & 1) ? "s" : "";

		if (accumulate) {
			bput(&b, "mla%s%s %s, %s, %s, %s", c, s,
			     REGS[(op >> 16) & 0xf], REGS[op & 0xf],
			     REGS[(op >> 8) & 0xf], REGS[(op >> 12) & 0xf]);
		} else {
			bput(&b, "mul%s%s %s, %s, %s", c, s,
			     REGS[(op >> 16) & 0xf], REGS[op & 0xf],
			     REGS[(op >> 8) & 0xf]);
		}
		return;
	}

	if ((op & 0x0f8000f0u) == 0x00800090u) {
		static const char *const LONG_OPS[4] = {
			"umull", "umlal", "smull", "smlal"
		};
		const uint32_t which = (op >> 21) & 3;
		const char *s = ((op >> 20) & 1) ? "s" : "";

		bput(&b, "%s%s%s %s, %s, %s, %s", LONG_OPS[which], c, s,
		     REGS[(op >> 12) & 0xf], REGS[(op >> 16) & 0xf],
		     REGS[op & 0xf], REGS[(op >> 8) & 0xf]);
		return;
	}

	/* BKPT arrived with ARMv5 and this StrongARM would take it as
	   undefined, but a debugger that cannot name a breakpoint instruction
	   is being unhelpful about the one thing it exists for. It has no
	   conditional form: any other condition is some other encoding. */
	if ((op & 0xfff000f0u) == 0xe1200070u) {
		bput(&b, "bkpt &%04x", (unsigned)
		     ((((op >> 8) & 0xfff) << 4) | (op & 0xf)));
		return;
	}

	if ((op & 0x0fb00ff0u) == 0x01000090u) {
		bput(&b, "swp%s%s %s, %s, [%s]", c, ((op >> 22) & 1) ? "b" : "",
		     REGS[(op >> 12) & 0xf], REGS[op & 0xf], REGS[(op >> 16) & 0xf]);
		return;
	}

	/* Halfword and signed transfers: bits 27:25 clear, bit 7 and bit 4 set. */
	if ((op & 0x0e000090u) == 0x00000090u && ((op >> 5) & 3) != 0) {
		const uint32_t kind = (op >> 5) & 3;
		const int load = (op >> 20) & 1;
		const uint32_t rd = (op >> 12) & 0xf;

		/* Signedness only means something on a load: there is no such
		   thing as storing a signed halfword. With L clear the same
		   encodings are STRH, and the doubleword pair that arrived with
		   ARMv5TE - which this StrongARM would refuse, but which is
		   still what the bytes say. */
		if (load) {
			static const char *const KINDS[4] = { "", "h", "sb", "sh" };

			bput(&b, "ldr%s%s %s, ", c, KINDS[kind], REGS[rd]);
			address_mode_3(&b, op);
			return;
		}

		if (kind == 1) {
			bput(&b, "strh%s %s, ", c, REGS[rd]);
			address_mode_3(&b, op);
			return;
		}

		/* A doubleword names an even register and its odd neighbour, so
		   an odd one is not a doubleword at all. */
		if ((rd & 1) != 0 || rd == 14) {
			bput(&b, "undefined ; &%08x", (unsigned) op);
			return;
		}

		bput(&b, "%s%s %s, %s, ", (kind == 2) ? "ldrd" : "strd", c,
		     REGS[rd], REGS[rd + 1]);
		address_mode_3(&b, op);
		return;
	}

	/* MRS and MSR, which look like TST/TEQ/CMP/CMN with S clear. */
	if ((op & 0x0fbf0fffu) == 0x010f0000u) {
		bput(&b, "mrs%s %s, %s", c, REGS[(op >> 12) & 0xf],
		     ((op >> 22) & 1) ? "spsr" : "cpsr");
		return;
	}

	if ((op & 0x0db0f000u) == 0x0120f000u) {
		const char *psr = ((op >> 22) & 1) ? "spsr" : "cpsr";
		char fields[5];
		int n = 0;

		if (op & (1u << 19)) fields[n++] = 'f';
		if (op & (1u << 18)) fields[n++] = 's';
		if (op & (1u << 17)) fields[n++] = 'x';
		if (op & (1u << 16)) fields[n++] = 'c';
		fields[n] = '\0';

		bput(&b, "msr%s %s_%s, ", c, psr, fields);
		shifter_operand(&b, op);
		return;
	}

	switch ((op >> 25) & 7) {
	case 0:
	case 1: {	/* data processing */
		const uint32_t which = (op >> 21) & 0xf;
		const int set = (op >> 20) & 1;
		const uint32_t rd = (op >> 12) & 0xf;
		const uint32_t rn = (op >> 16) & 0xf;

		if (which >= 8 && which <= 11) {
			/* These four opcodes only mean a comparison when S is
			   set. With S clear the encoding belongs to the
			   miscellaneous space - MRS, MSR, BX and the rest, all
			   decoded above this point - so anything reaching here
			   is not a comparison and must not be printed as one. */
			if (!set) {
				bput(&b, "undefined ; &%08x", (unsigned) op);
				return;
			}

			/* Comparisons have no destination, and always set flags,
			   so an S would be noise. */
			bput(&b, "%s%s %s, ", DP_OPS[which], c, REGS[rn]);
			shifter_operand(&b, op);
			return;
		}

		if (which == 13 || which == 15) {
			/* MOV and MVN have no first operand. */
			bput(&b, "%s%s%s %s, ", DP_OPS[which], c, set ? "s" : "",
			     REGS[rd]);
			shifter_operand(&b, op);
			return;
		}

		bput(&b, "%s%s%s %s, %s, ", DP_OPS[which], c, set ? "s" : "",
		     REGS[rd], REGS[rn]);
		shifter_operand(&b, op);
		return;
	}

	case 2:
	case 3: {	/* load and store */
		const int load = (op >> 20) & 1;
		const int byte = (op >> 22) & 1;
		const int pre = (op >> 24) & 1;
		const int writeback = (op >> 21) & 1;

		/* A register offset with bit 4 set is not an addressing mode; the
		   architecture reserves it. */
		if (((op >> 25) & 1) && (op & (1u << 4))) {
			bput(&b, "undefined ; &%08x", (unsigned) op);
			return;
		}

		bput(&b, "%s%s%s%s %s, ", load ? "ldr" : "str", c,
		     byte ? "b" : "",
		     (!pre && writeback) ? "t" : "",
		     REGS[(op >> 12) & 0xf]);
		address_mode_2(&b, op);

		/* Literal pools are how ARM code gets a constant, so resolving
		   pc-relative loads turns an unreadable line into the one that
		   says where the value came from. */
		if (((op >> 16) & 0xf) == 15 && pre && ((op >> 25) & 1) == 0) {
			const uint32_t offset = op & 0xfff;
			const uint32_t target = ((op >> 23) & 1)
			    ? addr + 8 + offset
			    : addr + 8 - offset;

			bput(&b, " ; &%08x", (unsigned) target);
		}
		return;
	}

	case 4: {	/* block transfer */
		static const char *const MODES[4] = { "da", "ia", "db", "ib" };
		const int load = (op >> 20) & 1;
		const uint32_t mode = ((op >> 23) & 1) | (((op >> 24) & 1) << 1);

		/* Transferring no registers at all is not an encoding of
		   anything; printing an empty list would look like a real
		   instruction that happens to do nothing. */
		if ((op & 0xffffu) == 0) {
			bput(&b, "undefined ; &%08x", (unsigned) op);
			return;
		}

		bput(&b, "%s%s%s %s%s, ", load ? "ldm" : "stm", c, MODES[mode],
		     REGS[(op >> 16) & 0xf], ((op >> 21) & 1) ? "!" : "");
		register_list(&b, op);

		if (op & (1u << 22)) {
			bput(&b, "^");
		}
		return;
	}

	case 5: {	/* branch */
		int32_t offset = (int32_t) ((op & 0x00ffffffu) << 8) >> 6;
		const uint32_t target = addr + 8 + (uint32_t) offset;

		bput(&b, "b%s%s &%08x", ((op >> 24) & 1) ? "l" : "", c,
		     (unsigned) target);
		return;
	}

	case 6: {	/* coprocessor load and store */
		const int load = (op >> 20) & 1;
		const int pre = (op >> 24) & 1;

		/* MCRR and MRRC live in this space and are not transfers at all;
		   printing them as an STC with a wild offset would be worse than
		   saying nothing. */
		if ((op & 0x0fe00000u) == 0x0c400000u) {
			bput(&b, "%s%s p%u, %u, %s, %s, c%u",
			     load ? "mrrc" : "mcrr", c,
			     (unsigned) ((op >> 8) & 0xf),
			     (unsigned) ((op >> 4) & 0xf),
			     REGS[(op >> 12) & 0xf],
			     REGS[(op >> 16) & 0xf],
			     (unsigned) (op & 0xf));
			return;
		}

		bput(&b, "%s%s%s p%u, c%u, [%s", load ? "ldc" : "stc", c,
		     ((op >> 22) & 1) ? "l" : "",
		     (unsigned) ((op >> 8) & 0xf),
		     (unsigned) ((op >> 12) & 0xf),
		     REGS[(op >> 16) & 0xf]);

		if (pre) {
			bput(&b, ", ");
			bput_offset(&b, (op >> 23) & 1, (op & 0xff) * 4);
			bput(&b, "]%s", ((op >> 21) & 1) ? "!" : "");
		} else {
			bput(&b, "], ");
			bput_offset(&b, (op >> 23) & 1, (op & 0xff) * 4);
		}
		return;
	}

	case 7: {
		if ((op & 0x0f000000u) == 0x0f000000u) {
			bput(&b, "swi%s &%06x", c, (unsigned) (op & 0x00ffffffu));
			return;
		}

		if (op & (1u << 4)) {
			/* MRC and MCR: how RISC OS talks to the cache and MMU,
			   so worth reading rather than guessing at. */
			const int to_arm = (op >> 20) & 1;

			bput(&b, "%s%s p%u, %u, %s, c%u, c%u, %u",
			     to_arm ? "mrc" : "mcr", c,
			     (unsigned) ((op >> 8) & 0xf),
			     (unsigned) ((op >> 21) & 7),
			     REGS[(op >> 12) & 0xf],
			     (unsigned) ((op >> 16) & 0xf),
			     (unsigned) (op & 0xf),
			     (unsigned) ((op >> 5) & 7));
			return;
		}

		bput(&b, "cdp%s p%u, %u, c%u, c%u, c%u, %u", c,
		     (unsigned) ((op >> 8) & 0xf),
		     (unsigned) ((op >> 20) & 0xf),
		     (unsigned) ((op >> 12) & 0xf),
		     (unsigned) ((op >> 16) & 0xf),
		     (unsigned) (op & 0xf),
		     (unsigned) ((op >> 5) & 7));
		return;
	}

	default:
		break;
	}

	bput(&b, "undefined ; &%08x", (unsigned) op);
}
