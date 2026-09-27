/*
 * xmlplist.c -- read Apple's XML property lists.
 *
 * The parser in plist.c reads the NextSTEP-style plists that .pbxproj
 * files use.  Apple's metadata -- SDKSettings.plist, ToolchainInfo.plist,
 * a platform's Info.plist -- is XML instead, so this reads that dialect
 * and produces the same plist_node tree, letting plist_dict_get() and
 * friends work on either.
 *
 * Only what those files actually contain is handled: dictionaries,
 * arrays, strings, integers, reals, booleans and dates.  Everything with
 * a scalar value becomes a PLIST_STRING, which is how callers want to
 * read versions and names anyway; <data> is skipped rather than
 * base64-decoded, since nothing here reads binary blobs.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "plist.h"

typedef struct {
	const char *text;
	size_t len;
	size_t pos;
} xml_reader;

static plist_node *parse_value(xml_reader *r);

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

static void
skip_space(xml_reader *r)
{
	while (r->pos < r->len && isspace((unsigned char)r->text[r->pos]))
		r->pos++;
}

/*
 * Advance past comments, the XML declaration and the DOCTYPE, none of
 * which carry values.
 */
static void
skip_noise(xml_reader *r)
{
	for (;;) {
		skip_space(r);
		if (r->pos + 4 <= r->len &&
		    strncmp(r->text + r->pos, "<!--", 4) == 0) {
			const char *end = strstr(r->text + r->pos, "-->");

			r->pos = end ? (size_t)(end - r->text) + 3 : r->len;
			continue;
		}
		if (r->pos + 2 <= r->len &&
		    (strncmp(r->text + r->pos, "<?", 2) == 0 ||
		     strncmp(r->text + r->pos, "<!", 2) == 0)) {
			const char *end = strchr(r->text + r->pos, '>');

			r->pos = end ? (size_t)(end - r->text) + 1 : r->len;
			continue;
		}
		return;
	}
}

/*
 * Read the next tag name.  Sets *closing when it is an end tag and
 * *selfclosing when it closes itself, as <true/> does.
 */
static char *
read_tag(xml_reader *r, int *closing, int *selfclosing)
{
	size_t start, end;
	char *name;

	*closing = 0;
	*selfclosing = 0;

	skip_noise(r);
	if (r->pos >= r->len || r->text[r->pos] != '<')
		return NULL;
	r->pos++;

	if (r->pos < r->len && r->text[r->pos] == '/') {
		*closing = 1;
		r->pos++;
	}

	start = r->pos;
	while (r->pos < r->len && r->text[r->pos] != '>')
		r->pos++;
	end = r->pos;
	if (r->pos < r->len)
		r->pos++;

	if (end > start && r->text[end - 1] == '/') {
		*selfclosing = 1;
		end--;
	}

	while (end > start && isspace((unsigned char)r->text[end - 1]))
		end--;

	if ((name = malloc(end - start + 1)) == NULL)
		return NULL;
	memcpy(name, r->text + start, end - start);
	name[end - start] = '\0';

	return name;
}

/* Replace the five predefined XML entities in place. */
static void
decode_entities(char *s)
{
	char *out = s;

	while (*s != '\0') {
		if (*s == '&') {
			if (strncmp(s, "&amp;", 5) == 0)      { *out++ = '&';  s += 5; continue; }
			if (strncmp(s, "&lt;", 4) == 0)       { *out++ = '<';  s += 4; continue; }
			if (strncmp(s, "&gt;", 4) == 0)       { *out++ = '>';  s += 4; continue; }
			if (strncmp(s, "&quot;", 6) == 0)     { *out++ = '"';  s += 6; continue; }
			if (strncmp(s, "&apos;", 6) == 0)     { *out++ = '\''; s += 6; continue; }
		}
		*out++ = *s++;
	}
	*out = '\0';
}

/* Text up to the next '<', with entities decoded. */
static char *
read_text(xml_reader *r)
{
	size_t start = r->pos;
	char *text;

	while (r->pos < r->len && r->text[r->pos] != '<')
		r->pos++;

	if ((text = malloc(r->pos - start + 1)) == NULL)
		return NULL;
	memcpy(text, r->text + start, r->pos - start);
	text[r->pos - start] = '\0';
	decode_entities(text);

	return text;
}

static void
skip_to_close(xml_reader *r)
{
	int closing, selfclosing;
	char *name = read_tag(r, &closing, &selfclosing);

	free(name);
}

