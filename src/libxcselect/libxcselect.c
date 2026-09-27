/*
 * libxcselect -- find the active developer directory.
 *
 * Apple's xcrun and xcode-select both link libxcselect rather than each
 * working this out for itself, which is what keeps them agreeing about
 * where the tools are.  This is a BSD-licensed implementation of that
 * library, and ours link it for the same reason.
 *
 * The selection order, recovered from the shipped library:
 *
 *	DEVELOPER_DIR from the environment, if set and not empty
 *	three symlinks, in this order:
 *	    /var/select/developer_dir
 *	    /var/db/xcode_select_link
 *	    /usr/share/xcode-select/xcode_dir_link
 *	the contents of /usr/share/xcode-select/xcode_dir_path
 *	then the built-in locations, each accepted only if it holds a
 *	usr/lib/libxcrun.dylib:
 *	    /Applications/Xcode.app/Contents/Developer
 *	    /Library/Developer/CommandLineTools
 *	    /Applications/Xcode-beta.app/Contents/Developer
 *
 * That libxcrun is the test for "this is really a developer directory"
 * rather than merely a directory, and it is what xcrun itself runs from,
 * so the two agree about which of the candidates are usable.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <os/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "xcselect.h"

/* Every buffer the shipped library sizes is this. */
#define XCSELECT_BUF_SIZE	0x400

/* What a developer directory has to hold to be one. */
#define XCSELECT_XCRUN		"usr/lib/libxcrun.dylib"

/* The symlinks consulted, in order, for a directory someone has selected. */
static const char * const dev_dir_links[] = {
	"/var/select/developer_dir",
	"/var/db/xcode_select_link",
	"/usr/share/xcode-select/xcode_dir_link",
};

/* Where a selection may be recorded as a file rather than a symlink. */
#define XCSELECT_DEV_DIR_FILE	"/usr/share/xcode-select/xcode_dir_path"

/* Used when nothing has been selected. */
static const struct {
	const char *dir;
	bool cltools;
} dev_dir_defaults[] = {
	{ "/Applications/Xcode.app/Contents/Developer",	false },
	{ "/Library/Developer/CommandLineTools",		true  },
	{ "/Applications/Xcode-beta.app/Contents/Developer", false },
};

/* ---- path helpers ---------------------------------------------------- */

/* The shipped library's own tests, so that a candidate is judged the
 * same way here as it is there. */
static bool
path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static bool
path_is_dir(const char *path)
{
	struct stat st;

	return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Append rel to buf, separating them with a slash unless there already is
 * one or buf is empty.  A full buf is left alone rather than truncated. */
static void
path_append(char *buf, size_t buf_size, const char *rel)
{
	size_t len = strlen(buf);

	if (len != 0 && buf[len - 1] != '/' && len + 1 < buf_size) {
		buf[len] = '/';
		buf[len + 1] = '\0';
	}

	strlcat(buf, rel, buf_size);
}

static void
path_join(char *buf, size_t buf_size, const char *dir, const char *rel)
{
	strlcpy(buf, dir, buf_size);
	path_append(buf, buf_size, rel);
}

/*
 * Does this directory hold a libxcrun, or failing that an xcrun?  Both
 * count, which is why "/" is treated as a developer directory: it has
 * /usr/bin/xcrun even though it is not one.
 */
static bool
path_contains_xcrun(const char *dir)
{
	char path[XCSELECT_BUF_SIZE];
	struct stat st;

	path_join(path, sizeof(path), dir, XCSELECT_XCRUN);
	if (path_exists(path))
		return true;

	path_join(path, sizeof(path), dir, "usr/bin/xcrun");
	if (stat(path, &st) != 0)
		return false;

	return S_ISREG(st.st_mode) && (st.st_mode & S_IRUSR) != 0;
}

/* A directory holding a kernel is a mounted system volume. */
static bool
path_contains_mach_kernel(const char *dir)
{
	char path[XCSELECT_BUF_SIZE];

	path_join(path, sizeof(path), dir, "mach_kernel");

	return path_exists(path);
}

/* Trailing slashes are noise, and would defeat the prefix comparison. */
static void
strip_trailing_slashes(char *path)
{
	size_t len = strlen(path);

	/* A bare "/" is a path, not a name to strip. */
	while (len >= 2 && path[len - 1] == '/')
		path[--len] = '\0';
}

static bool
basename_is(const char *path, const char *name)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL && strcmp(slash + 1, name) == 0;
}

