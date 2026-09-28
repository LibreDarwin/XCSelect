/* ivk.c -- drive xcselect_invoke_xcrun directly, from a chosen libxcselect.
 *
 * The /usr/bin/xcrun shim's whole job is this one call, so a direct driver
 * measures our libxcselect's invoke_xcrun against Apple's with the same
 * arguments, the same DEVELOPER_DIR, and -- because both pick the fixture's
 * own libxcrun -- exactly the same downstream.  Any difference is the
 * library's own doing.
 *
 * Usage: ivk [-X] [<tool_name>] [args...]
 *   -X         pass require_xcode = true
 *   <tool_name> "-"  is the shim's own NULL (xcrun itself); the following
 *           arguments are the tool's.  Any other word is the tool to run
 *           (progname), as xcode-select hands it over.
 *
 * Environ:
 *   DRIVE_DEV  developer directory to set DEVELOPER_DIR to (checked first)
 *   DRIVE_LIB  libxcselect to load; without it, Apple's /usr/lib one
 *
 * Copyright (c) 2026, LibreDarwin.  BSD-3-Clause.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char *argv[])
{
	void (*invoke)(char *, int, char *[], int);
	bool require_xcode = false;
	char *tool = NULL;
	void *handle;
	int a;

	for (a = 1; a < argc; a++) {
		if (tool == NULL && strcmp(argv[a], "-X") == 0) {
			require_xcode = true;
			continue;
		}
		tool = argv[a];
		break;
	}

	if (tool == NULL) {
		fputs("usage: ivk [-X] [tool_name|-] [args...]\n", stderr);
		exit(2);
	}
	/* The shim's own NULL, spelled "-" here. */
	if (strcmp(tool, "-") == 0)
		tool = NULL;

	if (getenv("DRIVE_DEV") != NULL)
		setenv("DEVELOPER_DIR", getenv("DRIVE_DEV"), 1);

	if ((handle = dlopen(getenv("DRIVE_LIB") != NULL ? getenv("DRIVE_LIB") :
	    "/usr/lib/libxcselect.dylib", RTLD_LAZY | RTLD_LOCAL)) == NULL) {
		fprintf(stderr, "ivk: dlopen: %s\n", dlerror());
		exit(1);
	}
	if ((invoke = (void (*)(char *, int, char *[], int))
	    dlsym(handle, "xcselect_invoke_xcrun")) == NULL) {
		fprintf(stderr, "ivk: dlsym: %s\n", dlerror());
		exit(1);
	}

	invoke(tool, argc - a - 1, argv + a + 1, require_xcode);

	fputs("ivk: invoke returned (should not happen)\n", stderr);
	return 1;
}