/*
  RPCEmu - An Acorn system emulator

  A small JSON reader and writer. See json.h.

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
#include <stdarg.h>

#include "json.h"

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
	char		*p;	/**< Current position, written through */
	JsonDoc		*doc;
} Parser;

static int parse_value(Parser *ps);

static void
skip_space(Parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' ||
	       *ps->p == '\n')
	{
		ps->p++;
	}
}

static int
node_new(Parser *ps, JsonType type)
{
	JsonValue *v;

	if (ps->doc->count >= ps->doc->capacity) {
		ps->doc->error = "too many JSON nodes";
		return -1;
	}

	v = &ps->doc->nodes[ps->doc->count];
	memset(v, 0, sizeof(*v));
	v->type = type;
	v->first_child = -1;
	v->next_sibling = -1;

	return ps->doc->count++;
}

/**
 * Read a quoted string, unescaping into the same buffer.
 *
 * Unescaping only ever shortens the text, so writing over the input is
 * safe. Returns a pointer to the terminated result.
 */
static char *
parse_string(Parser *ps)
{
	char *out;
	char *start;

	if (*ps->p != '"') {
		ps->doc->error = "expected a string";
		return NULL;
	}
	ps->p++;

	start = ps->p;
	out = ps->p;

	while (*ps->p != '"') {
		if (*ps->p == '\0') {
			ps->doc->error = "unterminated string";
			return NULL;
		}

		if (*ps->p == '\\') {
			ps->p++;
			switch (*ps->p) {
			case '"':  *out++ = '"';  break;
			case '\\': *out++ = '\\'; break;
			case '/':  *out++ = '/';  break;
			case 'b':  *out++ = '\b'; break;
			case 'f':  *out++ = '\f'; break;
			case 'n':  *out++ = '\n'; break;
			case 'r':  *out++ = '\r'; break;
			case 't':  *out++ = '\t'; break;
			case 'u': {
				unsigned code = 0;
				int i;

				for (i = 0; i < 4; i++) {
					const char c = ps->p[1 + i];

					code <<= 4;
					if (c >= '0' && c <= '9') {
						code |= (unsigned) (c - '0');
					} else if (c >= 'a' && c <= 'f') {
						code |= (unsigned) (c - 'a' + 10);
					} else if (c >= 'A' && c <= 'F') {
						code |= (unsigned) (c - 'A' + 10);
					} else {
						ps->doc->error = "bad \\u escape";
						return NULL;
					}
				}
				ps->p += 4;

				/* UTF-8, enough for the BMP. Surrogate pairs
				   are not needed by this protocol. */
				if (code < 0x80) {
					*out++ = (char) code;
				} else if (code < 0x800) {
					*out++ = (char) (0xc0 | (code >> 6));
					*out++ = (char) (0x80 | (code & 0x3f));
				} else {
					*out++ = (char) (0xe0 | (code >> 12));
					*out++ = (char) (0x80 | ((code >> 6) & 0x3f));
					*out++ = (char) (0x80 | (code & 0x3f));
				}
				break;
			}
			default:
				ps->doc->error = "bad escape";
				return NULL;
			}
			ps->p++;
		} else {
			*out++ = *ps->p++;
		}
	}

	ps->p++;	/* closing quote */
	*out = '\0';

	return start;
}

static int
parse_object(Parser *ps)
{
	const int node = node_new(ps, JSON_OBJECT);
	int last = -1;

	if (node < 0) {
		return -1;
	}

	ps->p++;	/* '{' */
	skip_space(ps);

	if (*ps->p == '}') {
		ps->p++;
		return node;
	}

	for (;;) {
		char *key;
		int child;

		skip_space(ps);
		key = parse_string(ps);
		if (key == NULL) {
			return -1;
		}

		skip_space(ps);
		if (*ps->p != ':') {
			ps->doc->error = "expected ':'";
			return -1;
		}
		ps->p++;
		skip_space(ps);

		child = parse_value(ps);
		if (child < 0) {
			return -1;
		}

		ps->doc->nodes[child].key = key;
		if (last < 0) {
			ps->doc->nodes[node].first_child = child;
		} else {
			ps->doc->nodes[last].next_sibling = child;
		}
		last = child;

		skip_space(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			return node;
		}
		ps->doc->error = "expected ',' or '}'";
		return -1;
	}
}

