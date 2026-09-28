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
#define TOOL_VERSION "1.0.0"
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

/* Ways that this tool may be called */
static const char *multicall_tool_names[4] = {
	"xcrun",
	"xcrun_log",
	"xcrun_verbose",
	"xcrun_nocache"
};

/* Our program's name as called by the user */
static char *progname;

/* Longest SDK or toolchain name we keep. */
#define XCRUN_NAME_MAX 255

/*
 * Copy a name, dropping a bundle suffix if it carries one, so that both
 * "MacOSX" and "MacOSX.sdk" name the same SDK.
 *
 * Only a known suffix, and only at the end.  A name is full of dots that
 * carry meaning: cutting at the first one turns "macosx26.5" into
 * "macosx26", which matches no SDK at all, and "MacOSX.Internal" into
 * "MacOSX" -- which matches the public SDK, so asking for the internal
 * one would quietly get the wrong SDK instead of an error.
 */
static const char *const sdk_exts[] = { ".sdk", NULL };
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
 * @func verbose_printf -- Print output to fp in verbose mode.
 * @arg fp - pointer to file (file, stderr, or stdio)
 * @arg str - string to print
 * @arg ... - additional arguments used
 */
static void verbose_printf(FILE *fp, const char *str, ...)
{
	va_list args;

	if (verbose_mode == 1) {
		va_start(args, str);
		vfprintf(fp, str, args);
		va_end(args);
	}
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
 * @return: 0 on success, -1 on failure
 */
static int validate_directory_path(const char *dir)
{
	struct stat fstat;
	int retval = -1;

	if (stat(dir, &fstat) != 0)
		fprintf(stderr, "xcrun: error: unable to validate path \'%s\' (errno=%s)\n", dir, strerror(errno));
	else {
		if (S_ISDIR(fstat.st_mode) == 0)
			fprintf(stderr, "xcrun: error: \'%s\' is not a valid path\n", dir);
		else
			retval = 0;
	}

	return retval;
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

	if ((name = xt_first_sdk_name(developer_dir)) != NULL) {
		verbose_printf(stdout, "xcrun: info: no configured sdk; using \'%s\'.\n", name);
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
 * toolchain carries, and the one our own bundles emit.
 *
 * @return: toolchain name
 */
static char *default_toolchain_name(void)
{
	default_config config = get_default_info(default_cfg_path());
	char *path;

	if (config.toolchain != NULL && *config.toolchain != '\0')
		return strdup(config.toolchain);

	if ((path = xt_find_toolchain(developer_dir, "XcodeDefault")) != NULL) {
		free(path);
		return strdup("XcodeDefault");
	}

	fprintf(stderr, "xcrun: error: no toolchain configured and none found in '%s'.\n",
		developer_dir != NULL ? developer_dir : "(unset)");
	exit(1);
}

/**
 * @func get_toolchain_path -- Return the specified toolchain path
 * @arg name - name of the toolchain
 * @return: absolute path of toolchain on success, exit on failure
 */
static char *get_toolchain_path(const char *name)
{
	char *path = NULL;

	if (developer_dir == NULL) {
		fprintf(stderr, "xcrun: error: failed to retrieve developer path, do you have it set?\n");
		exit(1);
	}

	/* Apple's <name>.xctoolchain first, then the older <name>.toolchain. */
	if ((path = xt_find_toolchain(developer_dir, name)) != NULL)
		return path;

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
			verbose_printf(stdout, "xcrun: info: using configuration \'%s\'.\n", path);
			return path;
		}
	}

	return XCRUN_DEFAULT_CFG;
}

/**
 * @func get_sdk_path -- Return the specified sdk path
 * @arg name - name of the sdk
 * @return: absolute path of sdk on success, exit on failure
 */
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
	 */
	if ((path = xt_find_sdk(developer_dir, name)) != NULL)
		return path;

	fprintf(stderr, "xcrun: error: \'%s\' is not a valid sdk name in \'%s\'.\n",
		name, developer_dir);
	exit(1);
}

/**
 * @func parse_target_triple -- Generate target triple by parsing iOS/MacOSX version and cpu architecture
 * @arg triple - buffer to place the target triple
 * @arg ver - Mac OSX or iOS version
 * @arg arch - Mac OSX or iOS cpu architecture
 */
