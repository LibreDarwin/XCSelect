/* libxcrun - the developer directory's half of xcrun
 *
 * Apple's /usr/bin/xcrun is not the implementation.  It is a 117 KB shim
 * that links only libxcselect and libSystem, imports exactly one symbol
 * from us -- xcselect_invoke_xcrun -- and never names libxcrun itself.
 * libxcselect is what resolves <developer dir>/usr/lib/libxcrun.dylib and
 * calls xcrun_main in it.  There is no xcrun binary inside Xcode.app at
 * all, so the one shim serves every developer directory, and each one
 * carries its own copy of this library.
 *
 * So this file is the work that binary used to do itself.  It is derived
 * from src/xcrun/xcrun.c, which is the same code, and the four exports
 * below are the whole of Apple's ABI for it:
 *
 *	const char *xcrun_get_version(void);
 *	void xcrun_set_unknown_utility_handler(void (^)(const char *));
 *	void xcrun_main(char *tool_name, int argc, char *argv[],
 *	    const char *devdir);
 *	void xcrun_iter_manpaths(const char *devdir, const char *sysroot,
 *	    void (^iter)(const char *path));
 *
 * None of them appear in a header, so the shipped library is called by
 * dlsym.  The signatures here were read off the disassembly rather than
 * guessed: xcrun_main takes its four arguments in x0-x3 and tail-calls
 * exit, xcrun_get_version is a one-instruction return of a literal,
 * xcrun_set_unknown_utility_handler Block_copies x0 into a global, and
 * xcrun_iter_manpaths strdups its second argument and calls the third
 * through a helper.
 *
 * xcrun_main does not return.  libxcselect treats a return as an
 * unexpected exit and reports it.
 *
 * Copyright (c) 2013-2014, Brian McKenzie <mckenzba@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the organization nor the names of its contributors may
 *     be used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

extern char **environ;

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>
#include <errno.h>
#include <sysexits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "ini.h"
#include <stdbool.h>

#include "devpath.h"
#include "xcselect.h"
#include "sdkpath.h"

/*
 * xcrun_set_unknown_utility_handler copies the block it is handed, the
 * same as the shipped library, so that a caller can hand us one from a
 * dlsym it has no type for and let it outlive the frame it was built in.
 */
#include <Block.h>

/* General stuff */
/* What the shipped library reports, e.g. "xcrun version 72." */
#define TOOL_VERSION "72."
#define SDK_CFG ".xcdev.dat"
#ifndef XCRUN_DEFAULT_CFG
#define XCRUN_DEFAULT_CFG "/usr/local/etc/xcrun.ini"
#endif
#ifndef XCRUN_DEFAULT_DEVELOPER_DIR
#define XCRUN_DEFAULT_DEVELOPER_DIR "/Library/Developer/CommandLineTools"
#endif

/*
 * Which of the --show-* answers was asked for.
 *
 * Only one of them is printed when several are given.  The SDK and
 * platform ones answer to the order they were asked in rather than to a
 * fixed one -- "xcrun --show-sdk-version --show-sdk-build-version" prints
 * the build and the other way round prints the version -- so the one
 * asked for last is the one kept.  The toolchain path is not one of
 * them: it is answered only when no SDK or platform answer was asked
 * for, and otherwise goes unanswered however late it comes.
 */
enum show_kind {
	SHOW_NONE = 256,
	SHOW_SDK_PATH,
	SHOW_SDK_VERSION,
	SHOW_SDK_BUILD_VERSION,
	SHOW_SDK_PLATFORM_PATH,
	SHOW_SDK_PLATFORM_VERSION
};

/* SDK configuration struct */
typedef struct {
	const char *name;
	const char *version;
	const char *toolchain;
	const char *default_arch;
	const char *deployment_target;
} sdk_config;

/* xcrun default configuration struct */
typedef struct {
	const char *sdk;
	const char *toolchain;
} default_config;

/* Output mode flags */
static int logging_mode = 0;
static int verbose_mode = 0;
static int finding_mode = 0;

/* Behavior mode flags */
static int explicit_sdk_mode = 0;
static int explicit_toolchain_mode = 0;
static int ios_deployment_target_set = 0;
static int macosx_deployment_target_set = 0;

/*
 * Whether the run went around the cache file rather than through it.  The
 * trace names the file, and the names are only true of a run that used it,
 * so the trace needs to know; the flag is not a mode of its own and is
 * kept beside the others rather than in the parser with its own kind of
 * local because the notes are printed from everywhere.
 */
static int cache_bypass_f = 0;

/*
 * Whether the run emptied the cache file first.  That is not the same as
 * bypassing it, so it is kept apart: a -k run still goes through the cache
 * afterwards, which is why it still names the key, but it has just made
 * sure there is nothing in the file to answer from, which is why it has no
 * answer to report.
 */
static int cache_cleared_f = 0;

/* Runtime info */
static char *developer_dir = NULL;
static char *current_sdk = NULL;
static char *current_toolchain = NULL;

/*
 * What libxcselect registers for a tool we do not know.  Only ever set
 * when we were called as something other than xcrun -- a multicall binary
 * that picked us up, or xcode-select -- because in that case the caller,
 * not us, decides whether an unknown name means "install the command line
 * tools" or something else.  The shipped library Block_copies the block
 * into here, so the copy outlives the frame it was built in.
 */
static void (^unknown_utility_handler)(const char *) = NULL;

/* Alternate behavior flags */
static char *alternate_sdk_path = NULL;
static char *alternate_toolchain_path = NULL;

/*
 * The --toolchain argument as it was written.  TOOLCHAINS is that text
 * verbatim, and it is the only place the distinction survives: a short
 * name is stripped of its extension for searching, a path is turned
 * into a toolchain search path, and neither is what TOOLCHAINS says.
 */
static const char *requested_toolchain = NULL;

/* Our program's name as called by the user */
static char *progname;

/* Longest SDK or toolchain name we keep. */
#define XCRUN_NAME_MAX 255

/*
 * A toolchain is named by its directory or by its bundle, and both
 * spellings mean the same toolchain, so a bundle suffix is dropped to find
 * them together.
 *
 * Only a known suffix, and only at the end.  A name is full of dots that
 * carry meaning: cutting at the first one turns "macosx26.5" into
 * "macosx26", which matches no SDK at all, and "MacOSX.Internal" into
 * "MacOSX" -- which matches the public SDK, so asking for the internal
 * one would quietly get the wrong SDK instead of an error.
 *
 * An SDK has no such trimming.  Named by path it is that path, and named
 * by name it is looked up exactly as it was written, because "MacOSX26.5"
 * and "MacOSX26.5.sdk" are not two spellings of one question -- the first
 * is a name and the second names nothing at all.
 */
static const char *const toolchain_exts[] = { ".xctoolchain", ".toolchain", NULL };

/*
 * basename(3) may write through its argument and may return a pointer
 * into storage the next call overwrites.  Both matter here: the paths
 * come from getenv(), which is not ours to modify, and a result kept
 * across another call changes underneath us -- which is what made
 * `TOOLCHAINS=... xcrun --find clang` report the toolchain as the
 * command it could not find.  So basename only ever sees a private
 * copy, and its result is copied out before anything else runs.
 */
static char *dup_basename(const char *path)
{
	char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "%s", (path != NULL) ? path : "");
	return strdup(basename(buf));
}

static void stripext(char *dst, size_t dstlen, const char *src,
                     const char *const *exts)
{
	size_t len, i;

	if (dst == NULL || dstlen == 0)
		return;
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}

	len = strlen(src);
	for (i = 0; exts[i] != NULL; i++) {
		size_t elen = strlen(exts[i]);

		if (len > elen && strcasecmp(src + len - elen, exts[i]) == 0) {
			len -= elen;
			break;
		}
	}

	if (len >= dstlen)
		len = dstlen - 1;

	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* Name of an SDK or toolchain given a path to it. */
static void name_from_path(char *dst, size_t dstlen, const char *path,
                           const char *const *exts)
{
	char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "%s", (path != NULL) ? path : "");
	stripext(dst, dstlen, basename(buf), exts);
}

/* helper function to test for the authenticity of an sdk */
static int test_sdk_authenticity(const char *path)
{
	int retval = 0;
	char *fname = NULL;

	fname = (char *)malloc(PATH_MAX - 1);

	sprintf(fname, "%s/info.ini", path);
	if (access(fname, F_OK) != (-1))
		retval = 1;

	free(fname);

	return retval;
}

/**
 * @func logging_printf -- Print output to fp in logging mode.
 * @arg fp - pointer to file (file, stderr, or stdio)
 * @arg str - string to print
 * @arg ... - additional arguments used
 */
static void logging_printf(FILE *fp, const char *str, ...)
{
	va_list args;

	if (logging_mode == 1) {
		va_start(args, str);
		vfprintf(fp, str, args);
		va_end(args);
	}
}

/**
 * @func usage -- Print helpful information about this program.
 * @arg status - what to exit with: 0 when the help was all that was asked
 * for, EX_USAGE when it is the answer to being used wrongly
 *
 * The complaint itself, when there is one, is printed by the caller: what
 * was wrong with the command line is said before the help that follows it.
 */
static void usage(int status)
{
	fprintf(stderr,
		"Usage: %s [options] <tool name> ... arguments ...\n"
		"\n"
		"Find and execute the named command line tool from the active developer\n"
		"directory.\n"
		"\n"
		"The active developer directory can be set using `xcode-select`, or via the\n"
		"DEVELOPER_DIR environment variable. See the xcrun and xcode-select manual\n"
		"pages for more information.\n"
		"\n"
		"Options:\n"
		"  -h, --help                  show this help message and exit\n"
		"  --version                   show the xcrun version\n"
		"  -v, --verbose               show verbose logging output\n"
		"  --sdk <sdk name>            find the tool for the given SDK name\n"
		"  --toolchain <name>          find the tool for the given toolchain\n"
		"  -l, --log                   show commands to be executed (with --run)\n"
		"  -f, --find                  only find and print the tool path\n"
		"  -r, --run                   find and execute the tool (the default behavior)\n"
		"  -n, --no-cache              do not use the lookup cache\n"
		"  -k, --kill-cache            invalidate all existing cache entries\n"
		"  --show-sdk-path             show selected SDK install path\n"
		"  --show-sdk-version          show selected SDK version\n"
		"  --show-sdk-build-version    show selected SDK build version\n"
		"  --show-sdk-platform-path    show selected SDK platform path\n"
		"  --show-sdk-platform-version show selected SDK platform version\n"
		"  --show-toolchain-path       show selected SDK preferred toolchain path\n"
		, progname);

	exit(status);
}

/**
 * @func no_utility_named -- report a run that names no tool
 *
 * Every way of asking to be run or looked up without saying which tool
 * gets the same answer, whatever option was used: -f, -r, -l, -v and -n
 * all say the utility was not named, and all of them print the usage after
 * it.  An option that takes a value instead says the argument is missing,
 * because the thing that is missing is the argument rather than the tool.
 */
static void no_utility_named(void)
{
	fprintf(stderr, "%s: error: no utility name specified\n", progname);
	usage(EX_USAGE);
}

/**
 * @func no_such_utility -- report a tool that could not be found
 *
 * The same message whether the tool was to be printed or run: the answer
 * is that there is no such utility, which is not a usage mistake, so it
 * exits EX_OSFILE rather than EX_USAGE.  The sh -c line above it has
 * already said what was tried.
 */
/**
 * @func named_path -- the tool as a path, when the caller named one
 * @arg name - the tool name as the caller wrote it
 * @return: the path to run, or NULL when the name is a name to search for
 *
 * A name holding a slash is a path and not a name, and is never looked
 * for: "xcrun /usr/bin/clang" runs that file and says nothing about
 * whether any directory on the search list has a clang of its own.  So
 * it is answered even when nothing is there -- "xcrun --find
 * /nonexistent/tool" prints the path and succeeds, and only running it
 * fails -- and the search, the toolchain and xcodebuild are all skipped.
 *
 * A relative path is made absolute against the current directory and
 * left as written otherwise, so "./tool" is reported as
 * "<cwd>/./tool" and not tidied into a canonical path.  Apple does not
 * resolve the dots either, and the difference is visible in what --find
 * prints.
 */
static char *named_path(const char *name)
{
	char cwd[PATH_MAX], *path;

	if (name == NULL || strchr(name, '/') == NULL)
		return NULL;

	if (name[0] == '/')
		return strdup(name);

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return NULL;
	if (asprintf(&path, "%s/%s", cwd, name) < 0)
		return NULL;

	return path;
}

