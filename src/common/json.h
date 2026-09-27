/*
 * json.h -- a JSON document model.
 *
 * Shaped after plist.h next door, because the two are read the same way:
 * parse a whole document, then walk it by key.  The minimal extractor in
 * notarytool answers "what is the value of this key" against a flat
 * response and cannot describe a nested document; a string catalog is
 * nested several levels deep, so this builds the tree.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __JSON_H__
#define __JSON_H__

#include <stddef.h>

typedef enum {
	JSON_OBJECT,
	JSON_ARRAY,
	JSON_STRING,
	JSON_NUMBER,
	JSON_BOOL,
	JSON_NULL
} json_type;

typedef struct json_node {
	json_type type;
	/* key is meaningful for members of an object; it is the member's key. */
	char *key;
	/* string holds the text of a JSON_STRING, unescaped. */
	char *string;
	double number;
	int boolean;
	struct json_node **items;
	size_t count;
	size_t cap;
} json_node;

/*
 * Parse a JSON document.  Returns the root node, or NULL if the text is
 * not well-formed JSON.  Trailing content after the root value is an
 * error, so a truncated file is rejected rather than half-read.
 */
json_node *json_parse(const char *text, size_t len);

void json_free(json_node *node);

/* Member of an object by key, or NULL. */
json_node *json_object_get(const json_node *obj, const char *key);

/* Element of an array by index, or NULL. */
json_node *json_array_at(const json_node *array, size_t index);

/* Number of members or elements; 0 for scalars. */
size_t json_count(const json_node *node);

/* Text of a JSON_STRING node, or NULL for any other type. */
const char *json_string(const json_node *node);

#endif /* __JSON_H__ */
