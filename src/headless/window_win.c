/*
  RPCEmu - An Acorn system emulator

  Optional live view for the headless build.

  A plain Win32 window on its own thread, blitting the frame store at the
  refresh rate. It is a viewer, not a frontend: the emulator does not depend
  on it, does not wait for it, and runs identically whether or not it is
  open. Closing the window leaves the machine running.

  GDI rather than DirectX on purpose. StretchDIBits of a 32-bit top-down DIB
  is a straight memory blit for the driver, costs nothing at these sizes, and
  needs no SDK beyond what every Windows install has. VIDC already hands us
  0x00RRGGBB pixels, which is exactly a BI_RGB 32bpp DIB, so there is no
  conversion. A DirectX or Metal backend can replace this file without the
  rest of the emulator noticing, because the interface it consumes is the
  frame store.

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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "headless.h"

#define WINDOW_CLASS	"RPCEmuHeadlessView"
#define REFRESH_TIMER	1

static HANDLE window_thread;
static HWND window_handle;
static volatile LONG window_quit;

/* The blit buffer, owned by the window thread. Keeping it here rather than
   allocating per frame means a steady-state redraw does no allocation. */
static uint32_t *blit_pixels;
static size_t blit_capacity;
static int blit_width;
static int blit_height;
static uint64_t blit_serial;

/**
 * Refresh the blit buffer from the frame store, if there is a new frame.
 *
 * @return Non-zero if the buffer changed and the window should repaint
 */
static int
blit_update(void)
{
	HeadlessFrame f;

	if (headless_frame_serial() == blit_serial) {
		return 0;
	}

	if (headless_frame_copy(&f) != 0) {
		return 0;
	}

	{
		const size_t words = (size_t) f.xsize * (size_t) f.ysize;

		if (words > blit_capacity) {
			uint32_t *p = realloc(blit_pixels, words * sizeof(uint32_t));

			if (p == NULL) {
				headless_frame_free(&f);
				return 0;
			}
			blit_pixels = p;
			blit_capacity = words;
		}

		memcpy(blit_pixels, f.pixels, words * sizeof(uint32_t));
		blit_width = f.xsize;
		blit_height = f.ysize;
		blit_serial = f.serial;
	}

	headless_frame_free(&f);

	return 1;
}

static void
blit_paint(HDC dc, const RECT *client)
{
	BITMAPINFO bmi;

	if (blit_pixels == NULL || blit_width <= 0 || blit_height <= 0) {
		return;
	}

	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
	bmi.bmiHeader.biWidth       = blit_width;
	bmi.bmiHeader.biHeight      = -blit_height;	/* negative: top-down */
	bmi.bmiHeader.biPlanes      = 1;
	bmi.bmiHeader.biBitCount    = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	SetStretchBltMode(dc, COLORONCOLOR);

	StretchDIBits(dc,
	              0, 0, client->right, client->bottom,
	              0, 0, blit_width, blit_height,
	              blit_pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK
window_proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_TIMER:
		if (wparam == REFRESH_TIMER && blit_update()) {
			InvalidateRect(wnd, NULL, FALSE);
		}
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		RECT client;
		HDC dc = BeginPaint(wnd, &ps);

		GetClientRect(wnd, &client);
		blit_paint(dc, &client);
		EndPaint(wnd, &ps);
		return 0;
	}

	case WM_ERASEBKGND:
		/* The blit covers the whole client area; erasing first only
		   makes it flicker. */
		return 1;

	case WM_CLOSE:
		/* Closing the viewer must not stop the machine. */
		DestroyWindow(wnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	default:
		return DefWindowProcA(wnd, msg, wparam, lparam);
	}
}

static DWORD WINAPI
window_thread_runner(LPVOID param)
{
	WNDCLASSA wc;
	MSG msg;
	const int refresh = (config.refresh > 0) ? config.refresh : 60;

	NOT_USED(param);

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc   = window_proc;
	wc.hInstance     = GetModuleHandleA(NULL);
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = WINDOW_CLASS;

	if (!RegisterClassA(&wc)) {
		rpclog("window: RegisterClass failed (%lu)\n",
		       (unsigned long) GetLastError());
		return 1;
	}

	window_handle = CreateWindowExA(
	    0, WINDOW_CLASS, "RPCEmu (headless view)",
	    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
	    CW_USEDEFAULT, CW_USEDEFAULT, 816, 636,
	    NULL, NULL, wc.hInstance, NULL);

	if (window_handle == NULL) {
		rpclog("window: CreateWindow failed (%lu)\n",
		       (unsigned long) GetLastError());
		return 1;
	}

	SetTimer(window_handle, REFRESH_TIMER, 1000 / refresh, NULL);

	while (!window_quit && GetMessageA(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	window_handle = NULL;

	return 0;
}

/**
 * Open the live view window.
 *
 * Runs on its own thread with its own message loop, so a stalled emulator
 * never freezes the view and a stalled view never slows the emulator.
 */
void
headless_window_open(void)
{
	window_quit = 0;

	window_thread = CreateThread(NULL, 0, window_thread_runner, NULL, 0, NULL);
	if (window_thread == NULL) {
		error("could not create the view window thread\n");
	}
}

void
headless_window_close(void)
{
	if (window_thread == NULL) {
		return;
	}

	InterlockedExchange(&window_quit, 1);
	if (window_handle != NULL) {
		PostMessageA(window_handle, WM_CLOSE, 0, 0);
	}

	WaitForSingleObject(window_thread, 1000);
	CloseHandle(window_thread);
	window_thread = NULL;

	free(blit_pixels);
	blit_pixels = NULL;
	blit_capacity = 0;
}
