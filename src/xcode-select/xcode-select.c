/* xcode-select - clone of apple's xcode-select utility
 *
 * Copyright (c) 2013, Brian McKenzie <mckenzba@gmail.com>
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
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xcselect.h"

#define TOOL_VERSION "1.0.0"
#define SDK_CFG ".xcdev.dat"
#ifndef XCRUN_DEFAULT_DEVELOPER_DIR
#define XCRUN_DEFAULT_DEVELOPER_DIR "/Library/Developer/CommandLineTools"
#endif

/*
 * The help, laid out as the tool that ships it lays it out: the
 * description wrapped in the source rather than by a formatter, and the
 * long --install line left as it is written.  It goes to stderr even when
 * it is all that was asked for, and asking for it is not an error.
 */
static const char usage_text[] =
		"Usage: xcode-select [options]\n"
		"\n"
		"Print or change the path to the active developer directory. This directory\n"
		"controls which tools are used for the Xcode command line tools (for example, \n"
		"xcodebuild) as well as the BSD development commands (such as cc and make).\n"
		"\n"
		"Options:\n"
		"  -h, --help                  print this help message and exit\n"
		"  -p, --print-path            print the path of the active developer directory\n"
		"  -s <path>, --switch <path>  set the path for the active developer directory\n"
		"  --install                   open a dialog for installation of the command line developer tools\n"
		"  -v, --version               print the xcode-select version\n"
		"  -r, --reset                 reset to the default command line tools path\n";

/**
 * @func usage -- Print the help, after a complaint when there is one.
 * @arg error - the complaint, or NULL when only the help was asked for
 */
static void usage(const char *error)
{

	if (error != NULL)
		fprintf(stderr, "xcode-select: error: %s\n", error);

	fputs(usage_text, stderr);

	exit((error != NULL) ? 1 : 0);
}

/**
 * @func usage -- Print the tool version.
 */
static void version(void)
{
	fprintf(stdout, "xcode-select version %s\n", TOOL_VERSION);

	exit(0);
}

/**
 * @func validate_directory_path -- validate if requested directory path exists
 * @arg dir - directory to validate
 * @return: 0 on success, 1 on failure
 */
static int validate_directory_path(const char *dir)
{
	struct stat fstat;
	int retval = 1;

	if (stat(dir, &fstat) != 0)
		fprintf(stderr, "xcode-select: error: unable to validate directory \'%s\' (errno=%s)\n", dir, strerror(errno));
	else {
		if (S_ISDIR(fstat.st_mode) == 0)
			fprintf(stderr, "xcode-select: error: \'%s\' is not a directory, please try a different path\n", dir);
		else
			retval = 0;
	}

	return retval;
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

	if ((value = getenv("DEVELOPER_DIR")) != NULL)
		return value;

	memset(devpath, 0, sizeof(devpath));

	if ((pathtocfg = getenv("HOME")) == NULL) {
		fprintf(stderr, "xcode-select: error: failed to read HOME environment variable.\n");
		return NULL;
	}

	/*
	 * Built with snprintf into a bounded buffer.  What stood here
	 * appended to the string getenv() returned -- writing past the end
	 * of the environment's own copy of HOME -- and then strcat'd onto
	 * a malloc'd buffer that had never been initialised, so the
	 * destination length came from whatever the heap happened to hold.
	 */
	if (snprintf(cfg_path, sizeof(cfg_path), "%s/%s", pathtocfg,
	    SDK_CFG) >= (int)sizeof(cfg_path)) {
		fprintf(stderr, "xcode-select: error: configuration path too"
		    " long.\n");
		return NULL;
	}

	if ((fp = fopen(cfg_path, "r")) != NULL) {
		fseek(fp, SEEK_SET, 0);
		(void)fread(devpath, (PATH_MAX - 1), 1, fp);
		value = devpath;
		fclose(fp);
	} else {
		struct stat st;

		/*
		 * No per-user selection.  libxcselect answers the rest --
		 * the symlinks xcode-select -s writes, the data file, then
		 * the system defaults -- which is the library Apple's
		 * xcode-select asks the same question of, so xcrun and this
		 * tool cannot disagree.
		 */
		static char devdir[PATH_MAX];
		bool from_env = false, cltools = false, fallback = false;

		(void)st;

		if (xcselect_get_developer_dir_path(devdir, sizeof(devdir),
		    &from_env, &cltools, &fallback))
			return devdir;

		fprintf(stderr, "xcode-select: error: unable to determine the"
		    " developer directory.\n");
		return NULL;
	}

	return value;
}

