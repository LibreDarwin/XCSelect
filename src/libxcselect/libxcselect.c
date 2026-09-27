/*
 * libxcselect -- find the active developer directory.
 *
 * Apple's xcrun and xcode-select both link libxcselect rather than each
 * working this out for itself, which is what keeps them agreeing about
 * where the tools are.  This is a BSD-licensed implementation of that
 * library, and ours link it for the same reason.
 *
 * The selection order is the one the tools behave by:
 *
 *	DEVELOPER_DIR from the environment, if set
 *	the link at /var/db/xcode_select_link, which xcode-select -s writes
 *	an Xcode in /Applications
 *	a Command Line Tools install
 *
 * A directory named here is reported even when it no longer exists --
 * that is what is_invalid is for.  Callers distinguish "nothing has been
 * selected" from "what was selected is gone", and answering both with a
 * bare failure would lose that.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xcselect.h"

#define XCSELECT_LINK		"/var/db/xcode_select_link"
#define XCODE_DEFAULT		"/Applications/Xcode.app/Contents/Developer"
#define CLTOOLS_DEFAULT		"/Library/Developer/CommandLineTools"

#define XCSELECT_VERSION	1

static bool
is_dir(const char *path)
{
	struct stat st;

	return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * A Command Line Tools install has no Xcode bundle around it; an Xcode
 * developer directory sits inside one.  The name is what tells them
 * apart, since both hold usr/bin.
 */
static bool
looks_like_cltools(const char *path)
{
	return path != NULL && strstr(path, "CommandLineTools") != NULL;
}

/*
 * The Developer directory this binary is installed in, if any: the
 * nearest directory above it with a usr/bin/xcrun.  Not a fixed number of
 * components -- xcrun is in <developer_dir>/usr/bin, but m4 and yacc are in
 * <developer_dir>/Toolchains/XcodeDefault.xctoolchain/usr/bin, and taking
 * three off those names the toolchain, where there is no xcrun to run.
 * The root is never an answer: every Mac has a /usr/bin/xcrun.
 */
static bool
self_developer_dir(char *buf, size_t buf_size)
{
	char raw[PATH_MAX], real[PATH_MAX], xcrun[PATH_MAX];
	uint32_t size = sizeof(raw);
	char *slash;

	if (_NSGetExecutablePath(raw, &size) != 0)
		return false;
	if (realpath(raw, real) == NULL)
		return false;

	while ((slash = strrchr(real, '/')) != NULL && slash != real) {
		*slash = '\0';
		if (snprintf(xcrun, sizeof(xcrun), "%s/usr/bin/xcrun", real) <
		    (int)sizeof(xcrun) && access(xcrun, X_OK) == 0)
			return strlcpy(buf, real, buf_size) < buf_size;
	}

	return false;
}

/*
 * Developer contents of an install.  Accepts either the bundle
 * (/Applications/Xcode.app) or the developer directory inside it, so a
 * caller may pass whichever it has.
 */
bool
xcselect_find_developer_contents_from_path(const char *path, char *buf,
    size_t buf_size, bool *is_command_line_tools)
{
	char candidate[PATH_MAX];

	if (path == NULL || buf == NULL || buf_size == 0)
		return false;

	if (is_command_line_tools != NULL)
		*is_command_line_tools = false;

	/* An Xcode bundle: the developer directory is inside it. */
	if (snprintf(candidate, sizeof(candidate), "%s/Contents/Developer",
	    path) < (int)sizeof(candidate) && is_dir(candidate)) {
		if (strlcpy(buf, candidate, buf_size) >= buf_size)
			return false;
		return true;
	}

	/* Already a developer directory. */
	if (is_dir(path)) {
		if (strlcpy(buf, path, buf_size) >= buf_size)
			return false;
		if (is_command_line_tools != NULL)
			*is_command_line_tools = looks_like_cltools(path);
		return true;
	}

	return false;
}