static plist_node *
parse_dict(xml_reader *r)
{
	plist_node *dict = node_alloc(PLIST_DICT);

	if (dict == NULL)
		return NULL;

	for (;;) {
		int closing, selfclosing;
		char *tag, *key;
		plist_node *value;
		size_t save = r->pos;

		if ((tag = read_tag(r, &closing, &selfclosing)) == NULL)
			break;

		if (closing && strcmp(tag, "dict") == 0) {
			free(tag);
			break;
		}
		if (strcmp(tag, "key") != 0) {
			/* Not a well-formed dict member; stop rather than guess. */
			free(tag);
			r->pos = save;
			break;
		}
		free(tag);

		key = read_text(r);
		skip_to_close(r);	/* </key> */

		if ((value = parse_value(r)) == NULL) {
			free(key);
			break;
		}
		value->key = key;

		if (node_append(dict, value) != 0) {
			plist_free(value);
			break;
		}
	}

	return dict;
}

static plist_node *
parse_array(xml_reader *r)
{
	plist_node *array = node_alloc(PLIST_ARRAY);

	if (array == NULL)
		return NULL;

	for (;;) {
		size_t save = r->pos;
		int closing, selfclosing;
		char *tag = read_tag(r, &closing, &selfclosing);
		plist_node *value;

		if (tag == NULL)
			break;
		if (closing && strcmp(tag, "array") == 0) {
			free(tag);
			break;
		}
		free(tag);

		r->pos = save;
		if ((value = parse_value(r)) == NULL)
			break;
		if (node_append(array, value) != 0) {
			plist_free(value);
			break;
		}
	}

	return array;
}

/*
 * Parse whatever value comes next.  Scalars all collapse to
 * PLIST_STRING; booleans become "true"/"false" so callers can test them
 * without a separate type.
 */
static plist_node *
parse_value(xml_reader *r)
{
	int closing, selfclosing;
	char *tag = read_tag(r, &closing, &selfclosing);
	plist_node *node;

	if (tag == NULL)
		return NULL;
	if (closing) {
		free(tag);
		return NULL;
	}

	if (strcmp(tag, "dict") == 0) {
		free(tag);
		return selfclosing ? node_alloc(PLIST_DICT) : parse_dict(r);
	}
	if (strcmp(tag, "array") == 0) {
		free(tag);
		return selfclosing ? node_alloc(PLIST_ARRAY) : parse_array(r);
	}
	if (strcmp(tag, "true") == 0 || strcmp(tag, "false") == 0) {
		if ((node = node_alloc(PLIST_STRING)) != NULL)
			node->string = strdup(tag);
		if (!selfclosing)
			skip_to_close(r);
		free(tag);
		return node;
	}
	if (strcmp(tag, "string") == 0 || strcmp(tag, "integer") == 0 ||
	    strcmp(tag, "real") == 0 || strcmp(tag, "date") == 0 ||
	    strcmp(tag, "data") == 0) {
		int is_data = (strcmp(tag, "data") == 0);

		free(tag);
		if (selfclosing) {
			if ((node = node_alloc(PLIST_STRING)) != NULL)
				node->string = strdup("");
			return node;
		}
		if ((node = node_alloc(PLIST_STRING)) == NULL)
			return NULL;
		node->string = read_text(r);
		if (is_data) {
			/* Not decoded; nothing here reads binary blobs. */
			free(node->string);
			node->string = strdup("");
		}
		skip_to_close(r);
		return node;
	}

	/* Unknown element: skip its content and report nothing. */
	free(tag);
	if (!selfclosing)
		skip_to_close(r);
	return NULL;
}

plist_node *
plist_parse_xml(const char *text, size_t len)
{
	xml_reader r;
	plist_node *root = NULL;

	if (text == NULL)
		return NULL;

	r.text = text;
	r.len = len;
	r.pos = 0;

	/* Step over the declaration and DOCTYPE, then the <plist> wrapper. */
	for (;;) {
		size_t save;
		int closing, selfclosing;
		char *tag;

		skip_noise(&r);
		save = r.pos;
		if ((tag = read_tag(&r, &closing, &selfclosing)) == NULL)
			return NULL;

		if (strncmp(tag, "plist", 5) == 0 && !closing) {
			free(tag);
			continue;
		}

		free(tag);
		r.pos = save;
		root = parse_value(&r);
		break;
	}

	return root;
}

int
plist_looks_like_xml(const char *text, size_t len)
{
	size_t i;

	if (text == NULL)
		return 0;

	for (i = 0; i < len && i < 256; i++) {
		if (isspace((unsigned char)text[i]))
			continue;
		return (text[i] == '<');
	}

	return 0;
}

/*
 * Dispatch on content rather than on file name: Apple's metadata is
 * binary in a shipped SDK and XML in one we emit, and .pbxproj is
 * NextSTEP, all under names that do not distinguish them.
 */
plist_node *
plist_parse_any(const char *text, size_t len)
{
	if (plist_looks_like_binary(text, len))
		return plist_parse_binary(text, len);
	if (plist_looks_like_xml(text, len))
		return plist_parse_xml(text, len);
	return plist_parse(text, len);
}