static void no_such_utility(const char *name)
{
	fprintf(stderr, "%s: error: unable to find utility \"%s\", not a developer tool or in PATH\n",
	    progname, name);
	exit(EX_OSFILE);
}

/**
 * @func version -- print out version info for this tool
 */
static void version(void)
{
	fprintf(stdout, "xcrun version %s\n", TOOL_VERSION);
	exit(0);
}

/**
 * @func validate_directory_path -- validate if requested directory path exists
 * @arg dir - directory to validate
 * @return: 0 on success, -1 on failure, with errno left describing why
 *
 * Silent: the caller knows better than this whether a path it cannot
 * resolve is worth telling the user about, and saying it twice is worse
 * than not saying it at all.
 */
static int validate_directory_path(const char *dir)
{
	struct stat fstat;

	if (stat(dir, &fstat) != 0)
		return -1;

	return S_ISDIR(fstat.st_mode) ? 0 : -1;
}

/**
 * @func devdir_has_toolchains -- whether the developer dir keeps toolchains
 *
 * A full developer dir holds a Toolchains directory, and an SDK is resolved
 * through the toolchain that owns it.  The flat Command Line Tools layout
 * has no such directory: its SDKs and its tools stand on their own, with no
 * toolchain in between.  What a missing SDK costs therefore differs by
 * layout, and this is the difference being asked about.
 */
static int
devdir_has_toolchains(void)
{
	char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "%s/Toolchains", developer_dir);
	return validate_directory_path(buf) == 0;
}

/**
 * @func sdk_cfg_handler -- handler used to process sdk info.ini contents
 * @arg user - ini user pointer (see ini.h)
 * @arg section - ini section name (see ini.h)
 * @arg name - ini variable name (see ini.h)
 * @arg value - ini variable value (see ini.h)
 * @return: 1 on success, 0 on failure
 */
static int sdk_cfg_handler(void *user, const char *section, const char *name, const char *value)
{
	sdk_config *config = (sdk_config *)user;

	if (MATCH_INI_STON("SDK", "name"))
		config->name = strdup(value);
	else if (MATCH_INI_STON("SDK", "version"))
		config->version = strdup(value);
	else if (MATCH_INI_STON("SDK", "toolchain"))
		config->toolchain = strdup(value);
	else if (MATCH_INI_STON("SDK", "default_arch"))
		config->default_arch = strdup(value);
	else if (MATCH_INI_STON("SDK", "ios_deployment_target")) {
		ios_deployment_target_set = 1;
		macosx_deployment_target_set = 0;
		config->deployment_target = strdup(value);
	} else if (MATCH_INI_STON("SDK", "macosx_deployment_target")) {
		ios_deployment_target_set = 0;
		macosx_deployment_target_set = 1;
		config->deployment_target = strdup(value);
	} else
		return 0;

	return 1;
}

/**
 * @func default_cfg_handler -- handler used to process xcrun's xcrun.ini contents
 * @arg user - ini user pointer (see ini.h)
 * @arg section - ini section name (see ini.h)
 * @arg name - ini variable name (see ini.h)
 * @arg value - ini variable value (see ini.h)
 * @return: 1 on success, 0 on failure
 */
static int default_cfg_handler(void *user, const char *section, const char *name, const char *value)
{
	default_config *config = (default_config *)user;

	if (MATCH_INI_STON("SDK", "name"))
		config->sdk = strdup(value);
	else if (MATCH_INI_STON("TOOLCHAIN", "name"))
		config->toolchain = strdup(value);
	else
		return 0;

	return 1;
}

/**
 * @func get_sdk_info -- fetch config info from a toolchain's info.ini
 * @arg path - path to sdk's info.ini
 * @return: struct containing sdk config info
 */
static sdk_config get_sdk_info(const char *path)
{
	sdk_config config;
	char info_path[PATH_MAX];
	char *value;

	memset(&config, 0, sizeof(config));

	/*
	 * No SDK at all is a thing the caller has already reported, and there
	 * is no directory here to have failed to read: every field is simply
	 * empty.  Saying "failed to retrieve sdk info from '(null)'" would
	 * name a path that was never asked for.
	 */
	if (path == NULL) {
		config.name = strdup("");
		config.deployment_target = strdup("");
		config.toolchain = strdup("");
		config.default_arch = strdup("");
		return config;
	}

	/*
	 * Prefer SDKSettings.plist, which is what a stock SDK carries -- in
	 * binary form there, XML in one we emit; sdkpath.c handles both.
	 * An SDK in the older layout still answers through info.ini.
	 */
	if ((value = xt_sdk_setting(path, "Version")) != NULL) {
		config.version = value;

		if ((value = xt_sdk_setting(path, "CanonicalName")) != NULL) {
			/* "macosx26.5" -- the name is the leading alphabetic run. */
			size_t n = strcspn(value, "0123456789");
			char *name = strndup(value, n);

			config.name = (name != NULL) ? name : strdup(value);
			free(value);
		} else {
			config.name = strdup("");
		}

		value = xt_sdk_setting(path, "DefaultDeploymentTarget");
		config.deployment_target = (value != NULL) ? value : strdup("");

		/*
		 * Neither of these appears in SDKSettings.plist.  The
		 * toolchain is a property of the Developer directory rather
		 * than of the SDK, and the architecture comes from the host
		 * or from -arch.
		 */
		config.toolchain = strdup("");
		config.default_arch = strdup("");

		return config;
	}

	snprintf(info_path, sizeof(info_path), "%s/info.ini", path);

	if (ini_parse(info_path, sdk_cfg_handler, &config) != (-1))
		return config;

	fprintf(stderr, "xcrun: error: failed to retrieve sdk info from '%s'. (errno=%s)\n",
		path, strerror(errno));
	exit(1);
}

/**
 * @func get_default_info -- fetch default configuration for xcrun
 * @arg path - path to xcrun.ini
 * @return: struct containing default config info
 */
  /* Defined below. */
  static const char *default_cfg_path(void);

static default_config get_default_info(const char *path)
{
	default_config config;

	memset(&config, 0, sizeof(config));

	/*
	 * A missing configuration is not an error: a Developer directory we
	 * did not build -- a stock Xcode, say -- has no xcrun.ini, and the
	 * defaults can be discovered from its layout instead.  See
	 * default_sdk_name() and default_toolchain_name().
	 */
	(void)ini_parse(path, default_cfg_handler, &config);

	return config;
}

/**
 * @func default_sdk_name -- SDK to use when none was requested
 *
 * The configured default wins; failing that, any SDK present in the
 * Developer directory does, which is what lets xcrun work against a
 * stock Xcode with no configuration of ours in it at all.
 *
 * @return: SDK name, or NULL when the directory holds none
 */
static char *default_sdk_name(void)
{
	default_config config = get_default_info(default_cfg_path());
	char *name;

	if (config.sdk != NULL && *config.sdk != '\0')
		return strdup(config.sdk);

	/*
	 * The default is a path, not a name, on purpose.  A stock Xcode has
	 * MacOSX.sdk, MacOSX26.5.sdk and MacOSX26.sdk all answering to
	 * macosx26.5, and a name handed back to xt_find_sdk comes to be
	 * resolved to whichever of those the search prefers -- the versioned
	 * one -- where the shipped xcrun reports MacOSX.sdk.  Resolving once,
	 * here, keeps the answer.
	 */
	if ((name = xt_default_sdk_path(developer_dir)) != NULL) {
		return name;
	}

	fprintf(stderr, "xcrun: error: no sdk configured and none found in '%s'.\n",
		developer_dir != NULL ? developer_dir : "(unset)");
	exit(1);
}

/**
 * @func default_toolchain_name -- toolchain to use when none was requested
 *
 * As above, with XcodeDefault as the last resort: it is the name a stock
 * toolchain carries, and the one our own bundles emit.  It is returned
 * whether or not a toolchain by that name is installed, since the name only
 * has to be conventional -- an unresolvable one is reported at the path it
 * would occupy rather than refused, and the Command Line Tools have no
 * toolchain directory at all.
 *
 * @return: toolchain name
 */
static char *default_toolchain_name(void)
{
	default_config config = get_default_info(default_cfg_path());

	if (config.toolchain != NULL && *config.toolchain != '\0')
		return strdup(config.toolchain);

	return strdup("XcodeDefault");
}

/**
 * @func get_toolchain_path -- Return the specified toolchain path
 * @arg name - name of the toolchain
 * @return: absolute path of toolchain
 *
 * A toolchain that is not installed is still reported at the path it would
 * occupy.  The Command Line Tools ship no Toolchains directory at all, and
 * xcrun --show-toolchain-path answers
 * <dev>/Toolchains/XcodeDefault.xctoolchain there anyway; a name that
 * cannot be resolved does not fail either, since the answer only ever goes
 * into a PATH, where a directory that is not there contributes nothing.
 * An installed toolchain is preferred, so a directory that happens to be
 * there with the older suffix is found rather than guessed at.
 */
static char *get_toolchain_path(const char *name)
{
	char *path = NULL;
	size_t e;
	char buf[PATH_MAX];

	if (developer_dir == NULL) {
		fprintf(stderr, "xcrun: error: failed to retrieve developer path, do you have it set?\n");
		exit(1);
	}

	/* Apple's <name>.xctoolchain first, then the older <name>.toolchain. */
	if ((path = xt_find_toolchain(developer_dir, name)) != NULL)
		return path;

	for (e = 0; toolchain_exts[e] != NULL; e++) {
		snprintf(buf, sizeof(buf), "%s/Toolchains/%s%s",
		    developer_dir, name, toolchain_exts[e]);
		return strdup(buf);
	}

	/* Unreachable: toolchain_exts is not empty. */
	fprintf(stderr, "xcrun: error: \'%s\' is not a valid toolchain name in \'%s\'.\n",
		name, developer_dir);
	exit(1);
}

/**
 * @func default_cfg_path -- locate xcrun's default configuration
 *
 * The release tree is relocatable, so a copy of xcrun.ini inside the
 * current developer directory takes precedence over the absolute
 * XCRUN_DEFAULT_CFG baked in at build time.  mk/bundle.mk emits the
 * former at <developer_dir>/usr/share/xcrun.ini; the compiled-in path
 * remains the fallback for an installed system copy.
 *
 * @return: path to the configuration to read
 */
static const char *default_cfg_path(void)
{
	static char path[PATH_MAX];
	struct stat st;
	char *devpath;

	if ((devpath = developer_dir) != NULL) {
		snprintf(path, sizeof(path), "%s/usr/share/xcrun.ini", devpath);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			return path;
		}
	}

	return XCRUN_DEFAULT_CFG;
}

/**
 * @func get_sdk_path -- Return the specified sdk path
 * @arg name - name of the sdk
 * @return: malloc'd path on success, NULL if the name locates no SDK
 *
 * A failure here is one attempt that did not find an SDK, and the shipped
 * library says so once per attempt rather than once per command: --find
 * resolves the SDK a single time and then goes on to look for the tool,
 * while every --show-* option resolves it a second time to name the item
 * it could not look up, and so is the one that stops.  Callers that need
 * the SDK cannot have it report why on their behalf, so they say it.
 */
/* The trace of a run that resolves an SDK is printed from below manpath_env. */
static void verbose_manpath_note(const char *sdkfield, const char *sdkpath);

/* Defined below manpath_env, used by get_sdk_path. */
static char *xcodebuild_sdk_query(const char *arg, const char *query, int voice);
static int cache_db_has_key(const char *sdkfield);

