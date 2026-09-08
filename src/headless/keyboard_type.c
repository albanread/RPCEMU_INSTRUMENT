/*
  RPCEmu - An Acorn system emulator

  Synthetic typing.

  RPCEmu's keyboard is a PS/2 device: the emulated machine sees scan codes,
  not characters. Host-level key injection (SendKeys and friends) fails
  because those arrive without a scan code and keyboard_map_key() has
  nothing to map. This types by generating scan-code set 2 sequences
  directly, which is what a real keyboard sends and what the i8042 expects.

  Events are paced rather than dumped into the queue in one go: the guest's
  keyboard driver reads one byte per interrupt, and a burst would either
  overflow the 256-byte PS/2 queue or arrive faster than RISC OS consumes
  it. One press or release per tick is slower than any human and still far
  faster than a person driving the GUI.

  Layout is UK, matching RISC OS's default.

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
#include <string.h>

#include "rpcemu.h"
#include "keyboard.h"
#include "headless.h"
#include "dbg.h"

/* PS/2 scan code set 2 make codes, as used by keyboard_key_press() */
#define SC_LSHIFT	0x12
#define SC_RETURN	0x5a
#define SC_SPACE	0x29
#define SC_TAB		0x0d
#define SC_ESCAPE	0x76
#define SC_BACKSPACE	0x66
#define SC_CAPSLOCK	0x58

/** One printable character's key, and whether Shift is needed for it. */
typedef struct {
	uint8_t	code;
	uint8_t	shift;
} KeyMapping;

/**
 * ASCII to UK-layout scan code.
 *
 * Index is the character itself. A zero code means "cannot be typed".
 */
static const KeyMapping ascii_map[128] = {
	['\t'] = { SC_TAB,	0 },
	['\n'] = { SC_RETURN,	0 },
	['\r'] = { SC_RETURN,	0 },
	[0x1b] = { SC_ESCAPE,	0 },
	[0x08] = { SC_BACKSPACE, 0 },
	[' ']  = { SC_SPACE,	0 },

	['`']  = { 0x0e, 0 },	['1'] = { 0x16, 0 },	['2'] = { 0x1e, 0 },
	['3']  = { 0x26, 0 },	['4'] = { 0x25, 0 },	['5'] = { 0x2e, 0 },
	['6']  = { 0x36, 0 },	['7'] = { 0x3d, 0 },	['8'] = { 0x3e, 0 },
	['9']  = { 0x46, 0 },	['0'] = { 0x45, 0 },	['-'] = { 0x4e, 0 },
	['=']  = { 0x55, 0 },

	['q']  = { 0x15, 0 },	['w'] = { 0x1d, 0 },	['e'] = { 0x24, 0 },
	['r']  = { 0x2d, 0 },	['t'] = { 0x2c, 0 },	['y'] = { 0x35, 0 },
	['u']  = { 0x3c, 0 },	['i'] = { 0x43, 0 },	['o'] = { 0x44, 0 },
	['p']  = { 0x4d, 0 },	['['] = { 0x54, 0 },	[']'] = { 0x5b, 0 },

	['a']  = { 0x1c, 0 },	['s'] = { 0x1b, 0 },	['d'] = { 0x23, 0 },
	['f']  = { 0x2b, 0 },	['g'] = { 0x34, 0 },	['h'] = { 0x33, 0 },
	['j']  = { 0x3b, 0 },	['k'] = { 0x42, 0 },	['l'] = { 0x4b, 0 },
	[';']  = { 0x4c, 0 },	['\''] = { 0x52, 0 },	['#'] = { 0x5d, 0 },

	['\\'] = { 0x61, 0 },	['z'] = { 0x1a, 0 },	['x'] = { 0x22, 0 },
	['c']  = { 0x21, 0 },	['v'] = { 0x2a, 0 },	['b'] = { 0x32, 0 },
	['n']  = { 0x31, 0 },	['m'] = { 0x3a, 0 },	[','] = { 0x41, 0 },
	['.']  = { 0x49, 0 },	['/'] = { 0x4a, 0 },

	/* Upper case: the same keys with Shift */
	['Q']  = { 0x15, 1 },	['W'] = { 0x1d, 1 },	['E'] = { 0x24, 1 },
	['R']  = { 0x2d, 1 },	['T'] = { 0x2c, 1 },	['Y'] = { 0x35, 1 },
	['U']  = { 0x3c, 1 },	['I'] = { 0x43, 1 },	['O'] = { 0x44, 1 },
	['P']  = { 0x4d, 1 },	['A'] = { 0x1c, 1 },	['S'] = { 0x1b, 1 },
	['D']  = { 0x23, 1 },	['F'] = { 0x2b, 1 },	['G'] = { 0x34, 1 },
	['H']  = { 0x33, 1 },	['J'] = { 0x3b, 1 },	['K'] = { 0x42, 1 },
	['L']  = { 0x4b, 1 },	['Z'] = { 0x1a, 1 },	['X'] = { 0x22, 1 },
	['C']  = { 0x21, 1 },	['V'] = { 0x2a, 1 },	['B'] = { 0x32, 1 },
	['N']  = { 0x31, 1 },	['M'] = { 0x3a, 1 },

	/* Shifted punctuation, UK layout */
	['!']  = { 0x16, 1 },	['"'] = { 0x1e, 1 },	['$'] = { 0x25, 1 },
	['%']  = { 0x2e, 1 },	['^'] = { 0x36, 1 },	['&'] = { 0x3d, 1 },
	['*']  = { 0x3e, 1 },	['('] = { 0x46, 1 },	[')'] = { 0x45, 1 },
	['_']  = { 0x4e, 1 },	['+'] = { 0x55, 1 },	['{'] = { 0x54, 1 },
	['}']  = { 0x5b, 1 },	[':'] = { 0x4c, 1 },	['@'] = { 0x52, 1 },
	['~']  = { 0x5d, 1 },	['|'] = { 0x61, 1 },	['<'] = { 0x41, 1 },
	['>']  = { 0x49, 1 },	['?'] = { 0x4a, 1 },
};

