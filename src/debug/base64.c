/*
  RPCEmu - An Acorn system emulator

  Base64, for putting binary through the control channel.

  The channel is line-delimited JSON, so a frame of video has to survive being
  a JSON string. Base64 costs a third in size and nothing in dependencies,
  which is the right trade for a channel that already carries text.

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

#include <stdlib.h>

#include "dbg.h"

static const char ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Encode bytes as base64.
 *
 * @param data Bytes to encode
 * @param len  How many
 * @return A malloc'd NUL-terminated string the caller must free, or NULL
 */
char *
dbg_base64_encode(const uint8_t *data, size_t len)
{
    const size_t groups = (len + 2) / 3;
    char *out = malloc(groups * 4 + 1);
    size_t i;
    size_t o = 0;

    if (out == NULL) {
        return NULL;
    }

    for (i = 0; i + 2 < len; i += 3) {
        const uint32_t triple =
            ((uint32_t) data[i] << 16) |
            ((uint32_t) data[i + 1] << 8) |
            (uint32_t) data[i + 2];

        out[o++] = ALPHABET[(triple >> 18) & 0x3f];
        out[o++] = ALPHABET[(triple >> 12) & 0x3f];
        out[o++] = ALPHABET[(triple >> 6) & 0x3f];
        out[o++] = ALPHABET[triple & 0x3f];
    }

    /* The tail: one or two bytes left, padded to a four-character group. */
    if (i < len) {
        const uint32_t triple =
            ((uint32_t) data[i] << 16) |
            ((i + 1 < len) ? ((uint32_t) data[i + 1] << 8) : 0u);

        out[o++] = ALPHABET[(triple >> 18) & 0x3f];
        out[o++] = ALPHABET[(triple >> 12) & 0x3f];
        out[o++] = (i + 1 < len) ? ALPHABET[(triple >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }

    out[o] = '\0';

    return out;
}