static char *get_sdk_path(const char *name)
{
	char *path = NULL;

	if (developer_dir == NULL) {
		fprintf(stderr, "xcrun: error: failed to retrieve developer path, do you have it set?\n");
		exit(1);
	}

	/*
	 * Apple keeps SDKs inside their platform bundle; the older flat
	 * <dev>/SDKs/<name>.sdk is still accepted.  See sdkpath.c.
	 *
	 * An absolute path is taken as the SDK itself rather than as a name
	 * to look up.  That is how an explicit "-sdk /path/to/SDK.sdk" is
	 * meant to work, and it is also what the default comes back as: the
	 * default is resolved to a path already, because several bundles can
	 * answer to one name and looking it up a second time would pick a
	 * different one.
	 *
	 * A path that is not there is still carried on with rather than
	 * refused: the option that named it has already said so, and the
	 * tool lookup that follows is what decides whether the SDK being
	 * unusable matters.  A tool that does not need one still runs.
	 */
	if (*name == '/') {
		validate_directory_path(name);

		/*
		 * Where a toolchain is in charge, a path asked for with the
		 * cache put aside is resolved the same way a name is: the
		 * toolchain says where it is, and the platform it lives in
		 * is named after it.  It is part of the same trace, so a
		 * bypassed or cleared cache reads nothing and asks the
		 * toolchain everything.
		 */
		if (devdir_has_toolchains() &&
		    (cache_bypass_f == 1 || cache_cleared_f == 1))
			(void)xcodebuild_sdk_query(name, "PlatformPath", 1);

		return strdup(name);
	}

	/*
	 * The directory is where we answer a name ourselves, and it is
	 * where a bypassed or cleared cache is still answered from, where
	 * there is no other answer to give: those flags govern the cache,
	 * and the directory is not it.  A toolchain layout is the case that
	 * has to ask elsewhere once the cache is put aside.
	 */
	if ((!devdir_has_toolchains() || (cache_bypass_f != 1 && cache_cleared_f != 1)) &&
	    (path = xt_find_sdk(developer_dir, name)) != NULL)
		return path;

	/*
	 * Only where the SDKs sit beside each other does the locating belong
	 * to us.  Where toolchains are kept it is xcodebuild's, and it says
	 * so itself -- with a result bundle and a timestamp on every line, so
	 * that half is not reproduced here.
	 */
	if (!devdir_has_toolchains()) {
		fprintf(stderr, "xcrun: error: SDK \"%s\" cannot be located\n", name);
		return NULL;
	}

	/*
	 * The name was not answered by the directory, so the toolchain is
	 * asked where the SDK is.  A name it cannot place has already said
	 * so, in its own terms, and there is no SDK then whether or not a
	 * tool that follows on needs one: the name named an SDK, and the
	 * SDK is what did not come back.
	 */
	if ((path = xcodebuild_sdk_query(name, "Path", 1)) == NULL)
		return NULL;

	/*
	 * Where the cache was put aside the two-ask form is made whole: the
	 * name resolved, and the platform it lives in follows, both asked of
	 * the toolchain that answered the first.
	 */
	if (cache_bypass_f == 1 || cache_cleared_f == 1)
		(void)xcodebuild_sdk_query(path, "PlatformPath", 1);

	return path;
}

/**
 * @func require_sdk_path -- get_sdk_path for a caller that cannot go on
 * @arg name - name of the sdk
 * @arg item - the SDK item being asked for, named in the error
 * @return: malloc'd path; does not return if the name locates no SDK
 *
 * A --show-* option is only ever about the SDK, so there is nothing left
 * to do once the SDK is not there: the item that could not be looked up
 * is what the command actually asked for, and it is named as such.
 */
static char *require_sdk_path(const char *name, const char *item)
{
	char *path = get_sdk_path(name);

	/*
	 * The SDK has been located, or has said that it is not there, and
	 * either way the manual page lookup that every --show-* shares has
	 * now been made -- the trace puts it here, between the report of the
	 * SDK and the report of the item, which is where the shipped library
	 * has it.
	 */
	verbose_manpath_note(path != NULL ? path : name, path);

	if (path != NULL)
		return path;

	/*
	 * Naming the item that could not be looked up is itself a lookup in
	 * the same SDK, so the SDK is reported as not located a second time
	 * here -- the shipped library looks it up again rather than
	 * remembering the answer, and a run of this option shows two
	 * "cannot be located" lines where a run of a tool shows one.
	 *
	 * Where toolchains are kept the miss is the toolchain's own doing and
	 * is told in its own terms, which a flat layout has none of: the SDK
	 * is asked where it lives, and that answer -- a property list that
	 * was never there -- is what fails, before the item is asked for at
	 * all.  Both are lookups in the same SDK, so both are made.
	 *
	 * Both are made the first time, though, and not after: a name the
	 * cache has already been asked about is one whose question has
	 * already been answered, and a run that finds the answer repeats
	 * neither the question nor its failure.  So the first run of a name
	 * that is not there asks twice more and the ones after it ask
	 * neither, which is the only difference between them.
	 */
	if (devdir_has_toolchains() && !cache_db_has_key(name)) {
		char *cwd = getcwd(NULL, 0);

		(void)xcodebuild_sdk_query(name, "PlatformPath", 0);
		fprintf(stderr, "xcrun: error: Failed to open property list '%s/%s/SDKSettings.plist'\n",
		    cwd != NULL ? cwd : "", name);
		(void)xcodebuild_sdk_query(name, item, 0);
		free(cwd);
	} else {
		(void)get_sdk_path(name);
	}
	fprintf(stderr, "xcrun: error: unable to lookup item '%s' in SDK '%s'\n", item, name);
	exit(1);
}

/**
 * @func sdk_path_or_null -- get_sdk_path for a caller that may not have one
 * @arg name - name of the sdk
 * @return: malloc'd path, or NULL; never exits
 *
 * For the environment a tool is run with.  SDKROOT, the target triple and
 * the deployment target are all read from the SDK, and an SDK that is not
 * there simply leaves those unset rather than stopping a tool that has
 * nothing to do with it -- the SDK has already said that it is not there.
 */
static char *sdk_path_or_null(const char *name)
{
	static char *cached_name = NULL;
	static char *cached_path = NULL;
	static int tried = 0;
	char *path;

	/*
	 * These three callers are one question asked three times, and the
	 * shipped library answers it once: a run against an SDK that is not
	 * there reports that SDK once, not once per variable.
	 */
	if (tried && strcmp(cached_name, name) == 0)
		return (cached_path == NULL) ? NULL : strdup(cached_path);

	path = get_sdk_path(name);

	free(cached_name);
	free(cached_path);
	cached_name = strdup(name);
	cached_path = (path == NULL) ? NULL : strdup(path);
	tried = 1;

	return path;
}

/*
 * parse_target_triple() and get_target_triple() are gone.  They built the
 * TARGET_TRIPLE that this used to hand every tool, which Apple does not
 * set: asked for the environment of a tool, Apple names SDKROOT, MANPATH,
 * CPATH, LIBRARY_PATH and a deployment target, and nothing else.
 */

/**
 * @func sdk_platform_dir -- the platform bundle an SDK is laid out in
 * @arg devdir - the developer directory in effect
 * @arg sdk - the SDK's path
 * @return: the ".../Platforms/<name>.platform" directory, or NULL
 *
 * A full Xcode keeps each SDK inside its platform, which is how the
 * platform is found: the name in the path is read off rather than
 * guessed, so a platform bundle this source has never heard of is
 * reported as itself.  A Command Line Tools directory has no Platforms
 * at all, and says nothing here.
 *
 * It has to be a platform of *this* developer directory.  An SDK handed
 * over by path can come from anywhere, and one from another Xcode is not
 * part of this installation: asked to run against an iPhoneOS SDK from
 * elsewhere, with a Command Line Tools directory in effect, Apple's
 * manual page path names the SDK's own pages and no platform at all.
 */
static char *sdk_platform_dir(const char *devdir, const char *sdk)
{
	static const char marker[] = "/Platforms/";
	const char *name, *end;
	char *here;
	size_t len;

	if (sdk == NULL || (len = strlen(devdir)) == 0)
		return NULL;

	if (asprintf(&here, "%s%s", devdir, marker) < 0)
		return NULL;
	if (strncmp(sdk, here, strlen(here)) != 0) {
		free(here);
		return NULL;
	}
	free(here);

	name = sdk + len + sizeof(marker) - 1;
	if ((end = strstr(name, ".platform/")) == NULL)
		return NULL;

	return strndup(sdk, (size_t)(end - sdk) + sizeof(".platform") - 1);
}

/**
 * @func sdk_dir_name -- the name of the directory an SDK is
 * @arg sdk - the SDK's path, or NULL
 * @return: the last path component, borrowed from sdk
 */
static const char *sdk_dir_name(const char *sdk)
{
	const char *slash;

	if (sdk == NULL)
		return NULL;

	slash = strrchr(sdk, '/');

	return (slash != NULL) ? slash + 1 : sdk;
}

/**
 * @func path_join -- a path under a directory, or NULL
 * @arg dir - the directory
 * @arg leaf - what to put under it
 */
static char *path_join(const char *dir, const char *leaf)
{
	char *path;

	if (dir == NULL || asprintf(&path, "%s/%s", dir, leaf) < 0)
		return NULL;

	return path;
}

/**
 * @func manpath_add -- add one entry to a MANPATH being built
 * @arg manpath - the MANPATH so far, or NULL while it is still empty
 * @arg path - the entry to add
 *
 * Every entry is written with its trailing colon, so the list ends in
 * one -- which is what Apple writes, and what leaves the empty entry at
 * the end to fall back on the system's own pages.
 */
static int manpath_add(char **manpath, const char *path)
{
	char *next;

	if (asprintf(&next, "%s%s:", (*manpath != NULL) ? *manpath : "", path) < 0)
		return -1;

	free(*manpath);
	*manpath = next;

	return 0;
}

/**
 * @func manpath_env -- the manual page path a tool is run with
 * @arg devdir - the developer directory in effect
 * @arg sdk - the SDK in effect, or NULL when none could be located
 * @return: the MANPATH value, or NULL if it could not be built
 *
 * The SDK's own pages, then the platform's, then the developer
 * directory's, then the default toolchain's.
 *
 * The SDK contributes nothing when it could not be located, but the
 * platform still does: an SDK named and not found leaves the platform
 * alone, and the host's is named in its place.  That is why a bad SDK
 * against a full Xcode still reports MacOSX.platform's pages.
 *
 * The platform is named only where its bundle is really there, since a
 * Command Line Tools directory has no platform bundle and Apple does not
 * invent one.  The toolchain's pages are named whether or not they
 * exist, also as Apple does: a toolchain not yet laid down is still
 * where its pages will go.
 */
static char *manpath_env(const char *devdir, const char *sdk)
{
	struct stat st;
	char toolchain[PATH_MAX], *manpath = NULL, *dir, *page;

	if (sdk != NULL) {
		if ((page = path_join(sdk, "usr/share/man")) == NULL ||
		    manpath_add(&manpath, page) != 0) {
			free(page);
			goto fail;
		}
		free(page);
	}

	if ((dir = sdk_platform_dir(devdir, sdk)) == NULL &&
	    (dir = path_join(devdir, "Platforms/MacOSX.platform")) == NULL)
		goto fail;
	if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
		if ((page = path_join(dir, "usr/share/man")) == NULL ||
		    manpath_add(&manpath, page) != 0) {
			free(page);
			free(dir);
			goto fail;
		}
		free(page);
	}
	free(dir);

	if ((page = path_join(devdir, "usr/share/man")) == NULL ||
	    manpath_add(&manpath, page) != 0) {
		free(page);
		goto fail;
	}
	free(page);

	snprintf(toolchain, sizeof(toolchain),
	    "%s/Toolchains/%s/usr/share/man", devdir, "XcodeDefault.xctoolchain");
	if (manpath_add(&manpath, toolchain) != 0)
		goto fail;

	return manpath;

fail:
	free(manpath);
	return NULL;
}

/*
 * What -v adds is notes on stderr about the work the run would otherwise do
 * in silence, and they are notes rather than warnings: nothing is diagnosed
 * and no exit status changes with them on.  The two that describe the cache
 * also say where that cache is, which is a per-user temporary file rather
 * than anything under the developer directory, and which is not where
 * TMPDIR points -- that variable is set per process for the child, and the
 * shipped library takes the per-user answer the C library gives instead.
 */

/**
 * @func cache_db_path -- the path of the cache file, if it can be had
 * @arg buf - where to write the path
 * @arg len - the size of buf
 * @return: buf, or NULL when the answer is not available
 */
static char *cache_db_path(char *buf, size_t len)
{
	size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, len);

	if (n == 0 || n > len)
		return NULL;

	/* The answer names a directory and ends in its separator already. */
	if (snprintf(buf + strlen(buf), len - strlen(buf), "%s", "xcrun_db") >= (int)(len - strlen(buf)))
		return NULL;

	return buf;
}