static int
parse_array(Parser *ps)
{
	const int node = node_new(ps, JSON_ARRAY);
	int last = -1;

	if (node < 0) {
		return -1;
	}

	ps->p++;	/* '[' */
	skip_space(ps);

	if (*ps->p == ']') {
		ps->p++;
		return node;
	}

	for (;;) {
		int child;

		skip_space(ps);
		child = parse_value(ps);
		if (child < 0) {
			return -1;
		}

		if (last < 0) {
			ps->doc->nodes[node].first_child = child;
		} else {
			ps->doc->nodes[last].next_sibling = child;
		}
		last = child;

		skip_space(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			return node;
		}
		ps->doc->error = "expected ',' or ']'";
		return -1;
	}
}

static int
parse_value(Parser *ps)
{
	skip_space(ps);

	switch (*ps->p) {
	case '{':
		return parse_object(ps);

	case '[':
		return parse_array(ps);

	case '"': {
		const int node = node_new(ps, JSON_STRING);
		char *s;

		if (node < 0) {
			return -1;
		}
		s = parse_string(ps);
		if (s == NULL) {
			return -1;
		}
		ps->doc->nodes[node].string = s;
		return node;
	}

	case 't':
		if (strncmp(ps->p, "true", 4) == 0) {
			const int node = node_new(ps, JSON_BOOL);

			if (node >= 0) {
				ps->doc->nodes[node].boolean = 1;
				ps->p += 4;
			}
			return node;
		}
		break;

	case 'f':
		if (strncmp(ps->p, "false", 5) == 0) {
			const int node = node_new(ps, JSON_BOOL);

			if (node >= 0) {
				ps->doc->nodes[node].boolean = 0;
				ps->p += 5;
			}
			return node;
		}
		break;

	case 'n':
		if (strncmp(ps->p, "null", 4) == 0) {
			const int node = node_new(ps, JSON_NULL);

			if (node >= 0) {
				ps->p += 4;
			}
			return node;
		}
		break;

	default:
		break;
	}

	/* Number, including the hex form &NNNN and 0xNNNN that addresses are
	   naturally written in. Neither is standard JSON, but a control
	   channel for an ARM machine is much easier to drive with them. */
	{
		const int node = node_new(ps, JSON_NUMBER);
		char *end = NULL;

		if (node < 0) {
			return -1;
		}

		if (ps->p[0] == '&') {
			ps->doc->nodes[node].number =
			    (double) strtoull(ps->p + 1, &end, 16);
		} else if (ps->p[0] == '0' && (ps->p[1] == 'x' || ps->p[1] == 'X')) {
			ps->doc->nodes[node].number =
			    (double) strtoull(ps->p + 2, &end, 16);
		} else {
			ps->doc->nodes[node].number = strtod(ps->p, &end);
		}

		if (end == NULL || end == ps->p) {
			ps->doc->error = "expected a value";
			return -1;
		}
		ps->p = end;
		return node;
	}
}

int
json_parse(JsonDoc *doc, char *text, JsonValue *pool, int pool_size)
{
	Parser ps;
	int root;

	doc->nodes = pool;
	doc->capacity = pool_size;
	doc->count = 0;
	doc->error = NULL;

	ps.p = text;
	ps.doc = doc;

	root = parse_value(&ps);
	if (root < 0 && doc->error == NULL) {
		doc->error = "parse failed";
	}

	return root;
}

const JsonValue *
json_member(const JsonDoc *doc, int object, const char *key)
{
	int child;

	if (object < 0 || object >= doc->count ||
	    doc->nodes[object].type != JSON_OBJECT)
	{
		return NULL;
	}

	for (child = doc->nodes[object].first_child; child >= 0;
	     child = doc->nodes[child].next_sibling)
	{
		if (doc->nodes[child].key != NULL &&
		    strcmp(doc->nodes[child].key, key) == 0)
		{
			return &doc->nodes[child];
		}
	}

	return NULL;
}