static void parse_target_triple(char *triple, const char *ver, const char *arch)
{
	int where = 1;
	int xx, yy, zz, ch, kern_ver;

	if (ver == NULL)
		return;

	xx = yy = zz = 0;

	do {
		ch = (int)*ver;

		switch (ch) {
			case '9':
			case '8':
			case '7':
			case '6':
			case '5':
			case '4':
			case '3':
			case '2':
			case '1':
			case '0':
				{
					switch (where) {
						case 1: /* major */
							xx *= 10;
							xx += (ch - '0');
							break;
						case 2: /* minor */
							yy *= 10;
							yy += (ch - '0');
							break;
						case 3: /* patch */
							zz *= 10;
							zz += (ch - '0');
						default:
							break;
					}
					break;
				}
			case '.':
			default:
				where++;
				break;
		}
	} while (*ver++ != '\0');

	switch (xx) {
		case 10:
			kern_ver = (yy + 4);
			break;
		case 9:
		case 8:
			kern_ver = 14;
			break;
		case 7:
			kern_ver = 14;
			break;
		case 6:
			kern_ver = 13;
			break;
		case 5:
			kern_ver = 11;
			break;
		case 4:
			{
				if (yy <= 2)
					kern_ver = 10;
				else
					kern_ver = 11;
				break;
			}
		case 3:
			kern_ver = 10;
			break;
		case 2:
			kern_ver = 9;
			break;
		case 1:
		default:
			kern_ver = 9;
			break;
	}

	sprintf(triple, "%s-apple-darwin%d", arch, kern_ver);

	return;
}

/**
 * @func get_target_triple -- get the target triple for the current sdk.
 * @arg current_sdk - specified sdk (ignored if TARGET_TRIPLE env variable is set)
 * @return: target triple string or NULL on error
 */