/**
 * @func set_developer_path -- set the current developer path
 * @arg path - path to set
 * @return: 0 on success, -1 on failure
 */
static int set_developer_path(const char *path)
{
	FILE *fp = NULL;
	char *pathtocfg = NULL;
	char cfg_path[PATH_MAX];

	if ((pathtocfg = getenv("HOME")) == NULL) {
		fprintf(stderr, "xcode-select: error: failed to read HOME variable.\n");
		return -1;
	}

	if (snprintf(cfg_path, sizeof(cfg_path), "%s/%s", pathtocfg,
	    SDK_CFG) >= (int)sizeof(cfg_path)) {
		fprintf(stderr, "xcode-select: error: configuration path too"
		    " long.\n");
		return -1;
	}

	if ((fp = fopen(cfg_path, "w+")) != NULL) {
		fwrite(path, 1, strlen(path), fp);
		fclose(fp);
	} else {
		fprintf(stderr, "xcode-select: error: unable to open configuration file. (errno=%s)\n", strerror(errno));
		return -1;
	}


	return 0;
}

int main(int argc, char *argv[])
{
	char complaint[PATH_MAX + 32];
	int ch;
	char *path = NULL;

	if (argc < 2)
		usage("no command option given");

	/* -h is answered where it is read, so it needs no flag of its own. */
	static int version_f, switch_f, printpath_f, install_f, reset_f;
	version_f = switch_f = printpath_f = install_f = reset_f = 0;

	static struct option options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'v' },
		{ "switch", required_argument, 0, 's' },
		{ "print-path", no_argument, 0, 'p' },
		{ "install", no_argument, 0, 'I' },
		{ "reset", no_argument, 0, 'r' },
		{ NULL, 0, 0, 0 }
	};

	/*
	 * A leading colon leaves the complaining to us, which is what decides
	 * how it is worded: a missing argument is not an argument that is not
	 * there, and the two are told apart here rather than by getopt.
	 */
	while ((ch = getopt_long_only(argc, argv, ":hvprIs:", options,
	    NULL)) != (-1)) {
		switch (ch) {
			case 'h':
				usage(NULL);
				break;
			case 'v':
				version_f = 1;
				break;
			case 's':
				switch_f = 1;
				path = optarg;
				break;
			case 'p':
				printpath_f = 1;
				break;
			case 'I':
				install_f = 1;
				break;
			case 'r':
				reset_f = 1;
				break;
			case ':':
				snprintf(complaint, sizeof(complaint),
				    "missing argument to '%s'", argv[optind - 1]);
				usage(complaint);
			default:
				snprintf(complaint, sizeof(complaint),
				    "invalid argument '%s'", argv[optind - 1]);
				usage(complaint);
		}
	}

	/* Anything left over is an operand, and this tool takes none. */
	if (optind < argc) {
		snprintf(complaint, sizeof(complaint), "invalid argument '%s'",
		    argv[optind]);
		usage(complaint);
	}

	if (version_f == 1)
		version();

	if (install_f == 1) {
		xcselect_trigger_install_request("xcode-select");
		return 0;
	}

	if (reset_f == 1) {
		/* Back to the default command line tools path. */
		if (validate_directory_path(XCRUN_DEFAULT_DEVELOPER_DIR) == 0)
			return set_developer_path(XCRUN_DEFAULT_DEVELOPER_DIR) == 0 ? 0 : 1;
		fprintf(stderr, "xcode-select: error: unable to determine the"
		    " default developer directory.\n");
		return 1;
	}

	if (switch_f == 1) {
		if (validate_directory_path(path) == 0)
			return set_developer_path(path) == 0 ? 0 : 1;
		else
			return 1;
	}

	if (printpath_f == 1) {
		path = get_developer_path();
		if (path == NULL)
			return 1;
		fprintf(stdout, "%s\n", path);
	}

	return 0;
}