/**
 * @func cache_db_has_key -- whether the cache already holds an answer
 * @arg sdkfield - the SDK as it is named in the key: a path, or the name
 *     as the user wrote it when no SDK could be located
 * @return: 1 when the key is in the file, 0 when it is not or the file
 *     cannot be read
 *
 * The file is a table of answers, and a name that has been asked about is
 * in it whether or not the answer was an SDK: a name that located nothing
 * is filed under the same key a name that located something is, and its
 * answer is the manual page path that would have been used.  A later run
 * of the same question therefore finds the name already known, and reports
 * what it knows instead of asking the SDK again -- which is the whole
 * difference between the first such run and the ones after it.
 *
 * The file is the shipped library's own and its framing is not ours to
 * depend on, so what it is asked is the one thing that does not depend on
 * the framing: whether the key is anywhere in it.  Every key is written
 * as text ending in a terminator, so the key is searched for with its
 * terminator, which is what tells one answer's name from the text of
 * another.
 *
 * A --no-cache run has not looked at the file and a -k run has just
 * emptied it, and in both cases there is nothing to find.
 */
static int cache_db_has_key(const char *sdkfield)
{
	char db[PATH_MAX];
	char key[PATH_MAX * 2];
	FILE *fp;
	char *buf;
	size_t keylen;
	long size;
	int found = 0;

	if (cache_bypass_f == 1 || cache_cleared_f == 1)
		return 0;

	if (cache_db_path(db, sizeof(db)) == NULL)
		return 0;

	if (snprintf(key, sizeof(key), "%s|%s|<manpath>", sdkfield, developer_dir) >= (int)sizeof(key))
		return 0;
	keylen = strlen(key) + 1;

	if ((fp = fopen(db, "rb")) == NULL)
		return 0;

	if (fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) > 0 &&
	    size >= (long)keylen) {
		rewind(fp);

		if ((buf = malloc((size_t)size)) != NULL) {
			if (fread(buf, 1, (size_t)size, fp) == (size_t)size &&
			    memmem(buf, (size_t)size, key, keylen) != NULL)
				found = 1;
			free(buf);
		}
	}

	fclose(fp);

	return found;
}

/**
 * @func verbose_note -- one note of the -v trace
 * @arg fmt - what the note says
 *
 * Every note is the same one word of the trace, so that a run can be read
 * by picking the lines out of whatever else shares the terminal.
 */
