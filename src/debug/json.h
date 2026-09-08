/*
  RPCEmu - An Acorn system emulator

  A small JSON reader and writer, enough for the control protocol.

  Deliberately minimal and allocation-light: parsing works in place on a
  mutable buffer using a caller-supplied node pool, so handling a command
  allocates nothing. There is no dependency to add to the build and nothing
  to keep in step with upstream.

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

#ifndef DBG_JSON_H
#define DBG_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} JsonType;

typedef struct {
	JsonType	type;
	double		number;
	int		boolean;
	const char	*string;	/**< Points into the parsed buffer */
	const char	*key;		/**< Member name, for object children */
	int		first_child;	/**< Node index, or -1 */
	int		next_sibling;	/**< Node index, or -1 */
} JsonValue;

typedef struct {
	JsonValue	*nodes;
	int		capacity;
	int		count;
	const char	*error;		/**< NULL if the parse succeeded */
} JsonDoc;

/**
 * Parse JSON in place.
 *
 * `text` is modified: strings are unescaped and terminated where they sit.
 *
 * @return Index of the root node, or -1 on error (doc->error says why)
 */
extern int json_parse(JsonDoc *doc, char *text, JsonValue *pool, int pool_size);

/** Look up a member of an object node. Returns NULL if absent. */
extern const JsonValue *json_member(const JsonDoc *doc, int object,
                                    const char *key);

extern const char *json_string(const JsonValue *v, const char *fallback);
extern double json_number(const JsonValue *v, double fallback);
extern long long json_int(const JsonValue *v, long long fallback);
extern int json_bool(const JsonValue *v, int fallback);

/** A growable output buffer for building responses. */
typedef struct {
	char	*buf;
	size_t	len;
	size_t	capacity;
	int	failed;		/**< Set if the buffer could not grow */
} JsonOut;

extern void json_out_init(JsonOut *out);
extern void json_out_free(JsonOut *out);
extern void json_out_reset(JsonOut *out);

/** Append text verbatim; the caller guarantees it is valid JSON. */
extern void json_out_raw(JsonOut *out, const char *text);

/** Append a quoted, escaped JSON string. */
extern void json_out_string(JsonOut *out, const char *s);

/** Append the same, for a string that is not NUL terminated. */
extern void json_out_stringn(JsonOut *out, const char *s, size_t len);

extern void json_out_printf(JsonOut *out, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* DBG_JSON_H */
