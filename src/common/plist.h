/* plist -- minimal NextSTEP-style plist parser
 *
 * A small, dependency-free parser for the ASCII / NextSTEP plist format used
 * by Xcode project files (e.g. project.pbxproj). It exposes a typed tree that
 * is sufficient for listing targets, build configurations, schemes and reading
 * the buildSettings dictionary of an XCBuildConfiguration entry.
 *
 * All memory used by a parsed tree is owned by a single arena; call
 * plist_free() once on the root node to release everything.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __PLIST_H__
#define __PLIST_H__

#include <stddef.h>

typedef enum {
	PLIST_DICT,
	PLIST_ARRAY,
	PLIST_STRING
} plist_type;

typedef struct plist_node {
	plist_type type;
	/* key is meaningful for members of a dict; it is the member's key text. */
	char *key;
	/* string holds the text of a PLIST_STRING value. */
	char *string;
	struct plist_node **items;
	size_t count;
	size_t cap;
	struct plist_node *root;
} plist_node;

/* Parse a NextSTEP plist blob. Returns a root node (a PLIST_DICT for valid
 * Xcode project plists) or NULL on failure. */
plist_node *plist_parse(const char *text, size_t len);

/* Parse an Apple XML property list -- SDKSettings.plist and friends --
 * into the same node tree, so plist_dict_get() works on either dialect.
 * Implemented in xmlplist.c. */
plist_node *plist_parse_xml(const char *text, size_t len);

/* True when a buffer looks like XML rather than a NextSTEP plist. */
int plist_looks_like_xml(const char *text, size_t len);

/* Parse an Apple binary property list (bplist00), which is what a real
 * Xcode SDK ships, into the same node tree.  Implemented in bplist.c. */
plist_node *plist_parse_binary(const char *text, size_t len);

/* True when a buffer carries the bplist00 magic. */
int plist_looks_like_binary(const char *text, size_t len);

/* Parse whichever of the three dialects the buffer holds. */
plist_node *plist_parse_any(const char *text, size_t len);

/* Release every allocation owned by a parsed plist tree. */
void plist_free(plist_node *node);

/* Return the value member for `key` in a dict, or NULL if absent/not a dict. */
plist_node *plist_dict_get(const plist_node *dict, const char *key);

/* Return the i-th element of an array, or NULL. */
plist_node *plist_array_at(const plist_node *array, size_t index);

/* Return the number of items in a dict or array, or 0 for strings. */
size_t plist_count(const plist_node *node);

/* Return a strdup'd concatenation of an array of string nodes joined by sep,
 * or NULL if the node is not an array. Caller frees the result. */
char *plist_join_strings(const plist_node *array, const char *sep);

#endif /* __PLIST_H__ */