static void verbose_note(const char *fmt, ...)
{
	va_list ap;

	if (verbose_mode != 1)
		return;

	fprintf(stderr, "xcrun: note: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* The manual page lookup is made once per run and named once per run. */
static int verbose_manpath_reported = 0;

/**
 * @func verbose_manpath_note -- name the manual page lookup that was made
 * @arg sdkfield - the SDK as it is named in the key: a path, or the name
 *     as the user wrote it when no SDK could be located
 * @arg sdkpath - the SDK that resolved, or NULL when none did
 *
 * The key is what the answer is filed under, and it is the same shape for
 * every action that needs a manual page path: the SDK, the developer
 * directory, and the path itself.  A --no-cache run is not in the file at
 * all, so it names nothing.
 */
static void verbose_manpath_note(const char *sdkfield, const char *sdkpath)
{
	char db[PATH_MAX];
	char *manpath;

	if (verbose_manpath_reported)
		return;
	verbose_manpath_reported = 1;

	if (cache_bypass_f == 1)
		return;

	verbose_note("database key is: %s|%s|<manpath>", sdkfield, developer_dir);

	/*
	 * An answer is reported only when there was one to report.  A -k run
	 * has just made sure the file is empty, and a -n run never looks at
	 * it -- both name the key, and neither says that it resolved.
	 */
	if (cache_cleared_f == 1)
		return;

	manpath = manpath_env(developer_dir, (char *)sdkpath);
	if (manpath == NULL)
		return;

	if (cache_db_path(db, sizeof(db)) != NULL)
		verbose_note("lookup resolved in '%s' : '%s'", db, manpath);

	free(manpath);
}

/**
 * @func verbose_toolchain_text -- the TOOLCHAINS a run would use, as text
 * @return: the text, or "" when no toolchain was asked for
 *
 * TOOLCHAINS is the text as it was given, whether that came from --toolchain
 * or from the environment, and a run with neither has none -- even though
 * the toolchain that is searched is the default one.  The name of what is
 * searched and the name of what is exported are not the same thing.
 */
static const char *verbose_toolchain_text(void)
{
	const char *text = requested_toolchain;

	if (text == NULL)
		text = getenv("TOOLCHAINS");

	return text != NULL ? text : "";
}

/**
 * @func verbose_env_note -- name the environment a tool would be given
 * @arg sdkroot - the SDKROOT the tool would see
 * @arg toolchains - the TOOLCHAINS the tool would see
 *
 * The six entries are the ones xcrun has an opinion about, and they are
 * named in a fixed order because the point of the trace is to be read: PATH
 * first because it decides what runs, and the cache last because it is the
 * only one of the six that is not in the environment being described.
 */
static void verbose_env_note(const char *sdkroot, const char *toolchains)
{
	char db[PATH_MAX];
	const char *val;

	val = getenv("PATH");
	verbose_note("PATH = '%s'", val != NULL ? val : "");
	verbose_note("SDKROOT = '%s'", sdkroot != NULL ? sdkroot : "");
	verbose_note("TOOLCHAINS = '%s'", toolchains);
	verbose_note("DEVELOPER_DIR = '%s'", developer_dir != NULL ? developer_dir : "");

	val = getenv("XCODE_DEVELOPER_USR_PATH");
	verbose_note("XCODE_DEVELOPER_USR_PATH = '%s'", val != NULL ? val : "");

	if (cache_db_path(db, sizeof(db)) != NULL)
		verbose_note("xcrun_db = '%s'", db);
}

/**
 * @func xcodebuild_sdk_query -- ask the toolchain where an SDK is
 * @arg arg - the SDK as xcodebuild should see it: a name, or a path
 * @arg query - the SDK item to ask for: Path or PlatformPath
 * @arg voice - whether the resolution is named for the -v trace
 * @return: malloc'd answer, or NULL when the toolchain has none
 *
 * Where toolchains are kept the SDK question is xcodebuild's to answer,
 * and the two answers this library asks for are where the SDK is and what
 * platform it lives in.  The shipped library says so as it works, with the
 * command named first, the environment the resolution is made under next,
 * and the answer the toolchain gives last -- and each of the three is
 * reproduced here because -v is a trace of the run, not a summary.
 *
 * The child is handed the question through the shell, as Apple runs it, so
 * it inherits this stderr: a lookup that fails will have already said what
 * the toolchain says about the SDK it was asked for, in terms Apple
 * controls, and this only reports the shape of the failure on top.  The
 * answer it gives on stdout is what is returned.
 */
static char *xcodebuild_sdk_query(const char *arg, const char *query, int voice)
{
	char *xcodebuild = NULL;
	char *cmd = NULL;
	char *line = NULL;
	size_t linecap = 0;
	FILE *fp;
	char *answer = NULL;

	if (developer_dir == NULL)
		return NULL;

	if (asprintf(&xcodebuild, "%s/usr/bin/xcodebuild", developer_dir) == -1)
		return NULL;

	/*
	 * A Command Line Tools developer directory has no xcodebuild in it
	 * at all, so a toolchain question is not even asked there: the SDK
	 * question is ours to answer, and answers it has none.  The caller
	 * decides what that means.
	 */
	if (access(xcodebuild, (F_OK | X_OK)) == -1) {
		free(xcodebuild);
		return NULL;
	}

	if (asprintf(&cmd, "%s -sdk %s -version %s", xcodebuild, arg, query) == -1) {
		free(xcodebuild);
		return NULL;
	}

	if (voice) {
		verbose_note("looking up SDK with '%s'", cmd);
		verbose_env_note(arg, "");
	}

	if ((fp = popen(cmd, "r")) != NULL) {
		ssize_t len = getline(&line, &linecap, fp);
		int cstatus = pclose(fp);

		if (voice) {
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
				line[--len] = '\0';

			if (len > 0 && WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0) {
				verbose_note("lookup resolved to: '%s'", line);
				answer = strdup(line);
			}
		} else if (len > 0 && WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0) {
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
				line[--len] = '\0';
			if (len > 0)
				answer = strdup(line);
		}
	}

	free(line);
	free(cmd);
	free(xcodebuild);

	/* A resolver that failed has already said why; the caller carries on. */
	return answer;
}

/**
 * @func verbose_tool_key_note -- name the utility lookup that was made
 * @arg name - the utility as the user named it
 * @arg sdkroot - the SDKROOT the lookup was made under
 * @arg toolchains - the TOOLCHAINS the lookup was made under
 *
 * A utility named by path is not looked up at all -- it is the path -- and
 * so has no key.  The trailing separator is the empty last field, and it is
 * not optional: it is the one character that tells a key ending in the
 * developer directory from one that does not.
 */
static void verbose_tool_key_note(const char *name, const char *sdkroot, const char *toolchains)
{
	verbose_note("database key is: %s|%s|%s|%s|", name,
	    sdkroot != NULL ? sdkroot : "", toolchains != NULL ? toolchains : "",
	    developer_dir != NULL ? developer_dir : "");
}

/**
 * @func env_entry_is -- whether an environ entry is one of the named variables
 * @arg entry - an entry of environ, "NAME=value" or "NAME"
 * @arg names - NUL-terminated names to test for
 */
static int env_entry_is(const char *entry, const char *const *names)
{
	const char *eq = strchr(entry, '=');
	size_t len = (eq != NULL) ? (size_t)(eq - entry) : strlen(entry);
	size_t i;

	for (i = 0; names[i] != NULL; i++)
		if (strlen(names[i]) == len && strncmp(entry, names[i], len) == 0)
			return 1;

	return 0;
}

/**
 * @func tool_env -- the whole environment a tool is run with
 * @arg devdir - the developer directory in effect
 * @arg sdk - the SDK in effect, or NULL when none could be located
 * @arg sdk_named - whether an SDK was named on the command line
 * @return: a NULL-terminated environment for execve, or NULL
 *
 * The caller's environment is the tool's environment, with a handful of
 * entries replaced.  It used to be replaced wholesale by a list of five,
 * which left every tool run by this with no TERM, no LANG, no TMPDIR and
 * no DEVELOPER_DIR at all -- a program run through xcrun is not a
 * program run with a scrubbed environment, and the parts of the
 * environment xcrun has an opinion about are only the ones below.
 *
 * What xcrun does set:
 *
 * > SDKROOT is where programs such as clang need to find the SDK.  When
 *   the SDK named cannot be located the name is passed through as given,
 *   which is what Apple does: a tool that reads SDKROOT is told which
 *   SDK was asked for, not handed nothing.
 * > MANPATH is the manual page path, in the order manpath_env lists.
 * > CPATH and LIBRARY_PATH are the host's own include and library
 *   directories, and are set for the macOS SDK alone: they name where
 *   the system headers and libraries are, and no other platform's SDK
 *   means anything by them.
 * > A deployment target, but only where one was asked for on the command
 *   line.  The one in the environment is already inherited, and the
 *   SDK's is a default Apple does not impose unasked.
 *
 * PATH is deliberately not touched.  Apple does not add the developer
 * directory or the toolchain to it, and a tool that needs them is a
 * tool that was not found by looking there.  Neither is LD_LIBRARY_PATH
 * nor TARGET_TRIPLE, which this used to set and Apple does not.
 */
static char **tool_env(const char *devdir, const char *sdk, int sdk_named)
{
	/*
	 * Two of these are always xcrun's: the SDK in effect is xcrun's
	 * answer however it was arrived at, and MANPATH is xcrun's pages
	 * with the caller's after them.
	 */
	static const char *const always[] = { "SDKROOT", "MANPATH", NULL };
	const char *computed[6] = { NULL };
	const char *deployment_target;
	struct stat st;
	const char *name, *inherited;
	char **envp, *manpath, *value;
	size_t kept = 0, total, n = 0, owned = 0;
	int i, c, set_cpaths, set_toolchains, set_targets;

	/*
	 * The rest are xcrun's only where it has an answer of its own to
	 * put there, and the caller's where it has not.  A value already in
	 * the environment is a value the caller asked for, and it stands:
	 * SDKROOT, TOOLCHAINS, a deployment target and the host's own
	 * include and library paths all read the same way, and PATH -- which
	 * is never touched for this reason -- is only the clearest case of
	 * it.  Naming a toolchain replaces the caller's, and asking for a
	 * deployment target hands the variable to xcrun even where the SDK
	 * turns out to name none, which leaves the tool with no target
	 * rather than with the caller's.
	 */
	set_cpaths = (sdk_named == 0 && sdk != NULL &&
	    (name = sdk_dir_name(sdk)) != NULL &&
	    strncmp(name, "MacOSX", sizeof("MacOSX") - 1) == 0);
	set_toolchains = (requested_toolchain != NULL);
	set_targets = (macosx_deployment_target_set == 1 || ios_deployment_target_set == 1);

	deployment_target = NULL;
	if (set_targets && sdk != NULL && stat(sdk, &st) == 0 && S_ISDIR(st.st_mode))
		deployment_target = get_sdk_info(sdk).deployment_target;

	/*
	 * Packed in order and not left with holes: a name list is read up to
	 * its NULL, so an empty slot in the middle would hide every name
	 * after it and quietly leave the caller's value in place for the
	 * very variables xcrun had taken over.
	 */
	c = 0;
	if (set_cpaths) {
		computed[c++] = "CPATH";
		computed[c++] = "LIBRARY_PATH";
	}
	if (set_toolchains)
		computed[c++] = "TOOLCHAINS";
	if (macosx_deployment_target_set == 1)
		computed[c++] = "MACOSX_DEPLOYMENT_TARGET";
	else if (ios_deployment_target_set == 1)
		computed[c++] = "IOS_DEPLOYMENT_TARGET";

	for (i = 0; environ[i] != NULL; i++)
		if (!env_entry_is(environ[i], always) && !env_entry_is(environ[i], computed))
			kept++;

	/* SDKROOT, MANPATH, CPATH, LIBRARY_PATH, TOOLCHAINS and a target */
	total = kept + 7;
	if ((envp = calloc(total + 1, sizeof(char *))) == NULL)
		return NULL;

	for (i = 0; environ[i] != NULL; i++)
		if (!env_entry_is(environ[i], always) && !env_entry_is(environ[i], computed))
			envp[n++] = environ[i];

	/*
	 * Everything before here belongs to environ and is only borrowed;
	 * everything from here on is built here and is ours to free.  The
	 * two are kept in one list because a tool is given one list, and
	 * the split is remembered so that a failure part way through can
	 * hand back only what this function made -- freeing a borrowed
	 * environ entry would be freeing a string the process is still
	 * using.
	 */
	owned = n;

	if (asprintf(&value, "SDKROOT=%s", (sdk != NULL) ? sdk : current_sdk) < 0)
		goto fail;
	envp[n++] = value;

	if ((manpath = manpath_env(devdir, sdk)) != NULL) {
		/*
		 * The caller's pages are kept, and go after ours rather than
		 * instead of them, in the empty slot the list already ends
		 * with -- the trailing colon Apple leaves is where the next
		 * entry goes, and it is a separator for that entry and
		 * nothing more.
		 */
		inherited = getenv("MANPATH");
		if (inherited != NULL && *inherited != '\0') {
			if (asprintf(&value, "MANPATH=%s%s", manpath, inherited) < 0) {
				free(manpath);
				goto fail;
			}
		} else if (asprintf(&value, "MANPATH=%s", manpath) < 0) {
			free(manpath);
			goto fail;
		}
		free(manpath);
		envp[n++] = value;
	} else if ((inherited = getenv("MANPATH")) != NULL && *inherited != '\0') {
		if (asprintf(&value, "MANPATH=%s", inherited) < 0)
			goto fail;
		envp[n++] = value;
	}

	/*
	 * These two name the host's own headers and libraries, and are set
	 * for the SDK in effect when no SDK was named -- which is how Apple
	 * has it.  Name one, and they are not set at all: asked for macOS
	 * and handed the macOS SDK, Apple's env has no CPATH in it either.
	 * A tool that needed them was reached by the default.
	 */
	if (set_cpaths) {
		if (asprintf(&value, "CPATH=%s", "/usr/local/include") < 0)
			goto fail;
		envp[n++] = value;
		if (asprintf(&value, "LIBRARY_PATH=%s", "/usr/local/lib") < 0)
			goto fail;
		envp[n++] = value;
	}

	if (requested_toolchain != NULL) {
		if (asprintf(&value, "TOOLCHAINS=%s", requested_toolchain) < 0)
			goto fail;
		envp[n++] = value;
	}

	/*
	 * Read only where it is wanted, and only from an SDK that is there:
	 * asking a path that does not exist for its settings is an error
	 * that ends the command, and a tool that needs no deployment target
	 * should not have its run ended over one.  A name that resolved to
	 * nothing is the same case and is left alone, the SDK having already
	 * said it is not there.  Where none is read the variable stays out,
	 * having been taken out of the environment above.
	 */
	if (deployment_target != NULL && macosx_deployment_target_set == 1) {
		if (asprintf(&value, "MACOSX_DEPLOYMENT_TARGET=%s", deployment_target) < 0)
			goto fail;
		envp[n++] = value;
	} else if (deployment_target != NULL && ios_deployment_target_set == 1) {
		if (asprintf(&value, "IOS_DEPLOYMENT_TARGET=%s", deployment_target) < 0)
			goto fail;
		envp[n++] = value;
	}

	envp[n] = NULL;
	return envp;

fail:
	for (i = (int)owned; i < (int)n; i++)
		free(envp[i]);
	free(envp);
	return NULL;
}

/**
 * @func call_command -- Execute new process to replace this one.
 * @arg cmd - program's absolute path
 * @arg argc - number of arguments to be passed to new process
 * @arg argv - arguments to be passed to new process
 * @return: -1 on error, otherwise no return
 */
static int call_command(const char *cmd, int argc, char *argv[])
{
	char **envp;
	char *sdk;
	int i;

	/*
	 * An SDK named by path is the SDK in effect, and it stays that one
	 * however far from the installation it is: a path nowhere near this
	 * Xcode is still the SDK a tool is run against, which is why it does
	 * not fall back to the default on failing to be here.
	 */
	if (alternate_sdk_path != NULL)
		sdk = strdup(alternate_sdk_path);
	else
		sdk = sdk_path_or_null(current_sdk);

	envp = tool_env(developer_dir, sdk,
	    (explicit_sdk_mode == 1 || alternate_sdk_path != NULL) ? 1 : 0);
	if (envp == NULL) {
		free(sdk);
		return -1;
	}

	if (logging_mode == 1) {
		logging_printf(stdout, "xcrun: info: invoking command:\n\t\"%s", cmd);
		for (i = 1; i < argc; i++)
			logging_printf(stdout, " %s", argv[i]);
		logging_printf(stdout, "\"\n");
	}

	return execve(cmd, argv, envp);
}

/**
 * @func search_command -- Search a set of directories for a given command
 * @arg name - program's name
 * @arg dirs - set of directories to search, seperated by colons
 * @return: the program's absolute path on success, NULL on failure
 */
static char *search_command(const char *name, char *dirs)
{
	char *cmd = NULL;	/* command's absolute path */
	char *absl_path = NULL;		/* path entry to search */
	char delimiter[2] = ":";	/* delimiter for directories in dirs argument */

	/* Allocate space for the program's absolute path */
	if ((cmd = (char *)malloc(PATH_MAX - 1)) == NULL)
		return NULL;

	/* Search each path entry in dirs until we find our program. */
	absl_path = strtok(dirs, delimiter);
	while (absl_path != NULL) {
		/* Construct our program's absolute path. */
		snprintf(cmd, PATH_MAX - 1, "%s/%s", absl_path, name);

		/* Does it exist? Is it an executable? */
		if (access(cmd, (F_OK | X_OK)) != (-1))
			return cmd;

		/* If not, move onto the next entry.. */
		absl_path = strtok(NULL, delimiter);
	}

	/*
	 * Every entry was looked at and none of them had it.  Returning the
	 * last path tried instead would have the caller try to run a file
	 * that was never there, and report it as an exec failure.
	 */
	free(cmd);

	return NULL;
}

/**
 * @func request_command - Request a program.
 * @arg name -- name of program
 * @arg argv -- arguments to be passed if program found
 * @return: -1 on failed search, 0 on successful search, no return on execute
 */
/*
 * Which toolchain an SDK should be searched alongside.
 *
 * An SDK may name one, but in practice none does -- neither the bundles
 * this tree emits nor Apple's own SDKSettings.plist carries a toolchain
 * key -- so the answer is almost always the toolchain already selected.
 * Passing the empty name straight through is what made any
 * `xcrun --sdk <name> --find <tool>` fail with "'' is not a valid
 * toolchain name" instead of finding the tool.
 */
static char *sdk_toolchain_name(const char *sdkpath)
{
	const char *name = get_sdk_info(sdkpath).toolchain;

	if (name == NULL || *name == '\0')
		name = current_toolchain;

	return strdup((name != NULL) ? name : "");
}

/**
 * @func sdk_from_environment -- the SDK the caller's environment names
 * @arg value - the SDKROOT the caller set, or NULL if it set none
 * @return: malloc'd name or path
 *
 * SDKROOT is the caller's own answer to the question and is taken as it
 * was written.  A path in it is that path, which is the shape
 * "SDKROOT=$(xcrun --show-sdk-path)" hands round to the next command; read
 * as a name instead, it would take a directory's name for an SDK's name
 * and then go looking for an SDK that is not called that.  A name in it is
 * looked up as one.  Nothing is trimmed off either end: the answer is not
 * ours to shorten, and the spelling of a path that is already resolved
 * needs none.
 */
static char *sdk_from_environment(const char *value)
{
	return (value != NULL) ? strdup(value) : default_sdk_name();
}

static int request_command(const char *name, int argc, char *argv[]);

/*
 * A tool can live somewhere our own search does not look -- in a system
 * directory, or a per-user one -- and the shipped xcrun still finds it.  It
 * does that by asking the toolchain's xcodebuild, which knows the full set of
 * places a tool can be installed, and printing whatever path comes back.  The
 * shipped library runs it as
 *
 *   sh -c '<devdir>/usr/bin/xcodebuild -sdk <sdkpath> -find <tool> 2> /dev/null'
 *
 * and reports a failure as
 *
 *   xcrun: error: sh -c '...' failed with exit code <n>: (null) (errno=...)
 *
 * followed by
 *
 *   xcrun: error: unable to find utility "<tool>", not a developer tool or in PATH
 *
 * which is why the command is spelled out in the message: it is the only
 * record of what was actually tried.  We do the same rather than inventing a
 * longer list of directories, because the toolchain's answer is the one that
 * matches what the toolchain will actually run.
 *
 * @return: malloc'd path, or NULL when the tool cannot be found
 */
static char *xcodebuild_find_path(const char *name)
{
	char *xcodebuild = NULL;
	char *sdk = NULL;
	char *cmd = NULL;
	char *shcmd = NULL;
	char *line = NULL;
	char *path = NULL;
	size_t linecap = 0;
	FILE *fp;
	int status;

	if (developer_dir == NULL)
		return NULL;

	if (asprintf(&xcodebuild, "%s/usr/bin/xcodebuild", developer_dir) == -1)
		return NULL;
	if (access(xcodebuild, (F_OK | X_OK)) == -1) {
		free(xcodebuild);
		return NULL;
	}

	/*
	 * -sdk is given the resolved SDK path, not the name, so an explicit
	 * --sdk is honoured the same way it is for the searches above.  A
	 * --sdk that named a path is that path either way, resolved or not:
	 * handing xcodebuild the default instead would answer a question
	 * about the SDK that was asked about with a different one.
	 */
	if (alternate_sdk_path != NULL) {
		if (asprintf(&sdk, "%s", alternate_sdk_path) == -1)
			return NULL;
	} else if ((sdk = sdk_path_or_null(current_sdk)) == NULL) {
		/*
		 * The SDK does not resolve, but xcodebuild is what says so
		 * there, and it wants the name that was asked for rather than
		 * a default standing in for it.  Handing it the name lets it
		 * report the same SDK as the search did.
		 */
		if (asprintf(&sdk, "%s", current_sdk) == -1) {
			free(xcodebuild);
			return NULL;
		}
	}
	if (asprintf(&cmd, "%s -sdk %s -find %s 2> /dev/null", xcodebuild, sdk, name) == -1) {
		free(sdk);
		free(xcodebuild);
		return NULL;
	}

	/* The message quotes the whole thing, so keep the two apart. */
	if (asprintf(&shcmd, "sh -c '%s'", cmd) == -1) {
		free(cmd);
		free(sdk);
		free(xcodebuild);
		return NULL;
	}

	/*
	 * Reported as 0 unless something here sets it, which is what the
	 * shipped library does: it is the exit code that says what went
	 * wrong, and errno only says so when it was the reason.  An SDK
	 * that is not there is such a reason, and is reported as the errno
	 * from looking for it rather than as nothing at all.
	 */
	errno = validate_directory_path(sdk) == (-1) ? ENOENT : 0;

	if ((fp = popen(shcmd, "r")) != NULL) {
		ssize_t len = getline(&line, &linecap, fp);
		int cstatus = pclose(fp);

		/*
		 * The shipped library names the lookup it is about to run and
		 * the answer it got back, both as notes: the command is what
		 * was actually tried, and the answer is where the toolchain
		 * placed the tool.  Both are exact, so they are part of the
		 * -v trace rather than of the error reporting below.
		 */
		verbose_note("looking up with '%s'", cmd);

		/* xcodebuild exits 70 for a tool it could not find. */
		if (len > 0 && WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0) {
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
				line[--len] = '\0';
			if (len > 0)
				path = strdup(line);
		}
		if (path != NULL) {
			verbose_note("lookup resolved with 'xcodebuild -find' to '%s'", path);
		} else {
			/*
			 * The status is reported as pclose hands it back,
			 * exit code shifted into the high half rather than
			 * unpacked from it: a tool xcodebuild will not name
			 * exits 69 and is reported as 17664.  The errno is
			 * the one from looking, not from running: an SDK
			 * that is not there, or a cache put aside so the
			 * filesystem is what answers, is a lookup made and
			 * failed for want of what it looked for.
			 */
			status = cstatus;
			if (validate_directory_path(sdk) == (-1) ||
			    cache_bypass_f == 1 || cache_cleared_f == 1)
				errno = ENOENT;
			fprintf(stderr, "xcrun: error: sh -c \'%s\' failed with exit code %d: (null) (errno=%s)\n",
			    cmd, status, strerror(errno));
		}
	}

	free(line);
	free(shcmd);
	free(cmd);
	free(sdk);
	free(xcodebuild);

	return path;
}

static int request_command(const char *name, int argc, char *argv[])
{
	char *cmd = NULL;	/* used to hold our command's absolute path */
	char *sdk_env = NULL;	/* used for passing SDKROOT in call_command */
	char *toolch_name = NULL;	/* toolchain name to be used with sdk */
	char *toolchain_env = NULL;	/* used for passing PATH in call_command */
	char *path_env = NULL;	/* the caller's PATH, searched last */
	int narrow_to_sdk_only = 0;	/* an SDK nothing resolves leaves nothing */
	char search_string[PATH_MAX * 1024];	/* our search string */

	/*
	 * If xcrun was called in a multicall state, we still want to specify current_sdk for SDKROOT and
	 * current_toolchain for PATH.
	 */
	if (current_sdk == NULL) {
		sdk_env = getenv("SDKROOT");
		current_sdk = sdk_from_environment(sdk_env);
	}

	if (current_toolchain == NULL) {
		current_toolchain = (char *)malloc(XCRUN_NAME_MAX);
		if ((toolchain_env = getenv("TOOLCHAINS")) != NULL)
			name_from_path(current_toolchain, XCRUN_NAME_MAX, toolchain_env,
				       toolchain_exts);
		else
			current_toolchain = default_toolchain_name();
	}

	/*
	 * Tools come from the toolchain, then the developer dir, then the
	 * caller's PATH.  The order matters: a real installation keeps shims
	 * in the developer dir that stand in for the toolchain's tools, and
	 * Apple resolves the toolchain's copy ahead of them.
	 *
	 * The SDK is deliberately not a source of tools.  Its usr/bin holds
	 * the *-config helper scripts, and those are shadowed by the copies
	 * in /usr/bin -- naming the SDK on the command line does not change
	 * that, so an SDK's own cups-config is never the one that runs.
	 */
	search_string[0] = '\0';

	if (explicit_sdk_mode == 1) {
		/*
		 * The toolchain the SDK belongs to, and only that one.  An
		 * SDK naming no toolchain of its own falls back to the
		 * default, but one that is not there at all brings none:
		 * a developer dir that has a toolchain is not a reason to
		 * answer for an SDK that was asked for by name.
		 *
		 * An SDK that resolves to nothing is not the end of it.  The
		 * SDK has said so; the tool being asked for is named by
		 * something else entirely, and the shipped library goes on
		 * looking for it, so this leaves the search empty rather
		 * than stopping, and the tools further down still answer.
		 *
	 * Where they answer is the other half of it.  A flat Command Line
	 * Tools layout resolves no SDK through a toolchain, so its usr/bin
	 * is there to be found and "xcrun --sdk bogus clang" still runs
	 * clang.  Where toolchains are kept, the tool for a named SDK is
	 * xcodebuild's to locate, and an SDK it cannot find is a toolchain
	 * it has nothing to search: so there the developer dir's tools and
	 * the caller's PATH are taken out of the search as well, leaving
	 * xcodebuild to answer and fail.  A name that is a path is not
	 * searched for at all, so it never reaches here.
	 */
		char *sdk = sdk_path_or_null(current_sdk);

		if (sdk != NULL) {
			toolch_name = sdk_toolchain_name(sdk);
			if (toolch_name == NULL && validate_directory_path(sdk) != (-1))
				toolch_name = current_toolchain;
			free(sdk);
		}
		if (toolch_name != NULL)
			sprintf(search_string, "%s/usr/bin", get_toolchain_path(toolch_name));
		else if (devdir_has_toolchains())
			narrow_to_sdk_only = 1;
	} else if (explicit_toolchain_mode == 1) {
		sprintf(search_string, "%s/usr/bin", get_toolchain_path(current_toolchain));
	} else if (alternate_sdk_path != NULL && validate_directory_path(alternate_sdk_path) == (-1)) {
		/*
		 * An SDK named by a path that is not there brings no toolchain
		 * with it, and where toolchains are kept that takes the
		 * developer dir's own tools and the caller's PATH out of the
		 * search too: the SDK asked for is what names the tools, so
		 * none is left but xcodebuild, and the tool is reported
		 * missing.  The flat Command Line Tools layout resolves no SDK
		 * through a toolchain, so there its usr/bin still answers.
		 */
		if (devdir_has_toolchains())
			narrow_to_sdk_only = 1;
	} else if (alternate_sdk_path != NULL && test_sdk_authenticity(alternate_sdk_path) == 1) {
		/* We also want to append an associated toolchain if this is really an SDK folder. */
		toolch_name = sdk_toolchain_name(alternate_sdk_path);
		if (toolch_name != NULL)
			sprintf(search_string, "%s/usr/bin", get_toolchain_path(toolch_name));
		else
			sprintf(search_string, "%s/usr/bin", get_toolchain_path(current_toolchain));
	} else if (alternate_toolchain_path != NULL) {
		sprintf(search_string, "%s/usr/bin", alternate_toolchain_path);
	} else {
		sprintf(search_string, "%s/usr/bin", get_toolchain_path(current_toolchain));
	}

	/* Then the developer dir, which is where the non-toolchain tools live. */
	if (!narrow_to_sdk_only) {
		if (search_string[0] != '\0')
			strncat(search_string, ":", (sizeof(search_string) - strlen(search_string) - 1));
		sprintf((search_string + strlen(search_string)), "%s/usr/bin", developer_dir);
	}

	/* And last of all the caller's PATH, which is where a system tool stays a system tool. */
	if (!narrow_to_sdk_only && (path_env = getenv("PATH")) != NULL) {
		strncat(search_string, ":", (sizeof(search_string) - strlen(search_string) - 1));
		strncat(search_string, path_env, (sizeof(search_string) - strlen(search_string) - 1));
	}

	/* Search each path entry in search_string until we find our program. */
	{
		char *sdk = sdk_path_or_null(current_sdk);
		const char *sdkfield = sdk != NULL ? sdk : current_sdk;

		verbose_tool_key_note(name, sdkfield, verbose_toolchain_text());
		free(sdk);
	}

	/*
	 * Where the utility is asked for and the developer dir keeps
	 * toolchains, the answer is the toolchain's to give -- but only
	 * when nothing second-hand is left to answer with: a warm lookup
	 * has a learned answer in the database and names it as such, and it
	 * is a bypassed or cleared cache that sends the question to
	 * xcodebuild directly.  xcrun reports what it was asked and where
	 * the answer came from, and asks no higher.  The searches above are
	 * what a flat Command Line Tools layout, or a tool being run, makes.
	 */
	if (finding_mode == 1 && devdir_has_toolchains() &&
	    (cache_bypass_f == 1 || cache_cleared_f == 1))
		return -1;

	if ((cmd = search_command(name, search_string)) != NULL) {
		/*
		 * The lookup found the tool, and a working copy of the
		 * cache says so.  A -k run has just emptied the file and a
		 * -n run never looks at it, so neither reports an answer;
		 * a -k run in particular would be naming what it itself
		 * erased, which is not an answer at all.
		 */
		if (cache_bypass_f != 1 && cache_cleared_f != 1) {
			char db[PATH_MAX];

			if (cache_db_path(db, sizeof(db)) != NULL)
				verbose_note("lookup resolved in '%s' : '%s'",
				    db, cmd);
		}
		if (finding_mode == 1) {
			fprintf(stdout, "%s\n", cmd);
			free(cmd);
			return 0;
		}
		call_command(cmd, argc, argv);
		/* NOREACH */
		fprintf(stderr, "xcrun: error: can't exec \'%s\' (errno=%s)\n", cmd, strerror(errno));
		return -1;
	}

	/*
	 * None of the directories had it.  Nothing is said here: the caller
	 * goes on to ask the toolchain, and if that cannot name it either,
	 * the message it prints is the one that explains the failure.  A
	 * line here would only be a guess at which of the two happened.
	 */
	return -1;
}

static int xcrun_parse_args(int argc, char *argv[]);

/**
 * @func xcrun_parse_xcrun_args -- hand the user's arguments to the parser
 * @arg argc - number of arguments passed by user, not counting xcrun itself
 * @arg argv - array of arguments passed by user, already without xcrun
 * @arg progname - the name to report in messages
 * @return: 0 (or none) on success, 1 on failure
 *
 * Our callers hand us argc - 1 and argv + 1, so argv[0] is the first
 * argument the user gave and no program name is present at all.  The parser
 * wants the usual C shape, with argv[0] naming the program, because
 * getopt_long_only unconditionally skips argv[0] when it looks for
 * options.  Hand it the shifted array unchanged and the first option is
 * mistaken for the program name, nothing is parsed, and the option itself
 * is then run as a tool.  Put the name back rather than rewrite the parser
 * to count differently from every other getopt caller.
 */
static int xcrun_parse_xcrun_args(int argc, char *argv[], char *progname)
{
	char **args;
	int i, retval;

	args = (char **)malloc(sizeof(char *) * (size_t)(argc + 2));
	if (args == NULL) {
		fprintf(stderr, "xcrun: error: failed to allocate memory\n");
		exit(1);
	}

	args[0] = progname;
	for (i = 0; i < argc; i++)
		args[i + 1] = argv[i];
	args[argc + 1] = NULL;

	retval = xcrun_parse_args(argc + 1, args);

	free(args);

	return retval;
}

/**
 * @func xcrun_parse_args -- xcrun's option parsing and dispatch proper
 * @arg argc - number of arguments passed by user
 * @arg argv - array of arguments passed by user
 * @return: 0 (or none) on success, 1 on failure
 *
 * The body of what used to be xcrun_main.  It is split out because the
 * exported xcrun_main is a different function: that one is handed the tool
 * name and developer directory by libxcselect, and does not return.
 */
static int xcrun_parse_args(int argc, char *argv[])
{
	int ch;
	int retval = 1;
	int optindex = 0;
	int argc_offset = 0;
	char *sdk = NULL;
	char *toolchain = NULL;
	char *tool_called = NULL;

	char *sdk_env = NULL;
	char *toolchain_env = NULL;

	static enum show_kind show_kind = SHOW_NONE;
	static int show_toolchain_f, verbose_f, log_f, find_f, run_f, nocache_f, killcache_f, version_f, sdk_f, toolchain_f;
	show_toolchain_f = verbose_f = log_f = find_f = run_f = nocache_f = killcache_f = version_f = sdk_f = toolchain_f = 0;

	/* Supported options */
	static struct option options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, &version_f, 1 },
		{ "verbose", no_argument, 0, 'v' },
		{ "sdk", required_argument, &sdk_f, 1 },
		{ "toolchain", required_argument, &toolchain_f, 1 },
		{ "log", no_argument, 0, 'l' },
		{ "find", required_argument, 0, 'f' },
		{ "run", required_argument, 0, 'r' },
		{ "no-cache", no_argument, 0, 'n' },
		{ "kill-cache", no_argument, 0, 'k' },
		{ "show-sdk-path", no_argument, 0, SHOW_SDK_PATH },
		{ "show-sdk-version", no_argument, 0, SHOW_SDK_VERSION },
		{ "show-sdk-build-version", no_argument, 0, SHOW_SDK_BUILD_VERSION },
		{ "show-sdk-platform-path", no_argument, 0, SHOW_SDK_PLATFORM_PATH },
		{ "show-sdk-platform-version", no_argument, 0, SHOW_SDK_PLATFORM_VERSION },
		{ "show-toolchain-path", no_argument, 0, 'T' },
		{ NULL, 0, 0, 0 }
	};

	/* Nothing to do is misuse of the tool, and is answered as such. */
	if (argc < 2)
		usage(EX_USAGE);

	/* Only parse arguments if they are given */
	if (*(*(argv + 1)) == '-') {
		if (strcmp(argv[1], "-") == 0 || strcmp(argv[1], "--") == 0)
			usage(EX_USAGE);
		while ((ch = getopt_long_only(argc, argv, ":+hvlr:f:nk", options, &optindex)) != (-1)) {
			switch (ch) {
				case 'h':
					usage(0);
					break;
				case 'v':
					verbose_f = 1;
					verbose_mode = 1;
					break;
				case 'l':
					log_f = 1;
					break;
				case 'r':
					run_f = 1;
					tool_called = strdup(optarg);
					++argc_offset;
					break;
				case 'f':
					find_f = 1;
					tool_called = strdup(optarg);
					++argc_offset;
					break;
				case SHOW_SDK_PATH:
				case SHOW_SDK_VERSION:
				case SHOW_SDK_BUILD_VERSION:
				case SHOW_SDK_PLATFORM_PATH:
				case SHOW_SDK_PLATFORM_VERSION:
					show_kind = (enum show_kind)ch;
					break;
				case 'T':
					show_toolchain_f = 1;
					break;
				case 'n':
					nocache_f = 1;
					cache_bypass_f = 1;
					break;
				case 'k':
					/*
					 * Clearing the file is not the same as not
					 * using it: a -k run still goes through the
					 * cache afterwards and still names it.  Only
					 * -n takes it out of the run, so only -n
					 * takes the notes that describe it away.
					 */
					killcache_f = 1;
					cache_cleared_f = 1;
					break;
				case 0: /* long-only options */
					switch (optindex) {
						case 1: /* --version */
							break;
						case 3: /* --sdk */
							if (*optarg != '-') {
								++argc_offset;
								sdk = optarg;
							/* we support absolute paths and short names */
								if (*sdk == '/') {
									/*
									 * A path is taken as the SDK it is
									 * whether or not anything can be read
									 * from it.  One that is not there is
									 * said so where a toolchain would
									 * have resolved it, which is where
									 * the flat layout has nothing to
									 * resolve through and stays quiet.
									 */
									if (validate_directory_path(sdk) == (-1) &&
									    devdir_has_toolchains())
										fprintf(stderr, "xcrun: error: Failed to determine realpath of '%s' (errno=%s)\n",
										    sdk, strerror(errno));
									alternate_sdk_path = sdk;
								} else {
									/*
									 * The name is kept as it was written, and
									 * the extension is not taken off it:
									 * "macosx26.5.sdk" is a directory's
									 * spelling of itself and is no more an
									 * SDK's name than "MacOSX26.5" is, so
									 * stripping it would answer a question
									 * nobody asked and hand back an SDK
									 * that Apple says cannot be located.
									 */
									current_sdk = (char *)malloc(XCRUN_NAME_MAX);
									explicit_sdk_mode = 1;
									snprintf(current_sdk, XCRUN_NAME_MAX, "%s", sdk);
								}
							} else {
								fprintf(stderr, "xcrun: error: sdk flag requires an argument.\n");
								exit(1);
							}
							break;
						case 4: /* --toolchain */
							if (*optarg != '-') {
								++argc_offset;
								toolchain = optarg;
								requested_toolchain = toolchain;
							/* we support absolute paths and short names */
							if (*toolchain == '/') {
								/* Said once, then used anyway, as for --sdk. */
								alternate_toolchain_path = toolchain;
							} else {
									current_toolchain = (char *)malloc(XCRUN_NAME_MAX);
									explicit_toolchain_mode = 1;
									stripext(current_toolchain, XCRUN_NAME_MAX, toolchain,
							    toolchain_exts);
								}
							} else {
								fprintf(stderr, "xcrun: error: toolchain flag requires an argument.\n");
								exit(1);
							}
							break;
						case 10: /* --show-sdk-path */
							break;
						case 11: /* --show-sdk-version */
							break;
						case 12: /* --snow-sdk-target-triple */
							break;
						case 13: /* --show-sdk-toolchain-path */
							break;
						case 14: /* --show-sdk-toolchain-version */
							break;
					}
					break;
			case ':':
				/*
				 * An option that takes a tool name asks for
				 * the tool by name of what it is missing, not
				 * by the option that wanted it: -f, -r and --sdk
				 * are all a missing utility, and xcrun says so
				 * the same way for each.
				 */
				if (optopt == 'f' || optopt == 'r') {
					fprintf(stderr, "%s: error: no utility name specified\n",
					    progname);
					usage(EX_USAGE);
				}
				fprintf(stderr, "%s: error: argument to '%s'"
				    " is missing\n", progname,
				    argv[optind - 1]);
				usage(EX_USAGE);
				case '?':
				default:
					fprintf(stderr, "%s: error: unrecognized"
					    " option: %s\n", progname,
					    argv[optind - 1]);
					usage(EX_USAGE);
					break;
			}

			++argc_offset;

			/*
			 * A run hands everything after its tool to the tool,
			 * so the line is finished there and this stops reading
			 * it.
			 *
			 * A find does not.  It never runs the tool at all, so
			 * nothing after the tool belongs to it, and the rest of
			 * the line is xcrun's to read: "--find clang --sdk
			 * macosx" is one command with an option on each side
			 * of the tool name, and both are read.  Apple counts
			 * an SDK named after the tool in a find and says so if
			 * it names nothing, and does not for a run.
			 */
			if (ch == 'r')
				break;
		}
	} else { /* We are just executing a program. */
		tool_called = strdup(argv[1]);
		++argc_offset;
	}

	/* The last non-option argument may be the command called. */
	if (optind < argc && ((run_f == 0 || find_f == 0) && tool_called == NULL)) {
		tool_called = strdup(argv[optind++]);
		++argc_offset;
	}

	/*
	 * An option that means "do something to this tool" is incomplete
	 * without the tool, and is answered before any SDK is worked out.
	 * -k is the exception: it only ever clears the cache, so it is
	 * complete on its own.  A --show-* is not about a tool either, and
	 * it is the one thing that is still printed when it arrived with
	 * -v, -l or -n instead of with a tool -- the trace of the lookup is
	 * what the combination asks for.
	 */
	if (tool_called == NULL && show_kind == SHOW_NONE &&
	    show_toolchain_f == 0 &&
	    (verbose_f == 1 || log_f == 1 || nocache_f == 1 ||
	     find_f == 1 || run_f == 1))
		no_utility_named();

	/* Print version? */
	if (version_f == 1)
		version();

	/* If our SDK and/or Toolchain hasn't been specified, fall back to environment or defaults. */
	if (current_sdk == NULL) {
		sdk_env = getenv("SDKROOT");
		current_sdk = sdk_from_environment(sdk_env);
	}

	if (current_toolchain == NULL) {
		current_toolchain = (char *)malloc(XCRUN_NAME_MAX);
		if ((toolchain_env = getenv("TOOLCHAINS")) != NULL)
			name_from_path(current_toolchain, XCRUN_NAME_MAX, toolchain_env,
				       toolchain_exts);
		else
			current_toolchain = default_toolchain_name();
	}

	/*
	 * Print the one thing that was asked for.  Which of the --show-*
	 * options that is was decided while reading them, so the answer
	 * given is the last one asked for and not a fixed order.
	 */
	switch (show_kind) {
		case SHOW_SDK_PATH:
			printf("%s\n", require_sdk_path(current_sdk, "Path"));
			exit(0);

		case SHOW_SDK_VERSION:
			/* Apple's xcrun prints the bare version and nothing else. */
			printf("%s\n", get_sdk_info(require_sdk_path(current_sdk, "SDKVersion")).version);
			exit(0);

		case SHOW_SDK_BUILD_VERSION: {
			char *build = xt_sdk_build_version(require_sdk_path(current_sdk, "ProductBuildVersion"));

			if (build == NULL) {
				fprintf(stderr, "xcrun: error: no build version for SDK '%s'.\n",
					current_sdk);
				exit(1);
			}

			printf("%s\n", build);
			exit(0);
		}

		case SHOW_SDK_PLATFORM_PATH: {
			char *sdk = require_sdk_path(current_sdk, "PlatformPath");
			char *platform = xt_sdk_platform_path(sdk);

			if (platform == NULL) {
				/*
				 * The Command Line Tools keep their SDKs outside
				 * any platform bundle, so there is no platform to
				 * name -- which is what this says, rather than
				 * complaining about the SDK.  The SDK it did
				 * consult is named either way, and naming it
				 * resolves the SDK a second time, which is the
				 * other half of what Apple prints here.
				 */
				fprintf(stderr, "xcrun: error: unable to lookup item 'PlatformPath' from command line tools installation\n");
				fprintf(stderr, "xcrun: error: unable to lookup item 'PlatformPath' in SDK '%s'\n", get_sdk_path(current_sdk));
				exit(1);
			}

			printf("%s\n", platform);
			exit(0);
		}

		case SHOW_SDK_PLATFORM_VERSION: {
			char *sdk = require_sdk_path(current_sdk, "PlatformVersion");
			char *platform = xt_sdk_platform_path(sdk);
			char *version = NULL;

			if (platform != NULL) {
				/*
				 * Apple's platform records the same version under
				 * "Version" and CFBundleShortVersionString; take
				 * the one that names itself, and accept the bundle
				 * key from a platform that carries only that.
				 */
				if ((version = xt_platform_setting(platform, "Version")) == NULL)
					version = xt_platform_setting(platform,
					    "CFBundleShortVersionString");
			}

			if (version == NULL) {
				fprintf(stderr, "xcrun: error: unable to lookup item 'PlatformVersion' from command line tools installation\n");
				fprintf(stderr, "xcrun: error: unable to lookup item 'PlatformVersion' in SDK '%s'\n", get_sdk_path(current_sdk));
				exit(1);
			}

			printf("%s\n", version);
			exit(0);
		}

		case SHOW_NONE:
			if (show_toolchain_f == 1) {
				/*
				 * The toolchain path is found the same way a
				 * manual page path is: under the SDK and the
				 * developer directory.  The toolchain resolves
				 * against the same key, so the trace names the
				 * same two things it names for the manual page
				 * path, with the toolchain where the manpath was.
				 * A run that never looks at the cache has nothing
				 * to key on either, and says nothing.
				 */
				char *sdk = sdk_path_or_null(current_sdk);
				const char *sdkfield = sdk != NULL ? sdk : current_sdk;
				char db[PATH_MAX];
				const char *tc;

				verbose_manpath_note(sdkfield, sdk);
				if (cache_bypass_f != 1) {
					verbose_note("database key is: %s|%s|<toolchain_dir>",
					    sdkfield, developer_dir);
					tc = get_toolchain_path(current_toolchain);
					if (cache_cleared_f != 1 &&
					    cache_db_path(db, sizeof(db)) != NULL)
						verbose_note("lookup resolved in '%s' : '%s'",
						    db, tc);
				} else
					tc = get_toolchain_path(current_toolchain);
				printf("%s\n", tc);
				free(sdk);
				exit(0);
			}
			break;
	}

	/*
	 * The cache flags are taken and nothing is said about them: a lookup
	 * here is made by running the toolchain's own xcodebuild, so there is
	 * no cache of our own for them to bypass or clear, and complaining
	 * about a flag the help advertises helps nobody.
	 */

	/* Turn on verbose mode?  Already done in the parser: a --show-* has
	 * to be able to trace the SDK lookup it does before this point. */

	/* Turn on logging mode? */
	if (log_f == 1)
		logging_mode = 1;

	/* Before we continue, double check if we have a tool to call. */
	if (tool_called == NULL) {
		/* -k asked only for the cache to be cleared; that is done. */
		if (killcache_f == 1)
			exit(0);
		no_utility_named();
	}

	/*
	 * A utility is about to be searched for or run, which is where the
	 * rest of the trace comes from: the manual page lookup every run
	 * makes, the environment the utility would be given, and the utility
	 * itself.  It is all said here, before the search, so that a run
	 * that finds nothing has said the same thing as one that does.
	 */
	{
		char *sdk = sdk_path_or_null(current_sdk);
		const char *sdkfield = sdk != NULL ? sdk : current_sdk;

		verbose_manpath_note(sdkfield, sdk);
		verbose_env_note(sdkfield, verbose_toolchain_text());
		verbose_note("xcrun via %s (xcrun)", tool_called);
		free(sdk);
	}

	/* Search for program? */
	if (find_f == 1) {
		char *path;

		/*
		 * A path was named, so it is what gets printed -- whether or not
		 * it is there.  Finding out is the caller's to do, not this
		 * tool's: --find answers where a name would resolve to, and a
		 * name the caller already spelled out has nowhere else to go.
		 */
		if ((path = named_path(tool_called)) != NULL) {
			/*
			 * The SDK is resolved before the path is printed, all
			 * the same.  It is asked for once per command, and an
			 * --sdk that names nothing has said so by now whether
			 * or not the tool turns out to be a path: the SDK was
			 * asked for, and what became of the answer belongs to
			 * the SDK, not to how the tool happened to be spelled.
			 * A tool run needs none of this, which is why only
			 * this one asks.
			 */
			free(sdk_path_or_null(current_sdk));
			fprintf(stdout, "%s\n", path);
			free(path);
			retval = 0;
		} else {
			finding_mode = 1;
			if (request_command(tool_called, 0, NULL) != -1)
				retval = 0;
			else if ((path = xcodebuild_find_path(tool_called)) != NULL) {
				fprintf(stdout, "%s\n", path);
				free(path);
				retval = 0;
			} else
				no_such_utility(tool_called);
		}
	}

	/* Search and execute program. (default behavior) */
	if (find_f != 1) {
		char *path;

		if ((path = named_path(tool_called)) != NULL) {
			/*
			 * Nothing was searched, so a file that will not run is
			 * the only thing that can go wrong, and it is reported
			 * as the exec that failed rather than as a missing
			 * utility: the caller gave us the file, so it exists
			 * or it does not, and no directory list is involved.
			 * EX_OSERR rather than the EX_OSFILE above, which is
			 * for a name no search could place.
			 */
			call_command(path, argc - argc_offset, argv + argc_offset);
			/* NOREACH */
			fprintf(stderr, "xcrun: error: can't exec \'%s\' (errno=%s)\n",
			    path, strerror(errno));
			exit(EX_OSERR);
		}
		else if (request_command(tool_called, argc - argc_offset, argv + argc_offset) != -1)
			retval = -1; /* NOREACH */
		/*
		 * Not in any of the directories searched above, but the
		 * toolchain may still know of it, and the shipped xcrun
		 * runs it from wherever that answer points.
		 */
		else if ((path = xcodebuild_find_path(tool_called)) != NULL) {
			call_command(path, argc - argc_offset, argv + argc_offset);
			/* NOREACH */
			fprintf(stderr, "xcrun: error: can't exec \'%s\' (errno=%s)\n", path, strerror(errno));
			exit(1);
		}
		else {
			/*
			 * A caller that is not xcrun itself decides what
			 * an unknown tool means, and has said so by
			 * registering a handler for it.  libxcrun owns
			 * the exit either way.
			 */
			if (unknown_utility_handler != NULL) {
				unknown_utility_handler(tool_called);
				exit(1);
			}
			no_such_utility(tool_called);
		}
	}

		/* The shipped library exits here rather than returning. */
		exit(retval);
}