bool
xcselect_get_developer_dir_path(char *buf, size_t buf_size,
    bool *is_command_line_tools, bool *is_missing, bool *is_invalid)
{
	char link[PATH_MAX];
	const char *env;
	ssize_t n;

	if (buf == NULL || buf_size == 0)
		return false;

	if (is_command_line_tools != NULL)
		*is_command_line_tools = false;
	if (is_missing != NULL)
		*is_missing = false;
	if (is_invalid != NULL)
		*is_invalid = false;

	/*
	 * The environment wins, and is taken as given: it is how a build
	 * selects a developer directory for one invocation, and
	 * second-guessing it would defeat that.
	 */
	if ((env = getenv("DEVELOPER_DIR")) != NULL && *env != '\0') {
		if (strlcpy(buf, env, buf_size) >= buf_size)
			return false;
		if (is_command_line_tools != NULL)
			*is_command_line_tools = looks_like_cltools(buf);
		if (is_invalid != NULL)
			*is_invalid = !is_dir(buf);
		return true;
	}

	/* What xcode-select -s wrote. */
	if ((n = readlink(XCSELECT_LINK, link, sizeof(link) - 1)) > 0) {
		link[n] = '\0';
		if (strlcpy(buf, link, buf_size) >= buf_size)
			return false;
		if (is_command_line_tools != NULL)
			*is_command_line_tools = looks_like_cltools(buf);
		if (is_invalid != NULL)
			*is_invalid = !is_dir(buf);
		return true;
	}

	/*
	 * Nothing selected.  A tool inside a Developer directory answers
	 * with its own, so a release tree that has been built or moved
	 * works with no configuration at all -- Apple's copies live in
	 * /usr/bin and never have this to fall back on.
	 */
	if (self_developer_dir(link, sizeof(link))) {
		if (strlcpy(buf, link, buf_size) >= buf_size)
			return false;
		if (is_command_line_tools != NULL)
			*is_command_line_tools = looks_like_cltools(buf);
		return true;
	}

	if (is_dir(XCODE_DEFAULT)) {
		if (strlcpy(buf, XCODE_DEFAULT, buf_size) >= buf_size)
			return false;
		return true;
	}

	if (is_dir(CLTOOLS_DEFAULT)) {
		if (strlcpy(buf, CLTOOLS_DEFAULT, buf_size) >= buf_size)
			return false;
		if (is_command_line_tools != NULL)
			*is_command_line_tools = true;
		return true;
	}

	if (is_missing != NULL)
		*is_missing = true;
	buf[0] = '\0';
	return false;
}

bool
xcselect_developer_dir_matches_path(const char *path)
{
	char devdir[PATH_MAX], resolved[PATH_MAX], want[PATH_MAX];

	if (path == NULL)
		return false;

	if (!xcselect_get_developer_dir_path(devdir, sizeof(devdir), NULL, NULL,
	    NULL))
		return false;

	/*
	 * Compared after resolution, so a symlinked or differently spelled
	 * path still matches the directory it names.
	 */
	if (realpath(devdir, resolved) == NULL || realpath(path, want) == NULL)
		return strcmp(devdir, path) == 0;

	return strcmp(resolved, want) == 0;
}

int
xcselect_get_version(void)
{
	return XCSELECT_VERSION;
}

bool
xcselect_host_sdk_path(char *buf, size_t buf_size)
{
	char devdir[PATH_MAX], candidate[PATH_MAX];

	if (buf == NULL || buf_size == 0)
		return false;

	if (!xcselect_get_developer_dir_path(devdir, sizeof(devdir), NULL, NULL,
	    NULL))
		return false;

	/* The macOS SDK, by the name the platform bundle gives it. */
	if (snprintf(candidate, sizeof(candidate),
	    "%s/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk", devdir) >=
	    (int)sizeof(candidate))
		return false;

	if (!is_dir(candidate)) {
		/* A Command Line Tools install keeps its SDKs flat. */
		if (snprintf(candidate, sizeof(candidate), "%s/SDKs/MacOSX.sdk",
		    devdir) >= (int)sizeof(candidate) || !is_dir(candidate))
			return false;
	}

	return strlcpy(buf, candidate, buf_size) < buf_size;
}

