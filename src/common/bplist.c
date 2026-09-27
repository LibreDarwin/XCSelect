/*
 * bplist.c -- read Apple's binary property lists (bplist00).
 *
 * Apple ships SDKSettings.plist and most other metadata in the binary
 * format rather than XML, so reading a real Xcode SDK means reading
 * bplist00.  The result is the same plist_node tree the XML and NextSTEP
 * parsers produce, so callers do not care which dialect a file was in.
 *
 * Layout, briefly: an 8-byte "bplist00" header, the objects, an offset
 * table, and a 32-byte trailer giving the sizes needed to walk both.
 * Each object starts with a marker byte whose high nibble is the type
 * and whose low nibble is either a small length or 0xf, meaning the
 * length follows as its own integer object.
 *
 * Scalars are rendered as strings, matching the other parsers: versions
 * and names are what callers read, and a uniform tree keeps
 * plist_dict_get() usable across all three formats.  UIDs and <data> are
 * not decoded, since nothing here reads binary blobs.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plist.h"

typedef struct {
	const uint8_t *data;
	size_t len;
	uint8_t offset_size;	/* bytes per entry in the offset table */
	uint8_t ref_size;	/* bytes per object reference */
	uint64_t num_objects;
	uint64_t offset_table;
} bplist;

static plist_node *read_object(const bplist *bp, uint64_t index, int depth);

static plist_node *
node_alloc(plist_type type)
{
	plist_node *n = calloc(1, sizeof(*n));

	if (n != NULL)
		n->type = type;
	return n;
}

static int
node_append(plist_node *parent, plist_node *child)
{
	if (parent->count == parent->cap) {
		size_t cap = parent->cap ? parent->cap * 2 : 8;
		plist_node **items = realloc(parent->items,
					     cap * sizeof(*items));

		if (items == NULL)
			return -1;
		parent->items = items;
		parent->cap = cap;
	}
	parent->items[parent->count++] = child;
	return 0;
}

/* Big-endian integer of the given width. */
static uint64_t
read_be(const uint8_t *p, size_t width)
{
	uint64_t value = 0;
	size_t i;

	for (i = 0; i < width; i++)
		value = (value << 8) | p[i];
	return value;
}

static int
in_bounds(const bplist *bp, uint64_t offset, uint64_t need)
{
	return (offset <= bp->len && need <= bp->len - offset);
}

/* Offset of object `index`, read out of the offset table. */
static uint64_t
object_offset(const bplist *bp, uint64_t index)
{
	uint64_t pos;

	if (index >= bp->num_objects)
		return bp->len;

	pos = bp->offset_table + index * bp->offset_size;
	if (!in_bounds(bp, pos, bp->offset_size))
		return bp->len;

	return read_be(bp->data + pos, bp->offset_size);
}

/*
 * Read the length that follows a marker whose low nibble is 0xf: it is
 * itself an integer object.  *pos is advanced past it.
 */
static uint64_t
read_long_length(const bplist *bp, uint64_t *pos)
{
	uint8_t marker;
	size_t width;

	if (!in_bounds(bp, *pos, 1))
		return 0;

	marker = bp->data[*pos];
	if ((marker & 0xf0) != 0x10)
		return 0;

	width = (size_t)1 << (marker & 0x0f);
	(*pos)++;
	if (!in_bounds(bp, *pos, width))
		return 0;

	{
		uint64_t value = read_be(bp->data + *pos, width);

		*pos += width;
		return value;
	}
}

static char *
make_string(const char *text, size_t len)
{
	char *s = malloc(len + 1);

	if (s == NULL)
		return NULL;
	memcpy(s, text, len);
	s[len] = '\0';
	return s;
}