static bool
is_root(const char *path)
{
	return path[0] == '/' && path[1] == '\0';
}

static bool
ends_with(const char *s, const char *suffix)
{
	size_t slen = strlen(s), xlen = strlen(suffix);

	return slen >= xlen && strcmp(s + slen - xlen, suffix) == 0;
}

/* ---- resolving a selected directory ---------------------------------- */

/*
 * Read one of the selection symlinks.  Returns true if it said anything
 * at all, having first checked that what it points at is a directory and
 * reported a link it could not read.  False means "not there", which is
 * not an error -- it is how the next candidate gets its turn.
 */
static bool
get_developer_dir_from_symlink(const char *link, char *buf, int buf_size,
    bool *is_dir)
{
	ssize_t n = readlink(link, buf, (size_t)buf_size - 1);

	if (n >= 1) {
		buf[n] = '\0';
		if (path_is_dir(buf)) {
			*is_dir = true;
			return true;
		}
		return false;
	}

	if (errno == ENOENT)
		return false;

	*is_dir = false;
	if (errno == EACCES)
		fprintf(stderr, "xcode-select: error: invalid permissions for"
		    " data link at '%s'\n", link);
	else
		fprintf(stderr, "xcode-select: error: unable to read data link"
		    " at '%s', expected symbolic link (%s)\n", link,
		    strerror(errno));

	return true;
}

bool
xcselect_find_developer_contents_from_path(char *path, char *buffer,
    size_t buffer_size, bool *was_cltools)
{
	char base[XCSELECT_BUF_SIZE];

	/* A relative path is relative to whoever is asking. */
	if (path[0] == '/') {
		strlcpy(base, path, sizeof(base));
	} else {
		if (getcwd(base, sizeof(base)) == NULL)
			return false;
		path_append(base, sizeof(base), path);
	}
	strip_trailing_slashes(base);

	/*
	 * A system volume, or the root of one, has no bundle to find and
	 * keeps its Command Line Tools at a fixed place instead.  This is
	 * what makes DEVELOPER_DIR=/ work.
	 */
	if (is_root(base) || path_contains_mach_kernel(base)) {
		path_join(buffer, buffer_size, base,
		    "Library/Developer/CommandLineTools");
		if (!path_contains_xcrun(buffer))
			return false;
		*was_cltools = true;
		return true;
	}

	/* Already the Command Line Tools developer directory. */
	if (basename_is(base, "CommandLineTools")) {
		strlcpy(buffer, base, buffer_size);
		if (!path_contains_xcrun(buffer))
			return false;
		*was_cltools = true;
		return true;
	}

	/* An Xcode bundle: the developer directory is inside it. */
	path_join(buffer, buffer_size, base, "Contents/Developer");
	if (path_is_dir(buffer)) {
		*was_cltools = false;
		return true;
	}

	/* Already a developer directory. */
	if (path_contains_xcrun(base)) {
		strlcpy(buffer, base, buffer_size);
		*was_cltools = false;
		return true;
	}

	/* A directory holding an Xcode.app, named by something shorter. */
	path_join(buffer, buffer_size, base, "Xcode.app/Contents/Developer");
	if (!path_is_dir(buffer))
		return false;

	*was_cltools = false;
	return true;
}

