/*
  RPCEmu - An Acorn system emulator

  Minimal PNG writer for headless screen captures.

  Deliberately dependency-free: no zlib, no libpng. The deflate stream uses
  stored (uncompressed) blocks, which is legal zlib and costs about 0.03%
  size overhead. A framebuffer dump is written once and read once, so the
  simplicity is worth more than the bytes.

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
#include <stdint.h>

#include "headless.h"

#define DEFLATE_STORED_MAX	65535

/*
  A growable byte sink. The encoder writes through this rather than to a FILE
  so the same code produces a file on disk or a buffer for the control
  channel: an IDE wanting to show the screen ten times a second should not
  have to poll a file the emulator just wrote.
*/
typedef struct {
	uint8_t	*data;
	size_t	len;
	size_t	capacity;
	int	failed;
} PngSink;

static void
sink_write(PngSink *sink, const void *bytes, size_t len)
{
	if (sink->failed) {
		return;
	}

	if (sink->len + len > sink->capacity) {
		size_t capacity = (sink->capacity != 0) ? sink->capacity : 65536;
		uint8_t *grown;

		while (capacity < sink->len + len) {
			capacity *= 2;
		}

		grown = realloc(sink->data, capacity);
		if (grown == NULL) {
			sink->failed = 1;
			return;
		}
		sink->data = grown;
		sink->capacity = capacity;
	}

	memcpy(sink->data + sink->len, bytes, len);
	sink->len += len;
}

static uint32_t crc_table[256];
static int crc_table_built;

static void
crc_table_build(void)
{
	uint32_t n, c, k;

	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++) {
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		}
		crc_table[n] = c;
	}
	crc_table_built = 1;
}

static uint32_t
crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
	size_t i;

	if (!crc_table_built) {
		crc_table_build();
	}

	for (i = 0; i < len; i++) {
		crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
	}

	return crc;
}

static uint32_t
adler32_update(uint32_t adler, const uint8_t *buf, size_t len)
{
	uint32_t s1 = adler & 0xffff;
	uint32_t s2 = (adler >> 16) & 0xffff;
	size_t i;

	for (i = 0; i < len; i++) {
		s1 = (s1 + buf[i]) % 65521;
		s2 = (s2 + s1) % 65521;
	}

	return (s2 << 16) | s1;
}

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

/**
 * Write one PNG chunk: length, type, data, CRC.
 */
static void
chunk_write(PngSink *sink, const char *type, const uint8_t *data, size_t len)
{
	uint8_t hdr[8];
	uint8_t crcbuf[4];
	uint32_t crc;

	put_be32(hdr, (uint32_t) len);
	memcpy(hdr + 4, type, 4);

	sink_write(sink, hdr, 8);
	if (len != 0) {
		sink_write(sink, data, len);
	}

	crc = crc32_update(0xffffffffu, hdr + 4, 4);
	if (len != 0) {
		crc = crc32_update(crc, data, len);
	}
	crc ^= 0xffffffffu;

	put_be32(crcbuf, crc);
	sink_write(sink, crcbuf, 4);
}

/**
 * Encode an xRGB8888 image as an 8-bit truecolour PNG in memory.
 *
 * @param pixels       Image data, 0x00RRGGBB or 0xFFRRGGBB per pixel
 * @param width        Image width in pixels
 * @param height       Image height in pixels
 * @param stride_words Words per row in pixels[] (usually equal to width)
 * @param out_len      Receives the encoded length
 * @return A malloc'd PNG the caller must free, or NULL on failure
 */