/* UTF-16BE to UTF-8, enough for the ASCII-range keys these files use. */
static char *
utf16_to_utf8(const uint8_t *p, uint64_t units)
{
	char *out = malloc(units * 3 + 1);
	size_t o = 0;
	uint64_t i;

	if (out == NULL)
		return NULL;

	for (i = 0; i < units; i++) {
		uint32_t c = (uint32_t)read_be(p + i * 2, 2);

		if (c < 0x80) {
			out[o++] = (char)c;
		} else if (c < 0x800) {
			out[o++] = (char)(0xc0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3f));
		} else {
			out[o++] = (char)(0xe0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[o] = '\0';

	return out;
}

static plist_node *
scalar_node(char *text)
{
	plist_node *n;

	if (text == NULL)
		return NULL;
	if ((n = node_alloc(PLIST_STRING)) == NULL) {
		free(text);
		return NULL;
	}
	n->string = text;
	return n;
}

static plist_node *
read_object(const bplist *bp, uint64_t index, int depth)
{
	uint64_t pos = object_offset(bp, index);
	uint8_t marker, type, nibble;
	uint64_t count;

	/* Guards against a malformed file looping back on itself. */
	if (depth > 32 || !in_bounds(bp, pos, 1))
		return NULL;

	marker = bp->data[pos++];
	type = marker & 0xf0;
	nibble = marker & 0x0f;

	switch (type) {
	case 0x00:					/* singletons */
		if (nibble == 0x08)
			return scalar_node(strdup("false"));
		if (nibble == 0x09)
			return scalar_node(strdup("true"));
		return NULL;				/* null, fill */

	case 0x10: {					/* integer */
		size_t width = (size_t)1 << nibble;
		char buf[32];

		if (!in_bounds(bp, pos, width))
			return NULL;
		snprintf(buf, sizeof(buf), "%llu",
			 (unsigned long long)read_be(bp->data + pos, width));
		return scalar_node(strdup(buf));
	}

	case 0x20: {					/* real */
		size_t width = (size_t)1 << nibble;
		char buf[64];

		if (!in_bounds(bp, pos, width))
			return NULL;
		if (width == 4) {
			uint32_t bits = (uint32_t)read_be(bp->data + pos, 4);
			float f;

			memcpy(&f, &bits, sizeof(f));
			snprintf(buf, sizeof(buf), "%g", (double)f);
		} else if (width == 8) {
			uint64_t bits = read_be(bp->data + pos, 8);
			double d;

			memcpy(&d, &bits, sizeof(d));
			snprintf(buf, sizeof(buf), "%g", d);
		} else {
			return NULL;
		}
		return scalar_node(strdup(buf));
	}

	case 0x40:					/* data */
		return scalar_node(strdup(""));

	case 0x50: {					/* ASCII string */
		count = nibble;
		if (nibble == 0x0f)
			count = read_long_length(bp, &pos);
		if (!in_bounds(bp, pos, count))
			return NULL;
		return scalar_node(make_string((const char *)bp->data + pos,
					       (size_t)count));
	}

	case 0x60: {					/* UTF-16BE string */
		count = nibble;
		if (nibble == 0x0f)
			count = read_long_length(bp, &pos);
		if (!in_bounds(bp, pos, count * 2))
			return NULL;
		return scalar_node(utf16_to_utf8(bp->data + pos, count));
	}

	case 0x80:					/* UID */
		return scalar_node(strdup(""));

	case 0xa0:					/* array */
	case 0xc0: {					/* set */
		plist_node *array;
		uint64_t i;

		count = nibble;
		if (nibble == 0x0f)
			count = read_long_length(bp, &pos);
		if (!in_bounds(bp, pos, count * bp->ref_size))
			return NULL;
		if ((array = node_alloc(PLIST_ARRAY)) == NULL)
			return NULL;

		for (i = 0; i < count; i++) {
			uint64_t ref = read_be(bp->data + pos + i * bp->ref_size,
					       bp->ref_size);
			plist_node *item = read_object(bp, ref, depth + 1);

			if (item != NULL && node_append(array, item) != 0)
				plist_free(item);
		}
		return array;
	}

	case 0xd0: {					/* dictionary */
		plist_node *dict;
		uint64_t keys, values, i;

		count = nibble;
		if (nibble == 0x0f)
			count = read_long_length(bp, &pos);
		if (!in_bounds(bp, pos, count * bp->ref_size * 2))
			return NULL;
		if ((dict = node_alloc(PLIST_DICT)) == NULL)
			return NULL;

		keys = pos;
		values = pos + count * bp->ref_size;

		for (i = 0; i < count; i++) {
			uint64_t kref = read_be(bp->data + keys + i * bp->ref_size,
						bp->ref_size);
			uint64_t vref = read_be(bp->data + values + i * bp->ref_size,
						bp->ref_size);
			plist_node *k = read_object(bp, kref, depth + 1);
			plist_node *v = read_object(bp, vref, depth + 1);

			if (k == NULL || v == NULL) {
				plist_free(k);
				plist_free(v);
				continue;
			}
			v->key = k->string;
			k->string = NULL;
			plist_free(k);

			if (node_append(dict, v) != 0)
				plist_free(v);
		}
		return dict;
	}

	default:
		return NULL;
	}
}

plist_node *
plist_parse_binary(const char *text, size_t len)
{
	const uint8_t *data = (const uint8_t *)text;
	const uint8_t *trailer;
	bplist bp;

	/* Header plus the 32-byte trailer is the smallest possible file. */
	if (data == NULL || len < 40 || memcmp(data, "bplist00", 8) != 0)
		return NULL;

	trailer = data + len - 32;

	bp.data = data;
	bp.len = len;
	bp.offset_size = trailer[6];
	bp.ref_size = trailer[7];
	bp.num_objects = read_be(trailer + 8, 8);
	bp.offset_table = read_be(trailer + 24, 8);

	if (bp.offset_size == 0 || bp.offset_size > 8 ||
	    bp.ref_size == 0 || bp.ref_size > 8)
		return NULL;
	if (bp.num_objects == 0 ||
	    !in_bounds(&bp, bp.offset_table, bp.num_objects * bp.offset_size))
		return NULL;

	return read_object(&bp, read_be(trailer + 16, 8), 0);
}

int
plist_looks_like_binary(const char *text, size_t len)
{
	return (text != NULL && len >= 8 && memcmp(text, "bplist00", 8) == 0);
}