static char *get_target_triple(const char *current_sdk)
{
	char *triple = NULL;
	char *default_arch = NULL;
	char *deployment_target = NULL;

	if ((triple = getenv("TARGET_TRIPLE")) != NULL)
		return triple;
	else {
		triple = (char *)malloc(64);

		if ((default_arch = strdup(get_sdk_info(get_sdk_path(current_sdk)).default_arch)) == NULL)
			return NULL;

		if ((deployment_target = strdup(get_sdk_info(get_sdk_path(current_sdk)).deployment_target)) == NULL)
			return NULL;

		parse_target_triple(triple, deployment_target, default_arch);

		return triple;
	}
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
	int i, n = 0;
	char *envp[8];
	char *target_triple = NULL;
	const char *deployment_target = NULL;
	const char *path_env = NULL;
	const char *home_env = NULL;

	/*
	 * Pass SDKROOT, PATH, HOME, LD_LIBRARY_PATH, TARGET_TRIPLE, and MACOSX_DEPLOYMENT_TARGET to the called program's environment.
	 *
	 * > SDKROOT is used for when programs such as clang need to know the location of the sdk.
	 * > PATH is used for when programs such as clang need to call on another program (such as the linker).
	 * > HOME is used for recursive calls to xcrun (such as when xcrun calls a script calling xcrun ect).
	 * > LD_LIBRARY_PATH is used for when tools needs to access libraries that are specific to the toolchain.
	 * > TARGET_TRIPLE is used for clang/clang++ cross compilation when building on a foreign host.
	 * > {MACOSX|IOS}_DEPLOYMENT_TARGET is used for tools like ld that need to set the minimum compatibility
	 *   version number for a linked binary.
	 *
	 * Each entry is allocated to fit.  These strings concatenate whole
	 * paths -- PATH in particular appends the caller's entire PATH to two
	 * absolute directories -- and a fixed PATH_MAX buffer overflows on any
	 * deeply nested developer directory, which _FORTIFY_SOURCE turns into
	 * a SIGTRAP.  Entries that have no value are left out rather than
	 * emitted uninitialised; envp must stay NUL-terminated, so nothing may
	 * be skipped in the middle.
	 */
	memset(envp, 0, sizeof(envp));

	path_env = getenv("PATH");
	home_env = getenv("HOME");

	if (asprintf(&envp[n], "SDKROOT=%s", get_sdk_path(current_sdk)) != (-1))
		n++;
	if (asprintf(&envp[n], "PATH=%s/usr/bin:%s/usr/bin:%s", developer_dir,
		     get_toolchain_path(current_toolchain),
		     (path_env != NULL) ? path_env : "") != (-1))
		n++;
	if (asprintf(&envp[n], "LD_LIBRARY_PATH=%s/usr/lib",
		     get_toolchain_path(current_toolchain)) != (-1))
		n++;
	if (home_env != NULL && asprintf(&envp[n], "HOME=%s", home_env) != (-1))
		n++;

	if ((target_triple = get_target_triple(current_sdk)) != NULL) {
		if (asprintf(&envp[n], "TARGET_TRIPLE=%s", target_triple) != (-1))
			n++;
	} else
		verbose_printf(stdout, "xcrun: info: no target triple information for %s.sdk.\n", current_sdk);

	if ((deployment_target = getenv("IOS_DEPLOYMENT_TARGET")) != NULL) {
		if (asprintf(&envp[n], "IOS_DEPLOYMENT_TARGET=%s", deployment_target) != (-1))
			n++;
	} else if ((deployment_target = getenv("MACOSX_DEPLOYMENT_TARGET")) != NULL) {
		if (asprintf(&envp[n], "MACOSX_DEPLOYMENT_TARGET=%s", deployment_target) != (-1))
			n++;
	} else {
		/* Use the deployment target info that is provided by the SDK. */
		deployment_target = get_sdk_info(get_sdk_path(current_sdk)).deployment_target;
		if (deployment_target != NULL) {
			if (macosx_deployment_target_set == 1) {
				if (asprintf(&envp[n], "MACOSX_DEPLOYMENT_TARGET=%s", deployment_target) != (-1))
					n++;
			} else if (ios_deployment_target_set == 1) {
				if (asprintf(&envp[n], "IOS_DEPLOYMENT_TARGET=%s", deployment_target) != (-1))
					n++;
			}
		}
	}

	envp[n] = NULL;

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
	cmd = (char *)malloc(PATH_MAX - 1);

	/* Search each path entry in dirs until we find our program. */
	absl_path = strtok(dirs, delimiter);
	while (absl_path != NULL) {
		verbose_printf(stdout, "xcrun: info: checking directory \'%s\' for command \'%s\'...\n", absl_path, name);

		/* Construct our program's absolute path. */
		sprintf(cmd, "%s/%s", absl_path, name);

		/* Does it exist? Is it an executable? */
		if (access(cmd, (F_OK | X_OK)) != (-1)) {
			verbose_printf(stdout, "xcrun: info: found command's absolute path: \'%s\'\n", cmd);
			break;
		}

		/* If not, move onto the next entry.. */
		absl_path = strtok(NULL, delimiter);
	}

	return cmd;
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

static int request_command(const char *name, int argc, char *argv[])
{
	char *cmd = NULL;	/* used to hold our command's absolute path */
	char *sdk_env = NULL;	/* used for passing SDKROOT in call_command */
	char *toolch_name = NULL;	/* toolchain name to be used with sdk */
	char *toolchain_env = NULL;	/* used for passing PATH in call_command */
	char search_string[PATH_MAX * 1024];	/* our search string */

	/*
	 * If xcrun was called in a multicall state, we still want to specify current_sdk for SDKROOT and
	 * current_toolchain for PATH.
	 */
	if (current_sdk == NULL) {
		current_sdk = (char *)malloc(XCRUN_NAME_MAX);
		if ((sdk_env = getenv("SDKROOT")) != NULL)
			name_from_path(current_sdk, XCRUN_NAME_MAX, sdk_env, sdk_exts);
		else
			current_sdk = default_sdk_name();
	}

	if (current_toolchain == NULL) {
		current_toolchain = (char *)malloc(XCRUN_NAME_MAX);
		if ((toolchain_env = getenv("TOOLCHAINS")) != NULL)
			name_from_path(current_toolchain, XCRUN_NAME_MAX, toolchain_env,
				       toolchain_exts);
		else
			current_toolchain = default_toolchain_name();
	}

	/* No matter the circumstance, search the developer dir. */
	sprintf(search_string, "%s/usr/bin:", developer_dir);

	/* If we implicitly specified an sdk, search the sdk and it's associated toolchain. */
	if (explicit_sdk_mode == 1) {
		toolch_name = sdk_toolchain_name(get_sdk_path(current_sdk));
		sprintf((search_string + strlen(search_string)), "%s/usr/bin:%s/usr/bin", get_sdk_path(current_sdk), get_toolchain_path(toolch_name));
		goto do_search;
	}

	/* If we implicitly specified a toolchain, only search the toolchain. */
	if (explicit_toolchain_mode == 1) {
		sprintf((search_string + strlen(search_string)), "%s/usr/bin", get_toolchain_path(current_toolchain));
		goto do_search;
	}

	/* If we explicitly specified an SDK, append it to the search string. */
	if (alternate_sdk_path != NULL) {
		sprintf((search_string + strlen(search_string)), "%s/usr/bin:", alternate_sdk_path);
		/* We also want to append an associated toolchain if this is really an SDK folder. */
		if (test_sdk_authenticity(alternate_sdk_path) == 1) {
			toolch_name = sdk_toolchain_name(alternate_sdk_path);
			sprintf((search_string + strlen(search_string)), "%s/usr/bin", get_toolchain_path(toolch_name));
			/* We now have a toolchain, so skip to search. */
			goto do_search;
		}
	}

	/* If we explicitly specified a toolchain, append it to the search string. */
	if (alternate_toolchain_path != NULL)
		sprintf((search_string + strlen(search_string)), "%s/usr/bin", alternate_toolchain_path);

	/* By default, we search our developer dir, our default sdk, and our default toolchain only. */
	if (explicit_sdk_mode == 0 && explicit_toolchain_mode == 0 && alternate_toolchain_path == NULL && alternate_sdk_path == NULL)
		sprintf((search_string + strlen(search_string)), "%s/usr/bin:%s/usr/bin", get_sdk_path(current_sdk), get_toolchain_path(current_toolchain));

	/* Search each path entry in search_string until we find our program. */
do_search:
	if ((cmd = search_command(name, search_string)) != NULL) {
		if (finding_mode == 1) {
			if (access(cmd, (F_OK | X_OK)) != (-1)) {
				fprintf(stdout, "%s\n", cmd);
				free(cmd);
				return 0;
			} else
				return -1;
		} else {
			call_command(cmd, argc, argv);
			/* NOREACH */
			fprintf(stderr, "xcrun: error: can't exec \'%s\' (errno=%s)\n", cmd, strerror(errno));
			return -1;
		}
	}

	/* We have searched everywhere, but we haven't found our program. State why. */
	fprintf(stderr, "xcrun: error: can't stat \'%s\' (errno=%s)\n", name, strerror(errno));

	return -1;
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
					break;
				case 'l':
					log_f = 1;
					break;
				case 'r':
					run_f = 1;
					tool_called = dup_basename(optarg);
					++argc_offset;
					break;
				case 'f':
					find_f = 1;
					tool_called = dup_basename(optarg);
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
					break;
				case 'k':
					killcache_f = 1;
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
									if (validate_directory_path(sdk) != (-1))
										alternate_sdk_path = sdk;
									else
										exit(1);
								} else {
									current_sdk = (char *)malloc(XCRUN_NAME_MAX);
									explicit_sdk_mode = 1;
									stripext(current_sdk, XCRUN_NAME_MAX, sdk, sdk_exts);
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
								/* we support absolute paths and short names */
								if (*toolchain == '/') {
									if (validate_directory_path(toolchain) != (-1))
										alternate_toolchain_path = toolchain;
									else
										exit(1);
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

			/* We don't want to parse any more arguments after these are set. */
			if (ch == 'f' || ch == 'r')
				break;
		}
	} else { /* We are just executing a program. */
		tool_called = dup_basename(argv[1]);
		++argc_offset;
	}

	/* The last non-option argument may be the command called. */
	if (optind < argc && ((run_f == 0 || find_f == 0) && tool_called == NULL)) {
		tool_called = dup_basename(argv[optind++]);
		++argc_offset;
	}

	/* Don't continue if we are missing arguments. */
	if ((verbose_f == 1 || log_f == 1) && tool_called == NULL) {
		fprintf(stderr, "xcrun: error: specified arguments require -r or -f arguments.\n");
		exit(1);
	}

	/* Print version? */
	if (version_f == 1)
		version();

	/* If our SDK and/or Toolchain hasn't been specified, fall back to environment or defaults. */
	if (current_sdk == NULL) {
		current_sdk = (char *)malloc(XCRUN_NAME_MAX);
		if ((sdk_env = getenv("SDKROOT")) != NULL)
			name_from_path(current_sdk, XCRUN_NAME_MAX, sdk_env, sdk_exts);
		else
			current_sdk = default_sdk_name();
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
			printf("%s\n", get_sdk_path(current_sdk));
			exit(0);

		case SHOW_SDK_VERSION:
			/* Apple's xcrun prints the bare version and nothing else. */
			printf("%s\n", get_sdk_info(get_sdk_path(current_sdk)).version);
			exit(0);

		case SHOW_SDK_BUILD_VERSION: {
			char *build = xt_sdk_build_version(get_sdk_path(current_sdk));

			if (build == NULL) {
				fprintf(stderr, "xcrun: error: no build version for SDK '%s'.\n",
					current_sdk);
				exit(1);
			}

			printf("%s\n", build);
			exit(0);
		}

		case SHOW_SDK_PLATFORM_PATH: {
			char *platform = xt_sdk_platform_path(get_sdk_path(current_sdk));

			if (platform == NULL) {
				fprintf(stderr, "xcrun: error: SDK '%s' is not inside a platform.\n",
					current_sdk);
				exit(1);
			}

			printf("%s\n", platform);
			exit(0);
		}

		case SHOW_SDK_PLATFORM_VERSION: {
			char *platform = xt_sdk_platform_path(get_sdk_path(current_sdk));
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
				fprintf(stderr, "xcrun: error: no platform version for SDK '%s'.\n",
					current_sdk);
				exit(1);
			}

			printf("%s\n", version);
			exit(0);
		}

		case SHOW_NONE:
			if (show_toolchain_f == 1) {
				printf("%s\n", get_toolchain_path(current_toolchain));
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

	/* Turn on verbose mode? */
	if (verbose_f == 1)
		verbose_mode = 1;

	/* Turn on logging mode? */
	if (log_f == 1)
		logging_mode = 1;

	/* Before we continue, double check if we have a tool to call. */
	if (tool_called == NULL) {
		fprintf(stderr, "xcrun: error: no tool specified.\n");
		exit(1);
	}

	/* Search for program? */
	if (find_f == 1) {
		finding_mode = 1;
		if (request_command(tool_called, 0, NULL) != -1)
			retval = 0;
		else {
			fprintf(stderr, "xcrun: error: unable to locate command \'%s\' (errno=%s)\n", tool_called, strerror(errno));
			exit(1);
		}
	}

	/* Search and execute program. (default behavior) */
	if (find_f != 1) {
			if (request_command(tool_called, (argc - argc_offset),  (argv += ((argc - argc_offset) - (argc - argc_offset) + (argc_offset)))) != -1)
				retval = -1; /* NOREACH */
			else {
				fprintf(stderr, "xcrun: error: failed to execute command \'%s\'.\n", tool_called);
				/*
				 * A caller that is not xcrun itself decides what
				 * an unknown tool means, and has said so by
				 * registering a handler for it.  libxcrun owns
				 * the exit either way.
				 */
				if (unknown_utility_handler != NULL)
					unknown_utility_handler(tool_called);
				else
					fprintf(stderr, "xcrun: error: aborting.\n");
				exit(1);
			}
		}

		/* The shipped library exits here rather than returning. */
		exit(retval);
}

/**
 * @func get_multicall_state -- Return a number that is associated to a given multicall state.
 * @arg cmd - command that binary is being called
 * @arg state - char array containing a set of possible "multicall states"
 * @arg state_size - number of elements in state array
 * @return: a number from 1 to state_size (first enrty to last entry found in state array), or -1 if one isn't found
 */
static int get_multicall_state(const char *cmd, const char *state[], int state_size)
{
	int i;

	for (i = 0; i < (state_size - 1); i++) {
		if (strcmp(cmd, state[i]) == 0)
			return (i + 1);
	}

	return -1;
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
	 * local/xcselect.md.  Naming the default rather than the canonical
	 * version is the point: our xt_first_sdk_name currently prefers the
	 * bundle named for its canonical name, which resolves to
	 * MacOSX26.5.sdk and is the remaining --show-sdk-path difference.
	 */
	if ((sdk = xt_find_sdk(devdir, "MacOSX")) != NULL) {
		snprintf(path, sizeof(path), "%s/usr/share/man", sdk);
		iter(path);
		free(sdk);
	}

	/*
	 * A Command Line Tools directory has no platform bundle, and the
	 * shipped library does not invent one: asked against it, the list is
	 * the SDK's, the developer directory's and the toolchain's, with the
	 * platform's pages absent rather than named and missing.  So this
	 * one is named only when it is really there.
	 */
	snprintf(path, sizeof(path), "%s/Platforms/MacOSX.platform/usr/share/man",
	    devdir);
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		iter(path);

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

	/* Strip out any path name that may have been passed into argv[0] */
	this_tool = dup_basename(tool_name);
	progname = this_tool;

	/* Check if we are being treated as a multi-call binary. */
	call_state = get_multicall_state(this_tool, multicall_tool_names, 4);

	/* Execute based on the state that we were called in. */
	switch (call_state) {
		case 1: /* xcrun */
			retval = xcrun_parse_args(argc, argv);
			break;
		case 2: /* xcrun_log */
			logging_mode = 1;
			retval = xcrun_parse_args(argc, argv);
			break;
		case 3: /* xcrun_verbose */
			verbose_mode = 1;
			retval = xcrun_parse_args(argc, argv);
			break;
		case 4: /* xcrun_nocache */
			retval = xcrun_parse_args(argc, argv);
			break;
		case -1:
		default: /* called as tool name */
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