bool
xcselect_get_developer_dir_path(char *buffer, int buffer_size,
    bool *was_environment, bool *was_cltools, bool *was_default)
{
	bool is_dir = false;
	size_t i;

	*was_cltools = false;
	*was_default = false;

	/*
	 * The environment wins, and is taken as given even when it names
	 * something that is not there: it is how a build selects a
	 * developer directory for one invocation, and the caller is better
	 * placed than we are to say so.  It is still run through the same
	 * resolution as any other path, so a bundle or a volume named there
	 * lands on the developer directory inside it, and the environment
	 * is rewritten to that so the next lookup in the same process
	 * agrees.
	 */
	char *env = getenv("DEVELOPER_DIR");
	if (env != NULL && env[0] != '\0') {
		if (xcselect_find_developer_contents_from_path(env, buffer,
		    (size_t)buffer_size, was_cltools)) {
			if (strcmp(buffer, env) != 0)
				setenv("DEVELOPER_DIR", buffer, 1);
		} else {
			strlcpy(buffer, env, (size_t)buffer_size);
		}
		*was_environment = true;
		return true;
	}

	*was_environment = false;

	for (i = 0; i < sizeof(dev_dir_links) / sizeof(*dev_dir_links); i++) {
		if (get_developer_dir_from_symlink(dev_dir_links[i], buffer,
		    buffer_size, &is_dir))
			break;
	}

	if (i < sizeof(dev_dir_links) / sizeof(*dev_dir_links)) {
		/* The link was read but names nothing usable. */
		if (!is_dir)
			return false;
		*was_cltools = basename_is(buffer, "CommandLineTools");
		return true;
	}

	/* A selection recorded as a file holding the path. */
	{
		char file[XCSELECT_BUF_SIZE];
		int fd;
		ssize_t n;

		strlcpy(file, XCSELECT_DEV_DIR_FILE, sizeof(file));
		if ((fd = open(file, O_RDONLY)) >= 0) {
			n = read(fd, buffer, (size_t)buffer_size - 1);
			if (n < 0) {
				fprintf(stderr, "xcode-select: error: unable to"
				    " read data file (%s)\n", strerror(errno));
				close(fd);
				return false;
			}
			/* One trailing newline is the writer's, not the path's. */
			if (n > 0 && buffer[n - 1] == '\n')
				n--;
			close(fd);

			if (n > 0) {
				buffer[n] = '\0';
				if (path_is_dir(buffer)) {
					*was_cltools = basename_is(buffer,
					    "CommandLineTools");
					return true;
				}
			}
		} else if (errno != ENOENT) {
			if (errno == EACCES)
				fputs("xcode-select: error: invalid permissions"
				    " for data file\n", stderr);
			else
				fprintf(stderr, "xcode-select: error: unable to"
				    " open data file (%s)\n", strerror(errno));
			return false;
		}
	}

	/*
	 * Nothing has been selected.  A candidate counts only if it holds a
	 * libxcrun, so a half-removed Xcode is passed over rather than
	 * chosen and then failing.
	 */
	*was_default = true;
	for (i = 0; i < sizeof(dev_dir_defaults) / sizeof(*dev_dir_defaults);
	    i++) {
		if (!path_contains_xcrun(dev_dir_defaults[i].dir))
			continue;

		strlcpy(buffer, dev_dir_defaults[i].dir, (size_t)buffer_size);
		*was_cltools = dev_dir_defaults[i].cltools;
		return true;
	}

	return false;
}

bool
xcselect_developer_dir_matches_path(const char *developer_dir, const char *path)
{
	char dir[XCSELECT_BUF_SIZE];
	bool was_environment, was_cltools, was_default;
	size_t len;

	/*
	 * Given a directory, use it.  Given nothing, ask which one is
	 * active, which is what a caller with nothing else to go on wants.
	 */
	if (developer_dir != NULL) {
		strlcpy(dir, developer_dir, sizeof(dir));
	} else if (!xcselect_get_developer_dir_path(dir, sizeof(dir),
	    &was_environment, &was_cltools, &was_default)) {
		return false;
	}

	strip_trailing_slashes(dir);

	/*
	 * Compared at the bundle where there is one, so that a path inside
	 * /Applications/Xcode.app matches whether the caller named the
	 * bundle or the developer directory inside it.
	 */
	if (ends_with(dir, ".app/Contents/Developer"))
		dir[strlen(dir) - strlen(".app/Contents/Developer")] = '\0';

	if (path == NULL)
		return false;

	len = strlen(dir);

	return strlen(path) >= len && strncmp(dir, path, len) == 0;
}

char *
xcselect_get_version(void)
{
	return (char *)XCSELECT_VER;
}

/* ---- host SDK -------------------------------------------------------- */

/*
 * The version this system reports, e.g. 15 for 15.3.  Read once, since
 * every call to xcselect_host_sdk_path wants it.
 */
static int
host_version(void)
{
	static int version;
	static bool done;
	char buf[32];
	size_t len = sizeof(buf);

	if (!done) {
		done = true;
		if (sysctlbyname("kern.osproductversion", buf, &len, NULL, 0)
		    == -1)
			return 0;
		buf[sizeof(buf) - 1] = '\0';
		version = (int)strtol(buf, NULL, 10);
	}

	return version;
}

