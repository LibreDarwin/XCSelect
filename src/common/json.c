/*
 * json.c -- a JSON document model.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

struct parser {
	const char *p;
	const char *end;
};

static json_node *parse_value(struct parser *ps);

static json_node *
node_new(json_type type)
{
	json_node *n = calloc(1, sizeof(*n));

	if (n != NULL)
		n->type = type;
	return n;
}

static int
node_append(json_node *parent, json_node *child)
{
	if (parent->count == parent->cap) {
		size_t cap = (parent->cap == 0) ? 8 : parent->cap * 2;
		json_node **items = realloc(parent->items, cap * sizeof(*items));

		if (items == NULL)
			return -1;
		parent->items = items;
		parent->cap = cap;
	}

	parent->items[parent->count++] = child;
	return 0;
}

static void
skip_ws(struct parser *ps)
{
	while (ps->p < ps->end && isspace((unsigned char)*ps->p))
		ps->p++;
}

static int
at(struct parser *ps, char c)
{
	return ps->p < ps->end && *ps->p == c;
}

/* Append a code point to buf as UTF-8. */
static void
put_utf8(char **out, unsigned long cp)
{
	char *o = *out;

	if (cp < 0x80) {
		*o++ = (char)cp;
	} else if (cp < 0x800) {
		*o++ = (char)(0xC0 | (cp >> 6));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*o++ = (char)(0xE0 | (cp >> 12));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*o++ = (char)(0xF0 | (cp >> 18));
		*o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	}

	*out = o;
}

static int
hex4(struct parser *ps, unsigned long *out)
{
	unsigned long v = 0;
	int i;

	if (ps->end - ps->p < 4)
		return -1;

	for (i = 0; i < 4; i++) {
		char c = ps->p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned long)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned long)(c - 'A' + 10);
		else
			return -1;
	}

	ps->p += 4;
	*out = v;
	return 0;
}

/*
 * Parse a string literal.  The result is never longer than the source,
 * since every escape shrinks: even \uXXXX pairs, which yield at most
 * four bytes of UTF-8 from six characters.
 */
static char *
parse_string(struct parser *ps)
{
	char *buf, *out;

	if (!at(ps, '"'))
		return NULL;
	ps->p++;

	if ((buf = malloc((size_t)(ps->end - ps->p) + 1)) == NULL)
		return NULL;
	out = buf;

	while (ps->p < ps->end && *ps->p != '"') {
		if (*ps->p != '\\') {
			*out++ = *ps->p++;
			continue;
		}

		ps->p++;
		if (ps->p >= ps->end)
			goto bad;

		switch (*ps->p++) {
		case '"':  *out++ = '"';  break;
		case '\\': *out++ = '\\'; break;
		case '/':  *out++ = '/';  break;
		case 'b':  *out++ = '\b'; break;
		case 'f':  *out++ = '\f'; break;
		case 'n':  *out++ = '\n'; break;
		case 'r':  *out++ = '\r'; break;
		case 't':  *out++ = '\t'; break;
		case 'u': {
			unsigned long cp;

			if (hex4(ps, &cp) != 0)
				goto bad;

			/*
			 * A character outside the basic plane arrives as a
			 * surrogate pair; the two halves are meaningless
			 * apart, so the low half is taken here rather than
			 * emitted on its own.
			 */
			if (cp >= 0xD800 && cp <= 0xDBFF &&
			    ps->end - ps->p >= 6 &&
			    ps->p[0] == '\\' && ps->p[1] == 'u') {
				unsigned long lo;

				ps->p += 2;
				if (hex4(ps, &lo) != 0)
					goto bad;
				if (lo >= 0xDC00 && lo <= 0xDFFF)
					cp = 0x10000 +
					    ((cp - 0xD800) << 10) + (lo - 0xDC00);
				else
					goto bad;
			}

			put_utf8(&out, cp);
			break;
		}
		default:
			goto bad;
		}
	}

	if (!at(ps, '"'))
		goto bad;
	ps->p++;

	*out = '\0';
	return buf;

bad:
	free(buf);
	return NULL;
}