/**
 * @func xcrun_get_version -- the version this library reports
 * @return: our own version string
 *
 * A literal return, in the shipped library as well.
 */
const char *
xcrun_get_version(void)
{
	return TOOL_VERSION;
}

/**
 * @func xcrun_set_unknown_utility_handler -- take over unknown tool names
 * @arg handler - called with the tool name we could not find
 *
 * Registered by libxcselect whenever the process is not xcrun itself.
 * The shipped library Block_copies what it is given and stores it in a
 * global, which is what lets the caller's block -- a file-scope literal
 * on their side, and ours above -- stay valid after the setter returns.
 */
void
xcrun_set_unknown_utility_handler(void (^handler)(const char *))
{
	unknown_utility_handler = Block_copy(handler);
}

/**
 * @func xcrun_iter_manpaths -- report a developer directory's manual pages
 * @arg devdir - the developer directory to describe
 * @arg sysroot - the sysroot in effect, or NULL
 * @arg iter - called once per directory, in the order they should be used
 *
 * This is how libxcselect builds the list behind
 * xcselect_get_manpaths().  It asks the library that ships with the
 * developer directory rather than guessing at its layout, so a toolchain
 * that has moved or added pages of its own is described accurately.  When
 * no such library can be asked, libxcselect falls back to a list it holds
 * itself; this is the list that fallback names, and the order matters.
 *
 * sysroot is accepted and ignored for the same reason the shipped library
 * accepts it: the pages are those of the active developer directory, not
 * of the sysroot.
 */
