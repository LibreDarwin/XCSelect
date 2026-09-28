/*
 * cfplist.h -- read property lists through CoreFoundation.
 *
 * Every property list this project reads is a config file, not a
 * document: SDKSettings.plist, Info.plist, ToolchainInfo.plist,
 * SystemVersion.plist and an export options plist.  All of them are
 * dictionaries, and all of them may be written as XML or as bplist00 --
 * plutil will produce either from the same source.  CoreFoundation
 * parses both dialects, and rejects a malformed one, which is the whole
 * reason this exists rather than a hand-rolled scanner.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XCODE_TOOLS_CFPLIST_H_
#define _XCODE_TOOLS_CFPLIST_H_

#include <CoreFoundation/CoreFoundation.h>

/**
 * @func cfplist_read -- read a property list file as a dictionary
 * @arg path - file to read
 *
 * The file must be rooted at a dictionary.  A property list may be
 * rooted at an array or a string, and every caller here wants a
 * dictionary to look a key up in, so anything else is refused: handing
 * an array to CFDictionaryGetValue() raises rather than returning NULL.
 * NULL is therefore always "unreadable", never "read but not a dict".
 *
 * @return: the dictionary, which the caller owns and releases with
 *          CFRelease(), or NULL if the file is missing, unreadable,
 *          malformed, or not dictionary-rooted.
 */
CFDictionaryRef cfplist_read(const char *path);

/**
 * @func cfplist_get -- the value for one key
 * @arg dict - dictionary from cfplist_read()
 * @arg key - key to look up
 *
 * @return: the value, valid for as long as @arg dict is, or NULL when
 *          the key is absent.  Not retained; do not release it.
 */
CFTypeRef cfplist_get(CFDictionaryRef dict, const char *key);

/**
 * @func cfplist_string -- a strdup'd C string for one string member
 * @arg dict - dictionary from cfplist_read()
 * @arg key - key to look up
 *
 * @return: the value, which the caller owns and frees with free(), or
 *          NULL when the key is absent, holds a non-string, or is longer
 *          than PATH_MAX.
 */
char *cfplist_string(CFDictionaryRef dict, const char *key);

#endif /* _XCODE_TOOLS_CFPLIST_H_ */