/**
 * A single key transition waiting to be delivered to the i8042.
 *
 * Transitions are grouped one group per character: for an upper-case letter
 * the group is Shift down, key down, key up, Shift up. A group is delivered
 * without pausing in the middle, because pausing between Shift and the key
 * it modifies is what leaves Shift stranded and turns "Desktop" into
 * "dESKTOP".
 */
typedef struct {
	uint8_t	code[2];	/**< Scan code, second byte 0 for 1-byte codes */
	uint8_t	press;		/**< 1 = make, 0 = break */
	uint8_t	group_end;	/**< Last transition of this character */
	uint8_t	ch;		/**< Character this group is meant to produce */
	int	index;		/**< Position of that character in pending_text */
} KeyEvent;

#define TYPE_QUEUE_SIZE	1024

static KeyEvent queue[TYPE_QUEUE_SIZE];
static int queue_head;
static int queue_tail;
static uint64_t next_event_ns;

/** How many times to re-send a character the machine did not echo. */
#define TYPE_MAX_RETRIES	4

/** Minimum milliseconds between key transitions. */
static unsigned type_interval_ms = 25;

/** How long to wait for a guest that is not collecting keyboard data. */
static unsigned type_stall_ms = 2000;

/** How long to wait for a character to be echoed before giving up on it. */
static unsigned echo_timeout_ms = 500;

/** Host time after which a stalled guest is typed at regardless. */
static uint64_t stall_deadline_ns;

/* Closed-loop pacing state: wait for the machine to echo each character
   before sending the next one. */
static uint64_t echo_baseline;		/**< Output total before this character */
static uint64_t echo_deadline_ns;	/**< Give up waiting at this time */
static int awaiting_echo;
static int in_group;			/**< Mid-character, do not pause */
static int group_start_head;		/**< Queue position to rewind to on retry */
static int group_retries;		/**< Retries spent on the current character */
static unsigned echo_timeouts;		/**< Characters given up on */
static unsigned retries_used;		/**< Characters that needed re-sending */
static uint8_t group_char;		/**< Character the current group should produce */
static int caps_corrected;		/**< Caps Lock has been dealt with */
static unsigned caps_corrections;
static int caps_invert;			/**< Machine has Caps Lock on */
static char pending_text[1024];		/**< Text being typed, escapes resolved */
static int pending_len;
static int group_index;			/**< pending_text position of current group */
static unsigned events_sent;		/**< Key transitions actually delivered */

