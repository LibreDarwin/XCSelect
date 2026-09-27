/* xcrun - clone of apple's xcode xcrun utility
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
#include <sys/stat.h>
#include <sys/types.h>

#include "ini.h"
#include <stdbool.h>

#include "devpath.h"
#include "xcselect.h"
#include "sdkpath.h"

/* General stuff */
#define TOOL_VERSION "1.0.0"
#define SDK_CFG ".xcdev.dat"
#ifndef XCRUN_DEFAULT_CFG
#define XCRUN_DEFAULT_CFG "/usr/local/etc/xcrun.ini"
#endif
#ifndef XCRUN_DEFAULT_DEVELOPER_DIR
#define XCRUN_DEFAULT_DEVELOPER_DIR "/Library/Developer/CommandLineTools"
#endif

/* Toolchain configuration struct */
typedef struct {
	const char *name;
	const char *version;
} toolchain_config;

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
 */
static void usage(void)
{
	fprintf(stderr,
		"Usage: %s [options] <tool name> ... arguments ...\n"
		"\n"
		"Find and execute the named command line tool from the active developer directory.\n"
		"\n"
		"The active developer directory can be set using `xcode-select`, or via the\n"
		"DEVELOPER_DIR environment variable.\n"
		"\n"
		"Options:\n"
		"  -h, --help                   show this help message and exit\n"
		"  --version                    show the xcrun version\n"
		"  -v, --verbose                show verbose logging output\n"
		"  --sdk <sdk name>             find the tool for the given SDK name\n"
		"  --toolchain <name>           find the tool for the given toolchain\n"
		"  -l, --log                    show commands to be executed (with --run)\n"
		"  -f, --find                   only find and print the tool path\n"
		"  -r, --run                    find and execute the tool (the default behavior)\n"
#if 0
		"  -n, --no-cache               do not use the lookup cache (not implemented yet - does nothing)\n"
		"  -k, --kill-cache             invalidate all existing cache entries (not implemented yet - does nothing)\n"
#endif
		"  --show-sdk-path              show selected SDK install path\n"
		"  --show-sdk-platform-path     show selected SDK platform path\n"
		"  --show-sdk-platform-version  show selected SDK platform version\n"
		"  --show-sdk-version           show selected SDK version\n"
		"  --show-sdk-target-triple     show selected SDK target triple\n"
		"  --show-sdk-toolchain-path    show selected SDK toolchain path\n"
		"  --show-sdk-toolchain-version show selected SDK toolchain version\n\n"
		, progname);

	exit(0);
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
 * @func toolchain_cfg_handler -- handler used to process toolchain info.ini contents
 * @arg user - ini user pointer (see ini.h)
 * @arg section - ini section name (see ini.h)
 * @arg name - ini variable name (see ini.h)
 * @arg value - ini variable value (see ini.h)
 * @return: 1 on success, 0 on failure
 */
static int toolchain_cfg_handler(void *user, const char *section, const char *name, const char *value)
{
	toolchain_config *config = (toolchain_config *)user;

	if (MATCH_INI_STON("TOOLCHAIN", "name"))
		config->name = strdup(value);
	else if (MATCH_INI_STON("TOOLCHAIN", "version"))
		config->version = strdup(value);
	else
		return 0;

	return 1;
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
 * @func get_toolchain_info -- fetch config info from a toolchain's info.ini
 * @arg path - path to toolchain's info.ini
 * @return: struct containing toolchain config info
 */
static toolchain_config get_toolchain_info(const char *path)
{
	toolchain_config config;
	char info_path[PATH_MAX];
	char *identifier;

	memset(&config, 0, sizeof(config));

	/*
	 * A stock toolchain describes itself with ToolchainInfo.plist, whose
	 * only key is a reverse-DNS Identifier.  Take the last component as
	 * the name, which is what our callers print, and fall through to
	 * info.ini for a toolchain in the older layout.
	 */
	if ((identifier = xt_toolchain_identifier(path)) != NULL) {
		char *last = strrchr(identifier, '.');

		config.name = strdup(last != NULL ? last + 1 : identifier);
		config.version = strdup("");
		free(identifier);
		return config;
	}

	snprintf(info_path, sizeof(info_path), "%s/info.ini", path);

	if (ini_parse(info_path, toolchain_cfg_handler, &config) != (-1))
		return config;

	fprintf(stderr, "xcrun: error: failed to retrieve toolchain info from '%s'. (errno=%s)\n",
		path, strerror(errno));
	exit(1);
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
/* Defined below, once get_developer_path() is available. */
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
 * @func get_developer_path -- retrieve current developer path
 * @return: string of current path on success, NULL string on failure
 */
static char *get_developer_path(void)
{
	FILE *fp = NULL;
	char devpath[PATH_MAX - 1];
	char *pathtocfg = NULL;
	char cfg_path[PATH_MAX];
	char *value = NULL;

	verbose_printf(stdout, "xcrun: info: attempting to retrieve developer path from DEVELOPER_DIR...\n");

	if ((value = getenv("DEVELOPER_DIR")) != NULL) {
		verbose_printf(stdout, "xcrun: info: using developer path \'%s\' from DEVELOPER_DIR.\n", value);
		return value;
	}

	bzero(devpath, (PATH_MAX - 1));

	verbose_printf(stdout, "xcrun: info: attempting to retrieve developer path from configuration cache...\n");
	if ((pathtocfg = getenv("HOME")) == NULL) {
		fprintf(stderr, "xcrun: error: failed to read HOME variable.\n");
		return NULL;
	}

	/*
	 * Bounded, and correctly sized.  The allocation here was
	 * strlen(HOME) + sizeof(SDK_CFG), which leaves no room for the
	 * separator the very next line writes -- one byte past the end of
	 * the buffer, every time this ran.
	 */
	if (snprintf(cfg_path, sizeof(cfg_path), "%s/%s", pathtocfg,
	    SDK_CFG) >= (int)sizeof(cfg_path)) {
		fprintf(stderr, "%s: error: configuration path too long.\n",
		    progname);
		return NULL;
	}

	if ((fp = fopen(cfg_path, "r")) != NULL) {
		fseek(fp, 0, SEEK_SET);
		(void)fread(devpath, (PATH_MAX - 1), 1, fp);
		value = devpath;
		fclose(fp);
	} else {
		struct stat st;


		/*
		 * No per-user selection.  libxcselect answers the rest --
		 * the link xcode-select -s writes, this binary's own
		 * Developer directory, then the system defaults.  Apple's
		 * xcrun asks the same library, which is what stops it and
		 * xcode-select from disagreeing about where the tools are.
		 */
		static char devdir[PATH_MAX];
		bool cltools = false, missing = false, invalid = false;

		(void)st;

		if (xcselect_get_developer_dir_path(devdir, sizeof(devdir),
		    &cltools, &missing, &invalid)) {
			verbose_printf(stdout, "xcrun: info: using developer"
			    " path \'%s\' from libxcselect%s.\n", devdir,
			    cltools ? " (command line tools)" : "");
			if (invalid)
				verbose_printf(stdout, "xcrun: info: note: that"
				    " directory does not exist.\n");
			return devdir;
		}

		if (missing)
			fprintf(stderr, "xcrun: error: no developer directory"
			    " has been selected.\n");
		else
			fprintf(stderr, "xcrun: error: unable to determine the"
			    " developer directory.\n");
		return NULL;
	}

	verbose_printf(stdout, "xcrun: info: using developer path \'%s\' from configuration cache.\n", value);


	return value;
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

	if ((devpath = get_developer_path()) != NULL) {
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
 * @func xcrun_main -- xcrun's main routine
 * @arg argc - number of arguments passed by user
 * @arg argv - array of arguments passed by user
 * @return: 0 (or none) on success, 1 on failure
 */
static int xcrun_main(int argc, char *argv[])
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

	static int help_f, verbose_f, log_f, find_f, run_f, nocache_f, killcache_f, version_f, sdk_f, toolchain_f, ssdkp_f, ssdkv_f, ssdkpp_f, ssdktt_f, ssdkpv_f, ssdkplatp_f, ssdkplatv_f;
	help_f = verbose_f = log_f = find_f = run_f = nocache_f = killcache_f = version_f = sdk_f = toolchain_f = ssdkp_f = ssdkv_f = ssdkpp_f = ssdktt_f = ssdkpv_f = ssdkplatp_f = ssdkplatv_f = 0;

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
		{ "show-sdk-path", no_argument, &ssdkp_f, 1 },
		{ "show-sdk-version", no_argument, &ssdkv_f, 1 },
		{ "show-sdk-target-triple", no_argument, &ssdktt_f, 1},
		{ "show-sdk-toolchain-path", no_argument, &ssdkpp_f, 1 },
		{ "show-sdk-toolchain-version", no_argument, &ssdkpv_f, 1 },
		{ "show-sdk-platform-path", no_argument, &ssdkplatp_f, 1 },
		{ "show-sdk-platform-version", no_argument, &ssdkplatv_f, 1 },
		{ NULL, 0, 0, 0 }
	};

	/* Print help if nothing is specified */
	if (argc < 2)
		usage();

	/* Only parse arguments if they are given */
	if (*(*(argv + 1)) == '-') {
		if (strcmp(argv[1], "-") == 0 || strcmp(argv[1], "--") == 0)
			usage();
		while ((ch = getopt_long_only(argc, argv, "+hvlr:f:nk", options, &optindex)) != (-1)) {
			switch (ch) {
				case 'h':
					help_f = 1;
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
				case '?':
				default:
					help_f = 1;
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

	/* Print help? */
	if (help_f == 1 || argc < 2)
		usage();

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

	/* Show SDK path? */
	if (ssdkp_f == 1) {
		printf("%s\n", get_sdk_path(current_sdk));
		exit(0);
	}

	/* Show SDK platform path? */
	if (ssdkplatp_f == 1) {
		char *sdk = get_sdk_path(current_sdk);
		char *platform = xt_sdk_platform_path(sdk);

		if (platform == NULL) {
			fprintf(stderr, "xcrun: error: SDK \'%s\' is not inside a platform.\n",
				current_sdk);
			exit(1);
		}

		printf("%s\n", platform);
		exit(0);
	}

	/* Show SDK platform version? */
	if (ssdkplatv_f == 1) {
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
			fprintf(stderr, "xcrun: error: no platform version for SDK \'%s\'.\n",
				current_sdk);
			exit(1);
		}

		printf("%s\n", version);
		exit(0);
	}

	/* Show SDK version? */
	if (ssdkv_f == 1) {
		/* Apple's xcrun prints the bare version and nothing else. */
		printf("%s\n", get_sdk_info(get_sdk_path(current_sdk)).version);
		exit(0);
	}

	/* Show SDK toolchain path? */
	if (ssdkpp_f == 1) {
		printf("%s\n", get_toolchain_path(current_toolchain));
		exit(0);
	}

	/* Show SDK toolchain version? */
	if (ssdkpv_f == 1) {
		printf("%s SDK Toolchain version %s (%s)\n", get_sdk_info(get_sdk_path(current_sdk)).name, get_toolchain_info(get_toolchain_path(current_toolchain)).version, get_toolchain_info(get_toolchain_path(current_toolchain)).name);
		exit(0);
	}

	/* Show SDK target triple ? */
	if (ssdktt_f == 1) {
		printf("%s\n", get_target_triple(current_sdk));
		exit(0);
	}

	/* Clear the lookup cache? */
	if (killcache_f == 1)
		fprintf(stderr, "xcrun: warning: --kill-cache not supported.\n");

	/* Don't use the lookup cache? */
	if (nocache_f == 1)
		fprintf(stderr, "xcrun: warning: --no-cache not supported.\n");

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
			fprintf(stderr, "xcrun: error: failed to execute command \'%s\'. aborting.\n", tool_called);
			exit(1);
		}
	}

	return retval;
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

int main(int argc, char *argv[])
{
	int retval = 1;
	int call_state;
	char *this_tool = NULL;

	/* Strip out any path name that may have been passed into argv[0] */
	this_tool = dup_basename(argv[0]);
	progname = this_tool;

	/* Get our developer dir */
	developer_dir = get_developer_path();

	/* Check if we are being treated as a multi-call binary. */
	call_state = get_multicall_state(this_tool, multicall_tool_names, 4);

	/* Execute based on the state that we were called in. */
	switch (call_state) {
		case 1: /* xcrun */
			retval = xcrun_main(argc, argv);
			break;
		case 2: /* xcrun_log */
			logging_mode = 1;
			retval = xcrun_main(argc, argv);
			break;
		case 3: /* xcrun_verbose */
			verbose_mode = 1;
			retval = xcrun_main(argc, argv);
			break;
		case 4: /* xcrun_nocache */
			retval = xcrun_main(argc, argv);
			break;
		case -1:
		default: /* called as tool name */
			/* Locate and execute the command */
			if (request_command(this_tool, argc, argv) != -1)
				retval = -1; /* NOREACH */
			else {
				fprintf(stderr, "xcrun: error: failed to execute command \'%s\'. aborting.\n", this_tool);
				exit(1);
			}
			break;
	}

	return retval;
}