void
xcrun_iter_manpaths(const char *devdir, const char *sysroot,
    void (^iter)(const char *))
{
	char path[PATH_MAX];
	char *sdk;
	struct stat st;

	if (devdir == NULL || iter == NULL)
		return;

	/*
	 * sysroot is accepted and ignored, as the shipped library accepts
	 * and ignores it: asked with a sysroot of "/", with an SDK path, and
	 * with none, it returns the same list every time.  The pages are
	 * those of the active developer directory.
	 */
	(void)sysroot;

	/*
	 * The SDK's own pages come first, then the platform's, then the
	 * developer directory's, then the default toolchain's.  Asked
	 * against a full Xcode this reproduces the shipped list exactly:
	 *
	 *	<dev>/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/share/man
	 *	<dev>/Platforms/MacOSX.platform/usr/share/man
	 *	<dev>/usr/share/man
	 *	<dev>/Toolchains/XcodeDefault.xctoolchain/usr/share/man
	 *
	 * Note the SDK is MacOSX.sdk, the same one the shipped library picks
	 * as the default SDK -- see cltools_lookup_sdk_by_key in
	 * local/xcselect.md.  Naming the default rather than looking one up
	 * by name is the point: a name lookup resolves away to whichever
	 * bundle carries the canonical name, and in a stock Xcode that is
	 * MacOSX26.5.sdk.
	 */
	if ((sdk = xt_default_sdk_path(devdir)) != NULL) {
		snprintf(path, sizeof(path), "%s/usr/share/man", sdk);
		iter(path);
		free(sdk);
	}

	/*
	 * A Command Line Tools directory has no platform bundle, and the
	 * shipped library does not invent one: asked against it, the list is
	 * the SDK's, the developer directory's and the toolchain's, with the
	 * platform's pages absent rather than named and missing.
	 *
	 * What decides it is the platform bundle and not the pages inside
	 * it.  Apple names the platform's pages for a full Xcode whether or
	 * not that Xcode ships any -- the man pages themselves are not
	 * installed, and the path is named anyway, because the platform is
	 * what the path belongs to and the platform is there.  Testing for
	 * the directory instead leaves the platform's entry out of the list
	 * on exactly the installations that have a platform, and in.
	 */
	snprintf(path, sizeof(path), "%s/Platforms/MacOSX.platform", devdir);
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(path, sizeof(path), "%s/Platforms/MacOSX.platform/usr/share/man",
		    devdir);
		iter(path);
	}

	snprintf(path, sizeof(path), "%s/usr/share/man", devdir);
	iter(path);

	/*
	 * Also named whether or not it exists, which is how the shipped
	 * library behaves against the Command Line Tools directory: the
	 * toolchain's pages are always reported, so a toolchain that has not
	 * been laid down yet is still where its pages will go.
	 */
	snprintf(path, sizeof(path), "%s/Toolchains/%s/usr/share/man", devdir,
	    "XcodeDefault.xctoolchain");
	iter(path);
}

