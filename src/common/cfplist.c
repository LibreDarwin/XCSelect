/*
 * cfplist.c -- read property lists through CoreFoundation.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <CoreFoundation/CoreFoundation.h>

#include "cfplist.h"

/*
 * These files are small -- a few kilobytes at most -- so they are read in
 * one go.  CFPropertyList recognises the XML and binary dialects alike, so
 * the same call covers a config shipping either one.
 *
 * The caller owns the result and releases it with CFRelease().
 */
CFDictionaryRef
cfplist_read(const char *path)
{
	CFDictionaryRef dict;
	CFDataRef data;
	CFTypeRef plist;
	struct stat st;
	char *text;
	size_t got;
	FILE *fp;

	if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return NULL;
	if ((fp = fopen(path, "rb")) == NULL)
		return NULL;
	if ((text = malloc((size_t)st.st_size + 1)) == NULL) {
		fclose(fp);
		return NULL;
	}

	/*
	 * A short read is not an error: the length actually read is what
	 * goes to the parser, so a truncated file is rejected by the parse
	 * rather than by trusting the stat.
	 */
	got = fread(text, 1, (size_t)st.st_size, fp);
	fclose(fp);

	data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)text, (CFIndex)got);
	free(text);
	if (data == NULL)
		return NULL;

	plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist == NULL)
		return NULL;

	/*
	 * A property list may be rooted at an array or a string, and every
	 * caller wants a dictionary to look a key up in.  Refusing anything
	 * else here keeps the cast honest -- handing an array to
	 * CFDictionaryGetValue() raises instead of returning NULL.
	 */
	if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		return NULL;
	}
	dict = (CFDictionaryRef)plist;

	return dict;
}

/*
 * The value for one key, or NULL when it is absent.  The key arrives as
 * a C string rather than a CFString, and CFSTR() only accepts a literal --
 * it concatenates its argument into a string literal at compile time -- so
 * the lookup key is built here instead.
 */
CFTypeRef
cfplist_get(CFDictionaryRef dict, const char *key)
{
	CFStringRef cfkey;
	CFTypeRef value;

	if (dict == NULL || key == NULL)
		return NULL;
	if ((cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key,
	    kCFStringEncodingUTF8)) == NULL)
		return NULL;

	value = CFDictionaryGetValue(dict, cfkey);
	CFRelease(cfkey);

	return value;
}

/*
 * A strdup'd C string for one string member, or NULL when the key is
 * absent or holds something other than a string.
 */
char *
cfplist_string(CFDictionaryRef dict, const char *key)
{
	CFStringRef value;
	char buf[PATH_MAX];
	char *out;

	if ((value = (CFStringRef)cfplist_get(dict, key)) == NULL)
		return NULL;
	if (CFGetTypeID(value) != CFStringGetTypeID())
		return NULL;
	if (!CFStringGetCString(value, buf, (CFIndex)sizeof(buf),
	    kCFStringEncodingUTF8))
		return NULL;

	if ((out = strdup(buf)) == NULL)
		return NULL;

	return out;
}
