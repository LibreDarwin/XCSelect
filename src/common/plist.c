/* plist -- minimal NextSTEP-style plist parser
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "plist.h"

typedef struct {
	char *text;
	size_t len;
	size_t pos;
} tokenizer;

typedef enum {
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_LPAREN,
	TOK_RPAREN,
	TOK_EQUAL,
	TOK_SEMI,
	TOK_COMMA,
	TOK_WORD,
	TOK_EOF,
	TOK_ERROR
} token_type;

typedef struct {
	token_type type;
	char *word;
} token;

static char *xstrndup(const char *s, size_t n)
{
	char *p = (char *)malloc(n + 1);
	if (p == NULL)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static void skip_comments(tokenizer *tz)
{
	while (tz->pos < tz->len) {
		if (tz->text[tz->pos] == '/' && tz->pos + 1 < tz->len && tz->text[tz->pos + 1] == '/') {
			tz->pos += 2;
			while (tz->pos < tz->len && tz->text[tz->pos] != '\n')
				tz->pos++;
		} else if (tz->text[tz->pos] == '/' && tz->pos + 1 < tz->len && tz->text[tz->pos + 1] == '*') {
			tz->pos += 2;
			while (tz->pos + 1 < tz->len &&
			       !(tz->text[tz->pos] == '*' && tz->text[tz->pos + 1] == '/'))
				tz->pos++;
			if (tz->pos + 1 < tz->len)
				tz->pos += 2;
		} else if (isspace((unsigned char)tz->text[tz->pos])) {
			tz->pos++;
		} else {
			break;
		}
	}
}

static token next_token(tokenizer *tz)
{
	token t;
	t.type = TOK_EOF;
	t.word = NULL;

	skip_comments(tz);
	if (tz->pos >= tz->len)
		return t;

	char c = tz->text[tz->pos];
	if (c == '{') { tz->pos++; t.type = TOK_LBRACE; return t; }
	if (c == '}') { tz->pos++; t.type = TOK_RBRACE; return t; }
	if (c == '(') { tz->pos++; t.type = TOK_LPAREN; return t; }
	if (c == ')') { tz->pos++; t.type = TOK_RPAREN; return t; }
	if (c == '=') { tz->pos++; t.type = TOK_EQUAL; return t; }
	if (c == ';') { tz->pos++; t.type = TOK_SEMI; return t; }
	if (c == ',') { tz->pos++; t.type = TOK_COMMA; return t; }

	if (c == '"') {
		tz->pos++;
		size_t start = tz->pos;
		size_t out = start;
		while (tz->pos < tz->len && tz->text[tz->pos] != '"') {
			if (tz->text[tz->pos] == '\\' && tz->pos + 1 < tz->len) {
				char esc = tz->text[tz->pos + 1];
				if (esc == 'n') esc = '\n';
				else if (esc == 't') esc = '\t';
				else if (esc == 'r') esc = '\r';
				else if (esc == '"' || esc == '\\') ;
				else esc = tz->text[tz->pos + 1];
				tz->text[out++] = esc;
				tz->pos += 2;
			} else {
				tz->text[out++] = tz->text[tz->pos++];
			}
		}
		if (tz->pos >= tz->len) {
			t.type = TOK_ERROR;
			return t;
		}
		tz->pos++; /* skip closing quote */
		t.type = TOK_WORD;
		t.word = xstrndup(tz->text + start, out - start);
		return t;
	}

	size_t start = tz->pos;
	while (tz->pos < tz->len) {
		char ch = tz->text[tz->pos];
		if (isspace((unsigned char)ch) || ch == '{' || ch == '}' || ch == '(' ||
		    ch == ')' || ch == '=' || ch == ';' || ch == ',' || ch == '/')
			break;
		tz->pos++;
	}
	t.type = TOK_WORD;
	t.word = xstrndup(tz->text + start, tz->pos - start);
	return t;
}

static plist_node *parse_element(tokenizer *tz);

static plist_node *new_node(plist_type type)
{
	plist_node *n = (plist_node *)calloc(1, sizeof(plist_node));
	if (n == NULL)
		return NULL;
	n->type = type;
	return n;
}

static int node_append(plist_node *parent, plist_node *child)
{
	if (parent->count == parent->cap) {
		size_t newcap = parent->cap ? parent->cap * 2 : 8;
		plist_node **items = (plist_node **)realloc(parent->items, sizeof(plist_node *) * newcap);
		if (items == NULL)
			return -1;
		parent->items = items;
		parent->cap = newcap;
	}
	parent->items[parent->count++] = child;
	return 0;
}