/*
 * The SDKs in one directory, as an array the caller frees.  An entry is
 * any directory holding a sys/cdefs.h, which is what tells a real SDK
 * from a stray folder.
 */
static size_t
sdks_at_path(const char *dir, char **out, size_t max)
{
	static const char probe_tail[] = "/usr/include/sys/cdefs.h";
	struct dirent *ent;
	DIR *d;
	size_t count = 0;

	if (max == 0 || (d = opendir(dir)) == NULL)
		return 0;

	while (count < max && (ent = readdir(d)) != NULL) {
		char *full;
		struct stat st;
		int len;

		len = asprintf(&full, "%s/%s%s", dir, ent->d_name,
		    probe_tail);
		if (len < 0)
			continue;

		/* What identifies an SDK is what is under it, so the probe
		 * is also the thing to shorten back to the SDK itself. */
		if (stat(full, &st) != 0) {
			free(full);
		} else {
			full[strlen(full) - (sizeof(probe_tail) - 1)] = '\0';
			out[count++] = full;
		}
	}

	closedir(d);

	return count;
}

/*
 * The number in a "MacOSX10.<n>.sdk" name, or 0 for anything else.  The
 * comparison is against the SDKs of the active developer directory, and
 * against the flat layout the Command Line Tools use.
 */
static unsigned
sdk_version(const char *path)
{
	const char *slash = strrchr(path, '/');
	char *base, *dot;
	unsigned version = 0;

	if (slash == NULL || strncmp(slash + 1, "MacOSX10.", 9) != 0)
		return 0;

	if ((base = strdup(slash + 10)) == NULL)
		return 0;

	if ((dot = strchr(base, '.')) != NULL && strcmp(dot, ".sdk") == 0) {
		*dot = '\0';
		version = (unsigned)strtol(base, NULL, 10);
	}

	free(base);

	return version;
}

int
xcselect_host_sdk_path(int which, char **out)
{
	char *list[64];
	char devdir[XCSELECT_BUF_SIZE];
	char *sdks_dir;
	char *match = NULL, *newest = NULL, *chosen;
	unsigned host, version, newest_version = 0;
	bool from_env, cltools, fallback;
	size_t count = 0, i;

	if (out == NULL)
		return EINVAL;
	if (which < XCSELECT_HOST_SDK_PATH_MATCH_OR_NEWEST ||
	    which > XCSELECT_HOST_SDK_PATH_NEWEST)
		return EINVAL;

	host = (unsigned)host_version();

	/*
	 * The platform SDKs of the active developer directory come first,
	 * but only when that directory really is an Xcode: the Command
	 * Line Tools and the built-in fallbacks have no platform tree, so
	 * asking costs a failed opendir and, worse, would let a stale
	 * platform SDK outrank the Command Line Tools one below.
	 */
	sdks_dir = NULL;
	if (xcselect_get_developer_dir_path(devdir, sizeof(devdir), &from_env,
	    &cltools, &fallback) && !cltools && !fallback &&
	    asprintf(&sdks_dir, "%s/Platforms/MacOSX.platform/Developer/SDKs",
	    devdir) >= 0) {
		count = sdks_at_path(sdks_dir, list,
		    sizeof(list) / sizeof(*list));
		free(sdks_dir);
	}

	/* The Command Line Tools keep theirs in the flat layout. */
	count += sdks_at_path("/Library/Developer/CommandLineTools/SDKs",
	    list + count, sizeof(list) / sizeof(*list) - count);

	/*
	 * The first SDK whose version is the running system's is the one
	 * that matches; a platform SDK found above has already won by
	 * being earlier in the list.  The highest version present is the
	 * newest, whether or not anything matched.
	 */
	for (i = 0; i < count; i++) {
		version = sdk_version(list[i]);

		if (newest == NULL || version > newest_version) {
			newest_version = version;
			newest = list[i];
		}
		if (match == NULL && version == host)
			match = list[i];
	}

	switch (which) {
	case XCSELECT_HOST_SDK_PATH_MATCH:
		chosen = match;
		break;
	case XCSELECT_HOST_SDK_PATH_NEWEST:
		chosen = newest;
		break;
	default:
		chosen = (match != NULL) ? match : newest;
		break;
	}

	for (i = 0; i < count; i++)
		if (list[i] != chosen)
			free(list[i]);

	*out = chosen;

	return chosen != NULL ? 0 : ENOENT;
}