static json_node *
parse_object(struct parser *ps)
{
	json_node *obj = node_new(JSON_OBJECT);

	if (obj == NULL)
		return NULL;

	ps->p++;	/* '{' */
	skip_ws(ps);

	if (at(ps, '}')) {
		ps->p++;
		return obj;
	}

	for (;;) {
		json_node *member;
		char *key;

		skip_ws(ps);
		if ((key = parse_string(ps)) == NULL)
			goto bad;

		skip_ws(ps);
		if (!at(ps, ':')) {
			free(key);
			goto bad;
		}
		ps->p++;

		if ((member = parse_value(ps)) == NULL) {
			free(key);
			goto bad;
		}
		member->key = key;

		if (node_append(obj, member) != 0) {
			json_free(member);
			goto bad;
		}

		skip_ws(ps);
		if (at(ps, ',')) {
			ps->p++;
			continue;
		}
		if (at(ps, '}')) {
			ps->p++;
			return obj;
		}
		goto bad;
	}

bad:
	json_free(obj);
	return NULL;
}

static json_node *
parse_array(struct parser *ps)
{
	json_node *arr = node_new(JSON_ARRAY);

	if (arr == NULL)
		return NULL;

	ps->p++;	/* '[' */
	skip_ws(ps);

	if (at(ps, ']')) {
		ps->p++;
		return arr;
	}

	for (;;) {
		json_node *item = parse_value(ps);

		if (item == NULL)
			goto bad;
		if (node_append(arr, item) != 0) {
			json_free(item);
			goto bad;
		}

		skip_ws(ps);
		if (at(ps, ',')) {
			ps->p++;
			continue;
		}
		if (at(ps, ']')) {
			ps->p++;
			return arr;
		}
		goto bad;
	}

bad:
	json_free(arr);
	return NULL;
}

static int
literal(struct parser *ps, const char *word)
{
	size_t n = strlen(word);

	if ((size_t)(ps->end - ps->p) < n || strncmp(ps->p, word, n) != 0)
		return 0;

	ps->p += n;
	return 1;
}

static json_node *
parse_value(struct parser *ps)
{
	json_node *n;

	skip_ws(ps);
	if (ps->p >= ps->end)
		return NULL;

	switch (*ps->p) {
	case '{':
		return parse_object(ps);
	case '[':
		return parse_array(ps);
	case '"': {
		char *s = parse_string(ps);

		if (s == NULL)
			return NULL;
		if ((n = node_new(JSON_STRING)) == NULL) {
			free(s);
			return NULL;
		}
		n->string = s;
		return n;
	}
	case 't':
		if (!literal(ps, "true"))
			return NULL;
		if ((n = node_new(JSON_BOOL)) != NULL)
			n->boolean = 1;
		return n;
	case 'f':
		if (!literal(ps, "false"))
			return NULL;
		return node_new(JSON_BOOL);
	case 'n':
		if (!literal(ps, "null"))
			return NULL;
		return node_new(JSON_NULL);
	default: {
		char *endp = NULL;
		double v = strtod(ps->p, &endp);

		if (endp == NULL || endp == ps->p || endp > ps->end)
			return NULL;
		ps->p = endp;
		if ((n = node_new(JSON_NUMBER)) != NULL)
			n->number = v;
		return n;
	}
	}
}

json_node *
json_parse(const char *text, size_t len)
{
	struct parser ps;
	json_node *root;

	if (text == NULL)
		return NULL;

	ps.p = text;
	ps.end = text + len;

	if ((root = parse_value(&ps)) == NULL)
		return NULL;

	/* Anything after the root value means the document is not what it claims. */
	skip_ws(&ps);
	if (ps.p != ps.end) {
		json_free(root);
		return NULL;
	}

	return root;
}

void
json_free(json_node *node)
{
	size_t i;

	if (node == NULL)
		return;

	for (i = 0; i < node->count; i++)
		json_free(node->items[i]);

	free(node->items);
	free(node->key);
	free(node->string);
	free(node);
}

json_node *
json_object_get(const json_node *obj, const char *key)
{
	size_t i;

	if (obj == NULL || obj->type != JSON_OBJECT || key == NULL)
		return NULL;

	for (i = 0; i < obj->count; i++)
		if (obj->items[i]->key != NULL &&
		    strcmp(obj->items[i]->key, key) == 0)
			return obj->items[i];

	return NULL;
}

json_node *
json_array_at(const json_node *array, size_t index)
{
	if (array == NULL || array->type != JSON_ARRAY || index >= array->count)
		return NULL;

	return array->items[index];
}

size_t
json_count(const json_node *node)
{
	return (node != NULL) ? node->count : 0;
}

const char *
json_string(const json_node *node)
{
	if (node == NULL || node->type != JSON_STRING)
		return NULL;

	return node->string;
}
