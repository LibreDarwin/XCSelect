/*
 * xcselect.h -- the interface Apple's /usr/lib/libxcselect.dylib exports.
 *
 * xcrun and xcode-select both link libxcselect: it is what finds the
 * active developer directory, so the two agree on where the tools are.
 * Apple ships no public header for it, so this is reconstructed from
 * three sources, checked against each other:
 *
 *	xcselect_private.h in MacOSX.Internal.sdk, which is Apple's own
 *	    declaration of eleven of the twelve
 *	the .tbd in the dyld shared cache, for the export list
 *	/usr/lib/libxcselect.dylib itself, for the behaviour
 *
 * Where they disagree the library wins, and where the library was
 * ambiguous the private header's parameter names are used, since those
 * are Apple's and they say what each out-parameter is for.
 *
 * The private header omits one of the twelve, xcselect_host_sdk_path,
 * which the .tbd shows is exported.  Its signature is read off the
 * instruction sequence: a selector and an out-pointer, returning 0,
 * ENOENT or EINVAL.  The selector's three values are named here from
 * what each branch does; see the implementation.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __XCSELECT_H__
#define __XCSELECT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The version this library reports.  Matched to the Internal SDK the
 * project builds against, so that a caller comparing against the SDK's
 * own XCSELECT_VER agrees with us.  A shipped OS reports the number its
 * own library was built with, which runs ahead of the SDK.
 */
/*
 * What xcselect_get_version reports.  This is the build of the library
 * itself, not of any toolchain, and is what a caller compares against to
 * decide whether the library it loaded is the one it expected.  The
 * shipped library is at 2416; note that the private header in the
 * internal SDK still declares 2384, so the two disagree.
 */
#define XCSELECT_VER "2416"

/*
 * The active developer directory, and where the answer came from.
 *
 * was_environment is set when DEVELOPER_DIR decided it, was_cltools when
 * the answer is a Command Line Tools install, and was_default when
 * nothing was selected and one of the built-in locations was used.  A
 * caller printing the answer to a user wants all three: the same path
 * means different things depending on how it was arrived at.
 *
 * Returns true when buffer was filled.
 */
bool xcselect_get_developer_dir_path(char *buffer, int buffer_size,
    bool *was_environment, bool *was_cltools, bool *was_default);

/*
 * Whether path is at or inside a developer directory.
 *
 * developer_dir may be NULL to ask about the active one, which is what a
 * caller that has nothing else to go on wants; a caller comparing
 * against some other install names it and does not pay for the lookup.
 */
bool xcselect_developer_dir_matches_path(const char *developer_dir,
    const char *path);

/*
 * Given a path to an Xcode or Command Line Tools install -- a bundle, a
 * developer directory, a volume, or the root of one -- write the
 * developer directory it names.  Sets was_cltools on the paths where
 * that can be true.
 */
bool xcselect_find_developer_contents_from_path(char *path, char *buffer,
    size_t buffer_size, bool *was_cltools);

/* The version of this library, as a string. */
char *xcselect_get_version(void);

/*
 * Which SDK to answer with.  A caller building for the machine it is
 * running on wants the match; one shipping wants the newest it can.
 */
enum {
	/* The SDK matching this system, or the newest, whichever exists. */
	XCSELECT_HOST_SDK_PATH_MATCH_OR_NEWEST = 1,
	/* Only the SDK matching this system; NULL if there is none. */
	XCSELECT_HOST_SDK_PATH_MATCH = 2,
	/* Only the newest SDK. */
	XCSELECT_HOST_SDK_PATH_NEWEST = 3,
};

/*
 * Write the path of an SDK of the active developer directory to *out.
 * Returns 0, ENOENT when there is no such SDK, or EINVAL for an
 * unrecognised selector.
 */
int xcselect_host_sdk_path(int which, char **out);

/*
 * Run a tool through xcrun.  argc and argv are the tool's own, without
 * the xcrun argument; tool_name may be NULL, which asks for the tool
 * named by argv[0].  require_xcode rejects a Command Line Tools install
 * rather than running against it.  Does not return on success.
 */
void xcselect_invoke_xcrun(char *tool_name, int argc, char *argv[],
    bool require_xcode);

/* Whether a bundle identifier belongs to a developer tool. */
bool xcselect_bundle_is_developer_tool(char *bundle_id);

/* Ask the system to offer the Command Line Tools.  True if it was asked. */
bool xcselect_trigger_install_request(const char *tool_name);

/*
 * Manual page directories, as an opaque handle the caller walks and then
 * frees.  The layout is Apple's, so a caller compiled against their
 * header and linked against ours agrees.
 */
typedef struct _xcselect_manpaths {
	char **paths;
	uint32_t count;
} xcselect_manpaths;

xcselect_manpaths *xcselect_get_manpaths(char *sysroot);
uint32_t xcselect_manpaths_get_num_paths(xcselect_manpaths *xcp);
const char *xcselect_manpaths_get_path(xcselect_manpaths *xcp, uint32_t id);
void xcselect_manpaths_free(xcselect_manpaths *xcp);

#ifdef __cplusplus
}
#endif

#endif /* __XCSELECT_H__ */