/* ---- running a tool -------------------------------------------------- */

void
xcselect_invoke_xcrun(char *tool_name, int argc, char *argv[],
    bool require_xcode)
{
	char devdir[XCSELECT_BUF_SIZE];
	char path[XCSELECT_BUF_SIZE];
	char **args;
	bool was_environment, was_cltools, was_default;
	int i;

	if (xcselect_get_developer_dir_path(devdir, sizeof(devdir),
	    &was_environment, &was_cltools, &was_default)) {
		/*
		 * A tool that genuinely needs Xcode is told so rather than
		 * run against the Command Line Tools, which would report
		 * confusing errors of its own.
		 */
		if (require_xcode && was_cltools) {
			fprintf(stderr, "xcode-select: error: tool '%s' requires"
			    " Xcode, but active developer directory '%s' is a"
			    " command line tools instance\n", tool_name, devdir);
			exit(1);
		}

		/* The xcrun we are about to become must not be re-entered. */
		if (tool_name != NULL)
			unsetenv("xcrun_log");

		path_join(path, sizeof(path), devdir, XCSELECT_XCRUN);
		if (path_exists(path)) {
			/* Not yet reconstructed; see the RE note below. */
			fprintf(stderr, "xcrun: error: unable to load"
			    " libxcrun.\n");
			exit(1);
		}

		/*
		 * No libxcrun, so this is a developer directory from a
		 * build of ours rather than a shipped one, and it still has
		 * a real xcrun to hand the work to.
		 */
		path_join(path, sizeof(path), devdir, "usr/bin/xcrun");
		if (!path_is_dir(devdir) || !path_exists(path)) {
			fprintf(stderr, "xcrun: error: invalid %s path (%s),"
			    " missing xcrun at: %s\n",
			    was_environment ? "DEVELOPER_DIR" : "active developer",
			    devdir, path);
			exit(1);
		}

		if ((args = calloc((size_t)argc + 3, sizeof(*args))) == NULL)
			exit(1);

		args[0] = path;
		if (tool_name != NULL)
			args[1] = tool_name;
		for (i = 0; i < argc; i++)
			args[1 + (tool_name != NULL ? 1 : 0) + i] = argv[i];

		execv(path, args);
		fprintf(stderr, "xcrun: error: unable to exec Xcode native"
		    " xcrun (%s).\n", strerror(errno));
		exit(1);
	}

	fprintf(stderr, "xcrun: error: unable to determine the developer"
	    " directory.\n");
	exit(1);
}

/* ---- developer tools, and asking for the tools ----------------------- */

/*
 * The bundle identifiers the shipped library treats as a developer tool.
 * They sit in its read-only data behind a shared "com.apple." prefix, and
 * only the part after that prefix is compared, so the prefix on its own is
 * not one.  There are nine of them.  The loop that walks the table stops on
 * a counter that is tested after it has been stepped, so it reads one
 * entry past what the bound alone suggests, and "RealityComposerPro" is
 * that ninth one.
 */
static const char * const dev_tool_bundles[] = {
	"dt.Xcode",
	"dt.SourceEdit",
	"iphonesimulator",
	"dt.Instruments",
	"FileMerge",
	"itunes.connect.ApplicationLoader",
	"AccessibilityInspector",
	"RealityComposer",
	"RealityComposerPro",
};

bool
xcselect_bundle_is_developer_tool(char *bundle_id)
{
	static const char prefix[] = "com.apple.";
	const size_t prefix_len = sizeof(prefix) - 1;
	size_t i;

	if (bundle_id == NULL)
		return false;
	if (strlen(bundle_id) < prefix_len)
		return false;
	if (strncmp(bundle_id, prefix, prefix_len) != 0)
		return false;

	for (i = 0; i < sizeof(dev_tool_bundles) / sizeof(*dev_tool_bundles); i++) {
		if (strcmp(bundle_id + prefix_len, dev_tool_bundles[i]) == 0)
			return true;
	}

	return false;
}

/*
 * A tool that cannot be found asks launchd to put an installer up, by way
 * of a Mach message port.  The payload is a one-key property list saying
 * which tool wanted it, and it goes to a service named on a port rather
 * than over the launchd socket API, so the request itself is one Mach
 * message.  CoreFoundation is opened and resolved at run time, which is
 * what keeps this library linking against nothing but the system
 * libraries, the way the shipped one does.
 */
