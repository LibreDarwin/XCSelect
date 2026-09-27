/*
 * xcselect.h -- the interface Apple's /usr/lib/libxcselect.dylib exports.
 *
 * xcrun and xcode-select both link libxcselect: it is what finds the
 * active developer directory, so the two agree on where the tools are.
 * Apple ships no header for it, so this is written from the library's
 * export list and from how their binaries call it.
 *
 * Two signatures are read directly off the call sites and are certain:
 *
 *	xcselect_get_developer_dir_path -- xcode-select passes a buffer,
 *	    0x400, and three separate one-byte out-params
 *	xcselect_find_developer_contents_from_path -- a path, a buffer,
 *	    0x400, and one one-byte out-param
 *	xcselect_invoke_xcrun -- a tool name (or NULL), argc - 1,
 *	    argv + 1, and a zero flag word
 *
 * The rest are shaped from their names and their use, and are marked
 * below where that is the case.  Anything linking this gets our
 * implementation, not Apple's; the point is that our xcrun and
 * xcode-select resolve the developer directory through one library, as
 * theirs do, rather than each working it out again.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __XCSELECT_H__
#define __XCSELECT_H__

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The active developer directory.
 *
 * Sets is_command_line_tools when the answer is a Command Line Tools
 * install rather than an Xcode; is_missing when nothing is selected;
 * is_invalid when what is selected does not exist any more.  Returns
 * true when buf was filled.
 */
bool xcselect_get_developer_dir_path(char *buf, size_t buf_size,
    bool *is_command_line_tools, bool *is_missing, bool *is_invalid);

/* Whether path is, or contains, the active developer directory. */
bool xcselect_developer_dir_matches_path(const char *path);

/*
 * Given a path to an Xcode or Command Line Tools install, write the
 * developer directory inside it.
 */
bool xcselect_find_developer_contents_from_path(const char *path,
    char *buf, size_t buf_size, bool *is_command_line_tools);

/* Library version, for callers that check before using newer calls. */
int xcselect_get_version(void);

/* The SDK matching the running system.  Shape inferred. */
bool xcselect_host_sdk_path(char *buf, size_t buf_size);

/*
 * Run a tool through xcrun.  argc and argv are the tool's own, without
 * the xcrun argument; tool may be NULL to take argv[0].  Does not
 * return on success.
 */
int xcselect_invoke_xcrun(const char *tool, int argc, char **argv, int flags);

/* Whether a bundle is a developer tool.  Shape inferred. */
bool xcselect_bundle_is_developer_tool(const char *path);

/* Ask the system to install the developer tools.  Shape inferred. */
void xcselect_trigger_install_request(void);

/*
 * Manual page directories for the active developer directory, as an
 * opaque handle the caller walks and then frees.
 */
typedef struct xcselect_manpaths xcselect_manpaths;

xcselect_manpaths *xcselect_get_manpaths(const char *developer_dir);
size_t xcselect_manpaths_get_num_paths(const xcselect_manpaths *paths);
const char *xcselect_manpaths_get_path(const xcselect_manpaths *paths,
    size_t index);
void xcselect_manpaths_free(xcselect_manpaths *paths);

#ifdef __cplusplus
}
#endif

#endif /* __XCSELECT_H__ */
