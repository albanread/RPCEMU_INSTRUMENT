/*
  RPCEmu - An Acorn system emulator

  Synthetic pointer, so the desktop can be driven as well as looked at.

  Everything the instrumentation could do until now was for a machine at a
  command prompt: type a line, read what came back. The RISC OS desktop
  answers to a pointer, and without one the whole graphical half of the
  system is something you can watch and not touch.

  RPCEmu already tracks an emulated pointer; in mousehack mode - which the
  headless build enables by default - its position is absolute and given in
  the same pixels the frame is published in, origin top left. So this is
  mostly a matter of exposing what is there, plus the one thing a caller
  cannot do for itself: holding a button down long enough for the guest to
  notice.

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

#include <string.h>

#include "rpcemu.h"
#include "keyboard.h"
#include "vidc20.h"
#include "headless.h"

/*
  How long a synthetic click holds the button down.

  RISC OS learns about the mouse by polling OS_Mouse, and the desktop polls
  it once per null event - tens of milliseconds apart. A press and release
  in the same instant is simply never seen. Eighty milliseconds is longer
  than any poll interval and short enough to feel instant.
*/
#define CLICK_HOLD_NS	(80ull * 1000000ull)

/*
  And how long to wait after moving before pressing.

  The desktop decides what a click means from where the pointer is when the
  button goes down, which it also learns by polling. Moving and clicking in
  the same instant can put the press at the old position.
*/
#define MOVE_SETTLE_NS	(40ull * 1000000ull)

static int	pending_buttons;	/**< Held by a click, waiting to be released */
static uint64_t	release_at_ns;
static uint64_t	press_at_ns;
static int	press_buttons;

static unsigned	moves;
static unsigned	clicks;

/**
 * Host button bits for a RISC OS button.
 *
 * The core maps host bits to what the guest sees, and the mapping is not the
 * identity: it depends on the two-button setting, which swaps middle and
 * right for people whose mice have neither. Going through it backwards here
 * means a caller asking for Menu gets Menu whatever the machine is
 * configured to believe about the pointer plugged into it.
 */
static int
host_bits(int riscos_buttons)
{
	int bits = 0;

	if (riscos_buttons & MOUSE_SELECT) {
		bits |= 1;
	}

	if (config.mousetwobutton) {
		if (riscos_buttons & MOUSE_MENU) {
			bits |= 2;
		}
		if (riscos_buttons & MOUSE_ADJUST) {
			bits |= 4;
		}
	} else {
		if (riscos_buttons & MOUSE_ADJUST) {
			bits |= 2;
		}
		if (riscos_buttons & MOUSE_MENU) {
			bits |= 4;
		}
	}

	return bits;
}

/**
 * Put the pointer somewhere.
 *
 * @param x Frame pixels from the left
 * @param y Frame pixels from the top
 */
void
headless_mouse_move(int x, int y)
{
	const int width = vidc_get_xsize();
	const int height = vidc_get_ysize();

	if (!mousehack) {
		/* Capture mode takes relative movements and there is no host
		   pointer here to take them from. */
		return;
	}

	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}
	if (width > 0 && x >= width) {
		x = width - 1;
	}
	if (height > 0 && y >= height) {
		y = height - 1;
	}

	mouse_mouse_move(x, y);
	moves++;
}

void
headless_mouse_down(int riscos_buttons)
{
	mouse_mouse_press(host_bits(riscos_buttons));
}

void
headless_mouse_up(int riscos_buttons)
{
	mouse_mouse_release(host_bits(riscos_buttons));
}

/**
 * Move, press and release, with enough time between for the guest to see it.
 *
 * @param x Frame pixels from the left, or -1 to click where the pointer is
 * @param y Frame pixels from the top
 * @param riscos_buttons Buttons to click
 * @param now_ns Host time
 * @return 0 if the click was accepted, non-zero if one is already in flight
 */
int
headless_mouse_click(int x, int y, int riscos_buttons, uint64_t now_ns)
{
	if (pending_buttons != 0 || press_buttons != 0) {
		return 1;
	}

	if (x >= 0 && y >= 0) {
		headless_mouse_move(x, y);
		press_at_ns = now_ns + MOVE_SETTLE_NS;
	} else {
		press_at_ns = now_ns;
	}

	press_buttons = riscos_buttons;
	clicks++;

	return 0;
}

/** Whether a click is still being delivered. */
int
headless_mouse_busy(void)
{
	return (pending_buttons != 0) || (press_buttons != 0);
}

/**
 * Advance a click in flight.
 *
 * Called from the main loop beside the typist, and for the same reason: the
 * guest has to be allowed to run between the parts of the gesture.
 */
void
headless_mouse_poll(uint64_t now_ns)
{
	if (press_buttons != 0 && now_ns >= press_at_ns) {
		headless_mouse_down(press_buttons);
		pending_buttons = press_buttons;
		press_buttons = 0;
		release_at_ns = now_ns + CLICK_HOLD_NS;
		return;
	}

	if (pending_buttons != 0 && now_ns >= release_at_ns) {
		headless_mouse_up(pending_buttons);
		pending_buttons = 0;
	}
}

/**
 * Where the pointer is and what is held, as the guest would be told.
 */
void
headless_mouse_state(int *x, int *y, int *riscos_buttons)
{
	int px = 0;
	int py = 0;

	mouse_position_get(&px, &py);

	if (x != NULL) {
		*x = px;
	}
	if (y != NULL) {
		*y = py;
	}

	if (riscos_buttons != NULL) {
		const int held = mouse_buttons_get();
		int out = 0;

		if (held & 1) {
			out |= MOUSE_SELECT;
		}

		if (config.mousetwobutton) {
			if (held & 2) {
				out |= MOUSE_MENU;
			}
			if (held & 4) {
				out |= MOUSE_ADJUST;
			}
		} else {
			if (held & 2) {
				out |= MOUSE_ADJUST;
			}
			if (held & 4) {
				out |= MOUSE_MENU;
			}
		}

		*riscos_buttons = out;
	}
}

unsigned
headless_mouse_moves(void)
{
	return moves;
}

unsigned
headless_mouse_clicks(void)
{
	return clicks;
}