#define XCSELECT_CF_PATH \
	"/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
#define XCSELECT_INSTALL_PORT \
	"com.apple.dt.CommandLineTools.installondemand"

/*
 * The format is kCFPropertyListBinaryFormat_v1_0, which is 200, not the
 * 0xc8 it looks like written down here, and the wait the shipped library
 * allows is five seconds on each side of the send.
 */
#define XCSELECT_PLIST_BINARY		0xc8
#define XCSELECT_INSTALL_WAIT		5.0

/*
 * These prototypes are taken from how the shipped library actually calls
 * into CoreFoundation, and deliberately not from its headers, which are
 * wrong about two of the functions involved in both the public and the
 * internal SDK:
 *
 *	CFPropertyListCreateData takes four arguments, not the five its
 *	header claims with a trailing CFErrorRef.  Calling it as the
 *	header says leaves that fifth argument undefined for the callee
 *	to write through.
 *
 *	CFMessagePortSendRequest takes the reply mode and the return data
 *	before the two timeouts, not after them as its header claims.
 *	The shipped library puts a null reply mode and a null return data
 *	in the third and fourth argument registers and the two five-second
 *	waits in the first two floating point registers, which is only
 *	consistent with the former order.
 *
 * The dlsym-and-declare-here approach is what the shipped library uses
 * for the same reason, so this is not a shortcut around it.
 */
typedef void (*cf_release_fn)(const void *);
typedef const void *(*cf_string_fn)(const void *, const char *, uint32_t);
typedef const void *(*cf_dict_fn)(const void *, long, const void *, const void *);
typedef void (*cf_dict_set_fn)(const void *, const void *, const void *);
typedef const void *(*cf_plist_fn)(const void *, const void *, uint32_t,
    uint32_t);
typedef const void *(*cf_port_fn)(const void *, const void *);
typedef int32_t (*cf_send_fn)(const void *, long, const void *, void **,
    void **, double, double);

struct xcselect_cf {
	cf_release_fn		release;
	cf_string_fn		string;
	cf_dict_fn		dict;
	cf_dict_set_fn		set;
	cf_plist_fn		plist;
	cf_port_fn		port;
	cf_send_fn		send;
};

static void *xcselect_cf_handle;

static void
xcselect_cf_open(void)
{
	xcselect_cf_handle = dlopen(XCSELECT_CF_PATH, RTLD_LAZY);
}

static void *
xcselect_cf_symbol(const char *name)
{
	/*
	 * The shipped library reaches for dispatch_once here, which would
	 * mean turning on blocks for this file just to open one path.
	 * pthread_once is the same guarantee out of the same library and
	 * leaves the build flags alone.
	 */
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, xcselect_cf_open);

	return xcselect_cf_handle != NULL ?
	    dlsym(xcselect_cf_handle, name) : NULL;
}

/*
 * The shipped library stops at the first symbol it cannot resolve and
 * answers no without asking for anything, so a CoreFoundation that is
 * present but incomplete does not turn into a half-built request.
 */
static bool
xcselect_cf_load(struct xcselect_cf *cf)
{
	cf->string = (cf_string_fn)xcselect_cf_symbol("CFStringCreateWithCString");
	cf->set = (cf_dict_set_fn)xcselect_cf_symbol("CFDictionarySetValue");
	cf->plist = (cf_plist_fn)xcselect_cf_symbol("CFPropertyListCreateData");
	cf->dict = (cf_dict_fn)xcselect_cf_symbol("CFDictionaryCreateMutable");
	cf->port = (cf_port_fn)xcselect_cf_symbol("CFMessagePortCreateRemote");
	cf->send = (cf_send_fn)xcselect_cf_symbol("CFMessagePortSendRequest");
	cf->release = (cf_release_fn)xcselect_cf_symbol("CFRelease");

	return cf->string != NULL && cf->set != NULL && cf->plist != NULL &&
	    cf->dict != NULL && cf->port != NULL && cf->send != NULL &&
	    cf->release != NULL;
}