int
xcselect_invoke_xcrun(const char *tool, int argc, char **argv, int flags)
{
	char devdir[PATH_MAX], path[PATH_MAX];
	char **args;
	int i;

	(void)flags;

	if (tool == NULL)
		tool = (argc > 0 && argv != NULL) ? argv[0] : NULL;
	if (tool == NULL)
		return -1;

	if (!xcselect_get_developer_dir_path(devdir, sizeof(devdir), NULL, NULL,
	    NULL))
		return -1;

	if (snprintf(path, sizeof(path), "%s/usr/bin/xcrun", devdir) >=
	    (int)sizeof(path))
		return -1;

	/* xcrun, the tool, then the tool's own arguments. */
	if ((args = calloc((size_t)argc + 3, sizeof(*args))) == NULL)
		return -1;

	args[0] = (char *)"xcrun";
	args[1] = (char *)tool;
	for (i = 0; i < argc; i++)
		args[i + 2] = argv[i];

	execv(path, args);

	free(args);
	return -1;
}

bool
xcselect_bundle_is_developer_tool(const char *path)
{
	char devdir[PATH_MAX];

	if (path == NULL)
		return false;

	if (!xcselect_get_developer_dir_path(devdir, sizeof(devdir), NULL, NULL,
	    NULL))
		return false;

	return strncmp(path, devdir, strlen(devdir)) == 0;
}

void
xcselect_trigger_install_request(void)
{
	/*
	 * Apple's puts up the "install the developer tools" prompt through
	 * a system service.  Nothing here should install anything on a
	 * user's behalf, so this reports rather than acts, and callers see
	 * the same "no developer directory" answer they would otherwise.
	 */
}

/* ---- man paths ---------------------------------------------------- */

struct xcselect_manpaths {
	char **paths;
	size_t count;
};

static void
manpaths_add(xcselect_manpaths *mp, const char *path)
{
	char **grown;

	if (!is_dir(path))
		return;

	if ((grown = realloc(mp->paths, (mp->count + 1) * sizeof(*grown))) == NULL)
		return;

	mp->paths = grown;
	if ((mp->paths[mp->count] = strdup(path)) != NULL)
		mp->count++;
}

xcselect_manpaths *
xcselect_get_manpaths(const char *developer_dir)
{
	xcselect_manpaths *mp;
	char devdir[PATH_MAX], buf[PATH_MAX];

	if ((mp = calloc(1, sizeof(*mp))) == NULL)
		return NULL;

	if (developer_dir == NULL) {
		if (!xcselect_get_developer_dir_path(devdir, sizeof(devdir),
		    NULL, NULL, NULL))
			return mp;
		developer_dir = devdir;
	}

	snprintf(buf, sizeof(buf), "%s/usr/share/man", developer_dir);
	manpaths_add(mp, buf);
	snprintf(buf, sizeof(buf),
	    "%s/Toolchains/XcodeDefault.xctoolchain/usr/share/man",
	    developer_dir);
	manpaths_add(mp, buf);

	return mp;
}

size_t
xcselect_manpaths_get_num_paths(const xcselect_manpaths *paths)
{
	return (paths != NULL) ? paths->count : 0;
}

const char *
xcselect_manpaths_get_path(const xcselect_manpaths *paths, size_t index)
{
	if (paths == NULL || index >= paths->count)
		return NULL;

	return paths->paths[index];
}

void
xcselect_manpaths_free(xcselect_manpaths *paths)
{
	size_t i;

	if (paths == NULL)
		return;

	for (i = 0; i < paths->count; i++)
		free(paths->paths[i]);

	free(paths->paths);
	free(paths);
}
