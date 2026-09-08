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
static int
chunk_write(FILE *f, const char *type, const uint8_t *data, size_t len)
{
	uint8_t hdr[8];
	uint8_t crcbuf[4];
	uint32_t crc;

	put_be32(hdr, (uint32_t) len);
	memcpy(hdr + 4, type, 4);

	if (fwrite(hdr, 1, 8, f) != 8) {
		return 1;
	}
	if (len != 0 && fwrite(data, 1, len, f) != len) {
		return 1;
	}

	crc = crc32_update(0xffffffffu, hdr + 4, 4);
	if (len != 0) {
		crc = crc32_update(crc, data, len);
	}
	crc ^= 0xffffffffu;

	put_be32(crcbuf, crc);

	return fwrite(crcbuf, 1, 4, f) != 4;
}

/**
 * Write an xRGB8888 image to a PNG file as 8-bit truecolour.
 *
 * @param path         Destination file
 * @param pixels       Image data, 0x00RRGGBB or 0xFFRRGGBB per pixel
 * @param width        Image width in pixels
 * @param height       Image height in pixels
 * @param stride_words Words per row in pixels[] (usually equal to width)
 * @return 0 on success, non-zero on failure
 */
int
png_write_xrgb(const char *path, const uint32_t *pixels,
               int width, int height, int stride_words)
{
	FILE *f;
	uint8_t ihdr[13];
	uint8_t *raw = NULL;
	uint8_t *zdata = NULL;
	size_t rowbytes, rawlen, zlen, pos, done;
	uint32_t adler = 1;
	int y, x, ret = 1;

	if (pixels == NULL || width <= 0 || height <= 0) {
		return 1;
	}

	rowbytes = (size_t) width * 3 + 1;	/* filter byte per row */
	rawlen = rowbytes * (size_t) height;

	raw = malloc(rawlen);
	if (raw == NULL) {
		return 1;
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

	f = fopen(path, "wb");
	if (f == NULL) {
		goto out;
	}

	if (fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) != 8) {
		fclose(f);
		goto out;
	}

	put_be32(ihdr, (uint32_t) width);
	put_be32(ihdr + 4, (uint32_t) height);
	ihdr[8]  = 8;	/* bit depth */
	ihdr[9]  = 2;	/* colour type: truecolour */
	ihdr[10] = 0;	/* compression: deflate */
	ihdr[11] = 0;	/* filter method: adaptive */
	ihdr[12] = 0;	/* interlace: none */

	if (chunk_write(f, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
	    chunk_write(f, "IDAT", zdata, pos) != 0 ||
	    chunk_write(f, "IEND", NULL, 0) != 0)
	{
		fclose(f);
		goto out;
	}

	ret = (fclose(f) != 0);

out:
	free(raw);
	free(zdata);

	return ret;
}