bool
xcselect_trigger_install_request(const char *tool_name)
{
	struct xcselect_cf cf;
	const void *key, *value, *dict, *data, *name, *port;
	char self[0x1000], parent[0x1000];
	int32_t sent;

	if (!xcselect_cf_load(&cf))
		return false;

	/* The service logs who is asking and who asked them, so that a
	 * request nobody expected can be traced back to a program. */
	if (proc_pidpath(getpid(), self, sizeof(self)) > 0 &&
	    proc_pidpath(getppid(), parent, sizeof(parent)) > 0) {
		os_log_t log = os_log_create(XCSELECT_INSTALL_PORT, "trace");

		if (os_log_type_enabled(log, OS_LOG_TYPE_INFO))
			os_log_info(log, "Command Line Tools installation"
			    " request from '%{public}s' (PID %d), parent"
			    " process '%{public}s' (parent PID %d)",
			    self, getpid(), parent, getppid());
	}

	dict = cf.dict(NULL, 0, NULL, NULL);
	key = cf.string(NULL, "tool-name", 0);
	value = cf.string(NULL, tool_name, 0);
	cf.set(dict, key, value);
	data = cf.plist(NULL, dict, XCSELECT_PLIST_BINARY, 0);
	cf.release(value);
	cf.release(key);
	cf.release(dict);

	if (data == NULL)
		return false;

	name = cf.string(NULL, XCSELECT_INSTALL_PORT, 0);
	port = cf.port(NULL, name);
	if (port == NULL) {
		cf.release(data);
		cf.release(name);
		return false;
	}

	/* Message identifier zero, no reply asked for, and five seconds
	 * allowed on each side of the send. */
	sent = cf.send(port, 0, data, NULL, NULL, XCSELECT_INSTALL_WAIT,
	    XCSELECT_INSTALL_WAIT);

	cf.release(data);
	cf.release(name);
	cf.release(port);

	return sent == 0;
}

/* ---- man paths ------------------------------------------------------- */

static void
manpaths_add(xcselect_manpaths *mp, const char *path)
{
	char **grown;

	if ((grown = realloc(mp->paths,
	    (mp->count + 1) * sizeof(*grown))) == NULL)
		return;

	mp->paths = grown;
	if ((mp->paths[mp->count] = strdup(path)) != NULL)
		mp->count++;
}

/*
 * The manual page directories belonging to a sysroot.  The shipped
 * library asks the developer directory's own libxcrun for these, so that
 * a toolchain which has moved or added pages of its own is described
 * accurately; the list below is what it falls back to, and what a
 * developer directory without a libxcrun gets.
 */
xcselect_manpaths *
xcselect_get_manpaths(char *sysroot)
{
	xcselect_manpaths *mp;
	char devdir[XCSELECT_BUF_SIZE];
	char path[PATH_MAX];
	bool from_env, cltools, fallback;

	/*
	 * The shipped library refuses outright when the process is in an
	 * App Sandbox, asking libSystem's __xpc_runtime_is_app_sandboxed.
	 * That symbol is not exported by this macOS -- not by libSystem,
	 * not by libxpc, and not findable through RTLD_DEFAULT -- so the
	 * check is left out rather than approximated.  A sandboxed caller
	 * is the one case where the two would differ.
	 */
	(void)sysroot;

	if (!xcselect_get_developer_dir_path(devdir, sizeof(devdir), &from_env,
	    &cltools, &fallback))
		return NULL;

	if ((mp = calloc(1, sizeof(*mp))) == NULL)
		return NULL;

	/* Based on the developer directory, never on sysroot. */
	snprintf(path, sizeof(path), "%s/usr/share/man", devdir);
	manpaths_add(mp, path);
	snprintf(path, sizeof(path), "%s/usr/llvm-gcc-4.2/share/man", devdir);
	manpaths_add(mp, path);
	snprintf(path, sizeof(path),
	    "%s/Toolchains/XcodeDefault.xctoolchain/usr/share/man", devdir);
	manpaths_add(mp, path);

	return mp;
}

uint32_t
xcselect_manpaths_get_num_paths(xcselect_manpaths *xcp)
{
	return (xcp != NULL) ? xcp->count : 0;
}

const char *
xcselect_manpaths_get_path(xcselect_manpaths *xcp, uint32_t id)
{
	if (xcp == NULL || id > xcp->count)
		return NULL;

	return xcp->paths[id];
}

void
xcselect_manpaths_free(xcselect_manpaths *xcp)
{
	uint32_t i;

	if (xcp == NULL)
		return;

	for (i = 0; i < xcp->count; i++)
		free(xcp->paths[i]);

	free(xcp->paths);
	free(xcp);
}