/**
 * @func xcrun_main_entry -- dispatch a call, under whichever name we were
 *	invoked
 * @arg tool_name - name the process was invoked under, without any path
 * @arg argc - argument count
 * @arg argv - argument vector
 * @arg devdir - resolved developer directory, or NULL to find one
 * @return: the exit status, which xcrun_main turns into an exit
 *
 * The multicall dispatch that used to be main().  Called as xcrun we parse
 * options; called as xcrun_log, xcrun_verbose or xcrun_nocache we take the
 * same options with one turned on for us; called as anything else we are a
 * tool being asked to run another one, which is the xcselect path where a
 * multicall binary named xcrun hands us work.
 */
static int
xcrun_main_entry(const char *tool_name, int argc, char *argv[],
    const char *devdir)
{
	int retval = 1;
	int call_state;
	char *this_tool = NULL;

  	/*
  	 * A NULL name is how the caller says "be xcrun": the system shim
  	 * passes NULL whenever the process was really invoked as xcrun, and
  	 * libxcselect passes it when it wants the xcrun interface.  Any
  	 * other name is a tool to run.
  	 *
  	 * That is the whole of the dispatch, and it is narrower than it
  	 * looks.  The shipped library does not consult a table of names and
  	 * does not treat "xcrun" or "xcrun_log" as special: asked for those,
  	 * it reports them as utilities it cannot find, while a NULL gets
  	 * the xcrun interface.  The table that used to be here turned the
  	 * caller's own name into behaviour, which meant a process invoked
  	 * under the name "xcrun" and one invoked under "xcrun_log" behaved
  	 * differently from the shipped library in both cases.
  	 */
  	if (tool_name == NULL) {
  		progname = "xcrun";
  		call_state = 1;
  		this_tool = dup_basename(progname);
  	} else {
  		/* Strip out any path name that may have been passed in. */
  		this_tool = dup_basename(tool_name);
  		progname = this_tool;
  		call_state = -1;
  	}

	/* Execute based on the state that we were called in. */
	switch (call_state) {
		case 1: /* be xcrun */
			retval = xcrun_parse_xcrun_args(argc, argv, progname);
			break;
		default: /* called as a tool name */
			/* Locate and execute the command */
			if (request_command(this_tool, argc, argv) != -1)
				retval = -1; /* NOREACH */
			else {
				fprintf(stderr, "xcrun: error: failed to execute command \'%s\'.\n", this_tool);
				if (unknown_utility_handler != NULL)
					unknown_utility_handler(this_tool);
				exit(1);
			}
			break;
	}

	(void)devdir;
	return retval;
}

/**
 * @func xcrun_main -- the developer directory's xcrun, as the shim reaches it
 * @arg tool_name - the name the caller was invoked under, without any path
 * @arg argc - number of arguments passed by user
 * @arg argv - array of arguments passed by user
 * @arg devdir - the developer directory the caller resolved, or NULL to
 *	find one ourselves
 *
 * libxcselect is what calls this, having already established which
 * developer directory is active, so devdir is normally handed down rather
 * than searched for.  Nobody hands one down when we are run directly, so
 * NULL still means "look", the way a bare xcrun has to.
 *
 * Does not return.  The caller treats a return as an unexpected exit.
 */
void
xcrun_main(char *tool_name, int argc, char *argv[], const char *devdir)
{
	/*
	 * libxcselect is the only thing that calls us, and it has already
	 * established which developer directory is active, so it always
	 * hands one down.  The shipped library is more forgiving: it
	 * carries a search of its own, which is why it does not need
	 * libxcselect and why it does not link it.  Duplicating that search
	 * here would put the caller inside the thing it called, so a NULL
	 * is reported rather than guessed at.
	 */
	if (devdir == NULL) {
		fprintf(stderr, "xcrun: error: no developer directory given.\n");
		exit(1);
	}

	developer_dir = strdup(devdir);

	exit(xcrun_main_entry(tool_name, argc, argv, devdir));
}
