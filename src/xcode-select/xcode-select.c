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

/*
 * The version the tool answers for -v is the version of the developer
 * tools it shipped with, which is what the shipped tool answers and
 * what its library reports, so the two agree -- it is not this source
 * file's own revision.
 */
#define TOOL_VERSION "2416"
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
	fprintf(stdout, "xcode-select version %s.\n", TOOL_VERSION);

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
	static char devdir[PATH_MAX];
	bool from_env = false, cltools = false, fallback = false;

	/*
	 * The active directory is the library's question and not this
	 * one's: it is the chain xcrun and every other tool agree on --
	 * DEVELOPER_DIR, the selection, the system defaults -- and it is
	 * what the shipped tool prints for -p.  There is no per-user
	 * file beside it; nothing of Apple's ever consulted one, so a
	 * caller without HOME is no worse off here, and no stale value
	 * can linger where an uninstall removed the tools.
	 */
	if (xcselect_get_developer_dir_path(devdir, sizeof(devdir),
	    &from_env, &cltools, &fallback))
		return devdir;

	fprintf(stderr, "xcode-select: error: unable to determine the"
	    " developer directory.\n");
	return NULL;
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
	static int manpaths_f;
	version_f = switch_f = printpath_f = install_f = reset_f = 0;
	manpaths_f = 0;

	static struct option options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'v' },
		{ "switch", required_argument, 0, 's' },
		{ "print-path", no_argument, 0, 'p' },
		{ "install", no_argument, 0, 'I' },
		{ "reset", no_argument, 0, 'r' },
		{ "show-manpaths", no_argument, 0, 'm' },
		{ NULL, 0, 0, 0 }
	};

	/*
	 * A leading colon leaves the complaining to us, which is what decides
	 * how it is worded: a missing argument is not an argument that is not
	 * there, and the two are told apart here rather than by getopt.
	 *
	 * --install is a long option only: -I is not a switch, and asking
	 * for it is an invalid argument the same as any other.
	 */
	while ((ch = getopt_long_only(argc, argv, ":hvprs:", options,
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
			case 'm':
				manpaths_f = 1;
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

	/*
	 * Only one thing may be asked for.  Repeating the same thing is not
	 * asking for two -- -p -p prints the path once, and -s a -s b
	 * switches to the last one given -- so what is counted is how many
	 * different actions were named, not how many options there were.
	 *
	 * --switch is not one of the counted actions.  Naming a directory to
	 * switch to alongside something else is refused by that other thing
	 * rather than here, which is why -p -s /some/path prints the path and
	 * -s /some/path -p prints the path too, and neither of them mentions
	 * the switch.
	 */
	{
		int actions = (version_f != 0) + (printpath_f != 0) +
		    (install_f != 0) + (reset_f != 0) + (manpaths_f != 0);

		if (actions > 1)
			usage("cannot execute multiple actions");
	}

	if (version_f == 1)
		version();

	/*
	 * --show-manpaths is not in the help, and deliberately so: the shipped
	 * tool answers it but does not advertise it, so advertising it here
	 * would be a difference of its own.  It is a long option only, on the
	 * same reasoning as --install.
	 *
	 * The pages are the active developer directory's, not the sysroot's, so
	 * the sysroot argument is NULL: libxcselect takes it the way the shipped
	 * library takes it and reports the same list either way.
	 */
	if (manpaths_f == 1) {
		xcselect_manpaths *mp;
		uint32_t i, n;

		if ((mp = xcselect_get_manpaths(NULL)) == NULL) {
			fprintf(stderr, "%s: error: unable to get manpaths\n",
			    getprogname());
			return 1;
		}

		n = xcselect_manpaths_get_num_paths(mp);
		for (i = 0; i < n; i++)
			fprintf(stdout, "%s\n",
			    xcselect_manpaths_get_path(mp, i));

		xcselect_manpaths_free(mp);
		return 0;
	}

	if (printpath_f == 1) {
		path = get_developer_path();
		if (path == NULL)
			return 1;
		fprintf(stdout, "%s\n", path);
		return 0;
	}

	if (install_f == 1) {
		char devdir[PATH_MAX];
		bool from_env = false, cltools = false, fallback = false;

		/*
		 * There is nothing to install when a developer directory can
		 * already be named, which is the whole of what this asks:
		 * whether the tools are there.  Which one it names is not
		 * reported, and neither is the dialog, so this never has to
		 * decide whether it could put one up.
		 */
		if (xcselect_get_developer_dir_path(devdir, sizeof(devdir),
		    &from_env, &cltools, &fallback)) {
			fprintf(stderr, "%s: note: Command line tools are"
			    " already installed. Use \"Software Update\" in"
			    " System Settings or the softwareupdate command"
			    " line interface to install updates\n",
			    getprogname());
			return 1;
		}

		if (xcselect_trigger_install_request(getprogname())) {
			fputs("xcode-select: note: install requested for"
			    " command line developer tools\n", stderr);
			return 0;
		}

		fputs("xcode-select: error: no developer tools were found, and"
		    " no install could be requested (perhaps no UI is"
		    " present), please install manually from"
		    " 'developer.apple.com'.\n", stderr);
		return 1;
	}

	if (switch_f == 1) {
		char found[PATH_MAX];
		bool cltools = false;

		/*
		 * The directory is looked at before the privilege is, so a
		 * path that names nothing is reported as that whether or not
		 * the caller could have switched to it anyway.
		 *
		 * What counts as a developer directory is the library's
		 * question and not this one's: a bundle is taken for the
		 * developer directory inside it, and a directory that is
		 * merely a directory -- /tmp, /usr -- is not one.  A path
		 * that names nothing is one complaint, not several, so the
		 * wording is left to here.
		 */
		if (!xcselect_find_developer_contents_from_path(path, found,
		    sizeof(found), &cltools)) {
			fprintf(stderr, "xcode-select: error: invalid developer"
			    " directory '%s'\n", path);
			return 1;
		}

		if (geteuid() != 0) {
			fprintf(stderr, "xcode-select: error: --switch must be"
			    " run as root (e.g. `sudo %s --switch"
			    " <xcode_folder_path>`).\n", getprogname());
			return 1;
		}

		return set_developer_path(path) == 0 ? 0 : 1;
	}

	if (reset_f == 1) {
		if (geteuid() != 0) {
			fprintf(stderr, "xcode-select: error: --reset must be"
			    " run as root (e.g. `sudo %s --reset`).\n",
			    getprogname());
			return 1;
		}

		/* Back to the default command line tools path. */
		if (validate_directory_path(XCRUN_DEFAULT_DEVELOPER_DIR) == 0)
			return set_developer_path(XCRUN_DEFAULT_DEVELOPER_DIR) == 0 ? 0 : 1;
		fprintf(stderr, "xcode-select: error: unable to determine the"
		    " default developer directory.\n");
		return 1;
	}

	return 0;
}