const char *
json_string(const JsonValue *v, const char *fallback)
{
	return (v != NULL && v->type == JSON_STRING) ? v->string : fallback;
}

double
json_number(const JsonValue *v, double fallback)
{
	return (v != NULL && v->type == JSON_NUMBER) ? v->number : fallback;
}

long long
json_int(const JsonValue *v, long long fallback)
{
	return (v != NULL && v->type == JSON_NUMBER) ?
	       (long long) v->number : fallback;
}

int
json_bool(const JsonValue *v, int fallback)
{
	if (v == NULL) {
		return fallback;
	}
	if (v->type == JSON_BOOL) {
		return v->boolean;
	}
	if (v->type == JSON_NUMBER) {
		return v->number != 0.0;
	}

	return fallback;
}

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

void
json_out_init(JsonOut *out)
{
	out->buf = NULL;
	out->len = 0;
	out->capacity = 0;
	out->failed = 0;
}

void
json_out_free(JsonOut *out)
{
	free(out->buf);
	json_out_init(out);
}

void
json_out_reset(JsonOut *out)
{
	out->len = 0;
	out->failed = 0;
	if (out->buf != NULL) {
		out->buf[0] = '\0';
	}
}

static int
out_reserve(JsonOut *out, size_t extra)
{
	size_t need = out->len + extra + 1;
	size_t cap;
	char *p;

	if (out->failed) {
		return 0;
	}
	if (need <= out->capacity) {
		return 1;
	}

	cap = (out->capacity != 0) ? out->capacity : 256;
	while (cap < need) {
		cap *= 2;
	}

	p = realloc(out->buf, cap);
	if (p == NULL) {
		out->failed = 1;
		return 0;
	}
	out->buf = p;
	out->capacity = cap;

	return 1;
}

void
json_out_raw(JsonOut *out, const char *text)
{
	const size_t n = strlen(text);

	if (!out_reserve(out, n)) {
		return;
	}
	memcpy(out->buf + out->len, text, n);
	out->len += n;
	out->buf[out->len] = '\0';
}

void
json_out_stringn(JsonOut *out, const char *s, size_t len)
{
	size_t i;

	/* Worst case is \uXXXX for every byte. */
	if (!out_reserve(out, len * 6 + 2)) {
		return;
	}

	out->buf[out->len++] = '"';

	for (i = 0; i < len; i++) {
		const unsigned char c = (unsigned char) s[i];

		switch (c) {
		case '"':  memcpy(out->buf + out->len, "\\\"", 2); out->len += 2; break;
		case '\\': memcpy(out->buf + out->len, "\\\\", 2); out->len += 2; break;
		case '\b': memcpy(out->buf + out->len, "\\b", 2);  out->len += 2; break;
		case '\f': memcpy(out->buf + out->len, "\\f", 2);  out->len += 2; break;
		case '\n': memcpy(out->buf + out->len, "\\n", 2);  out->len += 2; break;
		case '\r': memcpy(out->buf + out->len, "\\r", 2);  out->len += 2; break;
		case '\t': memcpy(out->buf + out->len, "\\t", 2);  out->len += 2; break;
		default:
			if (c < 0x20 || c >= 0x7f) {
				/* Escape control bytes, and anything above
				   ASCII: VDU output is bytes, not text, and
				   must survive as valid UTF-8 JSON. */
				out->len += (size_t) sprintf(out->buf + out->len,
				                             "\\u%04x", c);
			} else {
				out->buf[out->len++] = (char) c;
			}
			break;
		}
	}

	out->buf[out->len++] = '"';
	out->buf[out->len] = '\0';
}

void
json_out_string(JsonOut *out, const char *s)
{
	json_out_stringn(out, (s != NULL) ? s : "", (s != NULL) ? strlen(s) : 0);
}

void
json_out_printf(JsonOut *out, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(NULL, 0, format, ap);
	va_end(ap);

	if (n < 0 || !out_reserve(out, (size_t) n)) {
		return;
	}

	va_start(ap, format);
	vsnprintf(out->buf + out->len, (size_t) n + 1, format, ap);
	va_end(ap);

	out->len += (size_t) n;
}