uint8_t *
png_encode_xrgb(const uint32_t *pixels, int width, int height,
                int stride_words, size_t *out_len)
{
	PngSink sink = { NULL, 0, 0, 0 };
	uint8_t ihdr[13];
	uint8_t *raw = NULL;
	uint8_t *zdata = NULL;
	size_t rowbytes, rawlen, zlen, pos, done;
	uint32_t adler = 1;
	int y, x;

	*out_len = 0;

	if (pixels == NULL || width <= 0 || height <= 0) {
		return NULL;
	}

	rowbytes = (size_t) width * 3 + 1;	/* filter byte per row */
	rawlen = rowbytes * (size_t) height;

	raw = malloc(rawlen);
	if (raw == NULL) {
		return NULL;
	}

	/* Filter type 0 (None) on every row: the point here is a faithful
	   dump, not a small file. */
	for (y = 0; y < height; y++) {
		uint8_t *row = raw + (size_t) y * rowbytes;
		const uint32_t *src = pixels + (size_t) y * (size_t) stride_words;

		row[0] = 0;
		for (x = 0; x < width; x++) {
			const uint32_t px = src[x];

			row[1 + x * 3 + 0] = (uint8_t) (px >> 16);	/* R */
			row[1 + x * 3 + 1] = (uint8_t) (px >> 8);	/* G */
			row[1 + x * 3 + 2] = (uint8_t) px;		/* B */
		}
	}

	/* zlib wrapper + stored deflate blocks */
	zlen = 2 + rawlen + 5 * ((rawlen + DEFLATE_STORED_MAX - 1) / DEFLATE_STORED_MAX) + 4;
	zdata = malloc(zlen);
	if (zdata == NULL) {
		goto out;
	}

	pos = 0;
	zdata[pos++] = 0x78;	/* CM = deflate, CINFO = 32K window */
	zdata[pos++] = 0x01;	/* FCHECK such that (0x78<<8|0x01) % 31 == 0 */

	done = 0;
	while (done < rawlen) {
		size_t n = rawlen - done;
		int final;

		if (n > DEFLATE_STORED_MAX) {
			n = DEFLATE_STORED_MAX;
		}
		final = (done + n >= rawlen);

		zdata[pos++] = (uint8_t) (final ? 1 : 0);	/* BFINAL, BTYPE=00 */
		zdata[pos++] = (uint8_t) (n & 0xff);
		zdata[pos++] = (uint8_t) (n >> 8);
		zdata[pos++] = (uint8_t) (~n & 0xff);
		zdata[pos++] = (uint8_t) ((~n >> 8) & 0xff);

		memcpy(zdata + pos, raw + done, n);
		pos += n;
		done += n;
	}

	adler = adler32_update(adler, raw, rawlen);
	put_be32(zdata + pos, adler);
	pos += 4;

	sink_write(&sink, "\x89PNG\r\n\x1a\n", 8);

	put_be32(ihdr, (uint32_t) width);
	put_be32(ihdr + 4, (uint32_t) height);
	ihdr[8]  = 8;	/* bit depth */
	ihdr[9]  = 2;	/* colour type: truecolour */
	ihdr[10] = 0;	/* compression: deflate */
	ihdr[11] = 0;	/* filter method: adaptive */
	ihdr[12] = 0;	/* interlace: none */

	chunk_write(&sink, "IHDR", ihdr, sizeof(ihdr));
	chunk_write(&sink, "IDAT", zdata, pos);
	chunk_write(&sink, "IEND", NULL, 0);

out:
	free(raw);
	free(zdata);

	if (sink.failed) {
		free(sink.data);
		return NULL;
	}

	*out_len = sink.len;

	return sink.data;
}

/**
 * Write an xRGB8888 image to a PNG file.
 *
 * @return 0 on success, non-zero on failure
 */
int
png_write_xrgb(const char *path, const uint32_t *pixels,
               int width, int height, int stride_words)
{
	size_t len = 0;
	uint8_t *png = png_encode_xrgb(pixels, width, height, stride_words, &len);
	FILE *f;
	int ret;

	if (png == NULL) {
		return 1;
	}

	f = fopen(path, "wb");
	if (f == NULL) {
		free(png);
		return 1;
	}

	ret = (fwrite(png, 1, len, f) != len);
	ret |= (fclose(f) != 0);
	free(png);

	return ret;
}