static int
is_alpha(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static uint8_t
lower_case(uint8_t c)
{
	return (c >= 'A' && c <= 'Z') ? (uint8_t) (c + 32) : c;
}

static void
queue_push_ch(uint8_t code, uint8_t press, uint8_t group_end, uint8_t ch,
              int index)
{
	const int next = (queue_tail + 1) % TYPE_QUEUE_SIZE;

	if (next == queue_head) {
		rpclog("type: queue full, character dropped\n");
		return;
	}

	queue[queue_tail].code[0] = code;
	queue[queue_tail].code[1] = 0;
	queue[queue_tail].press = press;
	queue[queue_tail].group_end = group_end;
	queue[queue_tail].ch = ch;
	queue[queue_tail].index = index;
	queue_tail = next;
}

static void
queue_push(uint8_t code, uint8_t press, uint8_t group_end)
{
	queue_push_ch(code, press, group_end, 0, -1);
}

void
headless_type_set_interval(unsigned ms)
{
	type_interval_ms = (ms > 0) ? ms : 1;
}

/**
 * Queue the transitions for one character.
 *
 * @return 0 if the character has no key on this layout
 */
static int
queue_char(unsigned char c, int index)
{
	KeyMapping m;

	if (c >= 128) {
		rpclog("type: cannot type character 0x%02x\n", c);
		return 0;
	}

	m = ascii_map[c];
	if (m.code == 0) {
		rpclog("type: no key for character 0x%02x\n", c);
		return 0;
	}

	/* With Caps Lock on the machine inverts the case of letters, so type
	   the opposite of what the layout says to get the letter asked for. */
	if (caps_invert && is_alpha(c)) {
		m.shift = !m.shift;
	}

	if (m.shift) {
		queue_push_ch(SC_LSHIFT, 1, 0, c, index);
		queue_push_ch(m.code, 1, 0, c, index);
		queue_push_ch(m.code, 0, 0, c, index);
		queue_push_ch(SC_LSHIFT, 0, 1, c, index);
	} else {
		queue_push_ch(m.code, 1, 0, c, index);
		queue_push_ch(m.code, 0, 1, c, index);
	}

	return 1;
}

/**
 * Queue everything from `index` onwards in the stored text.
 */
static void
queue_from(int index)
{
	int i;

	for (i = index; i < pending_len; i++) {
		queue_char((unsigned char) pending_text[i], i);
	}
}

/**
 * Queue a string to be typed into the machine.
 *
 * Backslash escapes are honoured: \n and \r for Return, \t for Tab, \e for
 * Escape, \\ for a literal backslash.
 *
 * @return number of characters queued, or -1 if any could not be typed
 */
int
headless_type_string(const char *text)
{
	int failed = 0;

	/* Resolve escapes once, and keep the result: if the machine turns out
	   to have Caps Lock on, the rest of the string has to be re-queued
	   with the shifts inverted. */
	pending_len = 0;
	for (; *text != '\0' && pending_len < (int) sizeof(pending_text) - 1; text++) {
		unsigned char c = (unsigned char) *text;

		if (c == '\\' && text[1] != '\0') {
			text++;
			switch (*text) {
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'e': c = 0x1b; break;
			case '0': c = 0;    break;
			case '\\': c = '\\'; break;
			default:  c = (unsigned char) *text; break;
			}
			if (c == 0) {
				continue;
			}
		}

		if (c >= 128 || ascii_map[c].code == 0) {
			rpclog("type: no key for character 0x%02x\n", c);
			failed = 1;
			continue;
		}

		pending_text[pending_len++] = (char) c;
	}
	pending_text[pending_len] = '\0';

	queue_from(0);

	return failed ? -1 : pending_len;
}

int
headless_type_busy(void)
{
	return queue_head != queue_tail;
}

/**
 * Deliver at most one key transition, if enough time has passed.
 *
 * Called from the main loop. Pacing on host time rather than instruction
 * count is deliberate for now: the guest's keyboard driver is interrupt
 * driven, and this keeps typing working at whatever speed the emulator
 * happens to run.
 *
 * @param now_ns Nanoseconds since startup
 */
void
headless_type_poll(uint64_t now_ns)
{
	const KeyEvent *ev;

	if (queue_head == queue_tail) {
		return;
	}

	/* Between characters, wait for the machine to echo the last one.
	   Closed-loop pacing beats any fixed delay: it tracks how fast the
	   guest actually is, so it neither outruns a busy machine nor crawls
	   on an idle one. If nothing is echoed — the machine is not at a
	   prompt, or is not echoing — the timeout lets typing continue. */
	if (awaiting_echo) {
		if (dbg_vdu_total() != echo_baseline) {
			/* Seen on screen: that character is safely in. */
			awaiting_echo = 0;
			group_retries = 0;

			/* RISC OS boots with Caps Lock on, so letters come back
			   with their case inverted. Rather than assume anything
			   about the machine's state, look at what actually
			   appeared: if it is the right letter in the wrong
			   case, toggle Caps Lock and send that character
			   again. Checked once, on the first letter typed. */
			if (!caps_corrected && is_alpha(group_char)) {
				char got = 0;

				caps_corrected = 1;

				if (dbg_vdu_read(&got, 1, dbg_vdu_total() - 1) == 1 &&
				    (uint8_t) got != group_char &&
				    lower_case((uint8_t) got) == lower_case(group_char))
				{
					rpclog("type: echo was '%c' not '%c',"
					       " machine has Caps Lock on;"
					       " inverting shift\n", got, group_char);
					caps_corrections++;
					caps_invert = 1;

					/* Rub out the character that came through
					   in the wrong case, then queue the rest
					   of the text again with the shifts the
					   other way round. Pressing Caps Lock
					   would be neater, but RISC OS does not
					   act on the scan code alone.

					   The Backspace goes through the queue
					   like everything else: sending it
					   directly would skip the pacing and be
					   dropped, which is the same mistake
					   this file exists to avoid. */
					queue_head = 0;
					queue_tail = 0;
					in_group = 0;
					queue_char('\b', -1);
					queue_from(group_index);

					next_event_ns = now_ns +
					    (uint64_t) type_interval_ms * 1000000ULL;
					return;
				}
			}
		} else if (now_ns < echo_deadline_ns) {
			return;
		} else if (group_retries < TYPE_MAX_RETRIES) {
			/* Nothing came back. RISC OS drops keystrokes that
			   arrive while its keyboard driver is busy, so send the
			   character again rather than pressing on and losing
			   it. Rewinding to the start of the group re-sends any
			   modifier with it. */
			group_retries++;
			queue_head = group_start_head;
			awaiting_echo = 0;
			retries_used++;
			next_event_ns = now_ns +
			    (uint64_t) type_interval_ms * 1000000ULL;
			return;
		} else {
			/* Give up on this one and keep going: the machine may
			   simply not be echoing (a password prompt, or not at a
			   prompt at all). */
			awaiting_echo = 0;
			group_retries = 0;
			echo_timeouts++;
		}
	}

	if (now_ns < next_event_ns) {
		return;
	}

	/* Wait for the guest to collect what it already has. keyboardsend()
	   overwrites the data register without checking, so injecting now
	   would destroy a byte in flight. */
	if (keyboard_output_pending()) {
		if (now_ns < stall_deadline_ns) {
			return;
		}
		/* The guest has not read for a long time: it is not listening,
		   so waiting longer achieves nothing. */
		rpclog("type: guest has not read the keyboard for %u ms,"
		       " continuing anyway\n", type_stall_ms);
	}

	next_event_ns = now_ns + (uint64_t) type_interval_ms * 1000000ULL;
	stall_deadline_ns = now_ns + (uint64_t) type_stall_ms * 1000000ULL;

	ev = &queue[queue_head];
	queue_head = (queue_head + 1) % TYPE_QUEUE_SIZE;

	/* Note where the output stream stood before the character goes in, so
	   the echo can be recognised when it arrives. */
	if (!in_group) {
		echo_baseline = dbg_vdu_total();
		group_start_head = (queue_head + TYPE_QUEUE_SIZE - 1) % TYPE_QUEUE_SIZE;
		group_char = ev->ch;
		group_index = ev->index;
		in_group = 1;
	}

	if (ev->press) {
		keyboard_key_press(ev->code);
	} else {
		keyboard_key_release(ev->code);
	}
	events_sent++;

	if (ev->group_end) {
		in_group = 0;
		awaiting_echo = 1;
		echo_deadline_ns = now_ns + (uint64_t) echo_timeout_ms * 1000000ULL;
	}
}

/**
 * How many characters were typed without the machine echoing anything.
 *
 * A non-zero count after typing at a prompt means characters may have been
 * missed; zero means every keystroke was seen and acknowledged.
 */
unsigned
headless_type_echo_timeouts(void)
{
	return echo_timeouts;
}

/** Key transitions actually delivered to the machine. */
unsigned
headless_type_events_sent(void)
{
	return events_sent;
}