static plist_node *parse_dict(tokenizer *tz)
{
	plist_node *n = new_node(PLIST_DICT);
	if (n == NULL)
		return NULL;
	for (;;) {
		token key = next_token(tz);
		if (key.type == TOK_RBRACE || key.type == TOK_EOF)
			return n;
		if (key.type != TOK_WORD)
			return n;
		token eq = next_token(tz);
		if (eq.type != TOK_EQUAL)
			return n;
		plist_node *val = parse_element(tz);
		if (val == NULL) {
			free(key.word);
			return n;
		}
		val->key = key.word;
		if (node_append(n, val) != 0)
			return n;
		token term = next_token(tz);
		if (term.type == TOK_RBRACE)
			return n;
		if (term.type == TOK_SEMI)
			continue;
		if (term.type == TOK_COMMA)
			continue;
		/* unexpected token; stop here */
		return n;
	}
}

static plist_node *parse_array(tokenizer *tz)
{
	plist_node *n = new_node(PLIST_ARRAY);
	if (n == NULL)
		return NULL;
	for (;;) {
		token t = next_token(tz);
		if (t.type == TOK_RPAREN || t.type == TOK_EOF)
			return n;
		plist_node *val = NULL;
		if (t.type == TOK_LBRACE) {
			tz->pos -= 1; /* back up to '{' */
			val = parse_element(tz);
		} else if (t.type == TOK_LPAREN) {
			tz->pos -= 1;
			val = parse_element(tz);
		} else if (t.type == TOK_WORD) {
			val = new_node(PLIST_STRING);
			if (val != NULL)
				val->string = t.word;
		} else {
			return n;
		}
		if (val == NULL)
			return n;
		if (node_append(n, val) != 0)
			return n;
		token sep = next_token(tz);
		if (sep.type == TOK_RPAREN || sep.type == TOK_EOF)
			return n;
		if (sep.type == TOK_SEMI) {
			token again = next_token(tz);
			if (again.type == TOK_RPAREN || again.type == TOK_EOF)
				return n;
			continue;
		}
		if (sep.type == TOK_COMMA)
			continue;
		return n;
	}
}

static plist_node *parse_element(tokenizer *tz)
{
	token t = next_token(tz);
	plist_node *n;
	if (t.type == TOK_LBRACE)
		n = parse_dict(tz);
	else if (t.type == TOK_LPAREN)
		n = parse_array(tz);
	else if (t.type == TOK_WORD) {
		n = new_node(PLIST_STRING);
		if (n != NULL)
			n->string = t.word;
	} else {
		n = NULL;
	}
	return n;
}

static void free_node(plist_node *n)
{
	if (n == NULL)
		return;
	for (size_t i = 0; i < n->count; i++)
		free_node(n->items[i]);
	free(n->key);
	if (n->type == PLIST_STRING)
		free(n->string);
	free(n->items);
	free(n);
}

plist_node *plist_parse(const char *text, size_t len)
{
	if (text == NULL)
		return NULL;

	char *buf = xstrndup(text, len);
	if (buf == NULL)
		return NULL;

	tokenizer tz;
	tz.text = buf;
	tz.len = len;
	tz.pos = 0;

	plist_node *root = parse_element(&tz);

	plist_node *result = root;
	if (result != NULL && root->type == PLIST_DICT) {
		/* swallow any trailing tokens; root is the whole document */
	}
	free(buf);
	return result;
}

void plist_free(plist_node *node)
{
	free_node(node);
}

plist_node *plist_dict_get(const plist_node *dict, const char *key)
{
	if (dict == NULL || dict->type != PLIST_DICT || key == NULL)
		return NULL;
	for (size_t i = 0; i < dict->count; i++) {
		plist_node *child = dict->items[i];
		if (child->key != NULL && strcmp(child->key, key) == 0)
			return child;
	}
	return NULL;
}

plist_node *plist_array_at(const plist_node *array, size_t index)
{
	if (array == NULL || array->type != PLIST_ARRAY || index >= array->count)
		return NULL;
	return array->items[index];
}

size_t plist_count(const plist_node *node)
{
	if (node == NULL)
		return 0;
	return node->count;
}

char *plist_join_strings(const plist_node *array, const char *sep)
{
	if (array == NULL || array->type != PLIST_ARRAY || array->count == 0)
		return NULL;
	size_t seplen = strlen(sep);
	size_t total = 0;
	for (size_t i = 0; i < array->count; i++) {
		plist_node *item = plist_array_at(array, i);
		total += (item->string ? strlen(item->string) : 0);
		if (i + 1 < array->count)
			total += seplen;
	}
	char *out = (char *)malloc(total + 1);
	if (out == NULL)
		return NULL;
	out[0] = '\0';
	for (size_t i = 0; i < array->count; i++) {
		plist_node *item = plist_array_at(array, i);
		if (item->string)
			strcat(out, item->string);
		if (i + 1 < array->count)
			strcat(out, sep);
	}
	return out;
}
