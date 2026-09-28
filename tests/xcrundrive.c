/* xcrundrive -- dlopen a libxcrun and call xcrun_main, the way the xcrun
 * front-end does, so the same arguments can be run against Apple's copy
 * and ours and the two transcripts diffed.  The library is picked by
 * DRIVE_LIB, and the developer directory is passed through so both sides
 * see the same DEVELOPER_DIR without touching the real xcode-select
 * state.  Every run is a fresh process and read-only.
 *
 * Usage: xcrundrive [<args...>]
 *   the arguments are handed to xcrun_main as its argv, verbatim.
 *
 * Environ:
 *   DRIVE_DEV  developer directory, given to xcrun_main as its devdir
 *   DRIVE_LIB  libxcrun to attempt to load; no default, so a suite that
 *              forgets to set it fails loudly instead of mis-diffing
 *
 * Copyright (c) 2026, LibreDarwin.  BSD-3-Clause.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*xcrun_main_fn)(char *, int, char *[], const char *);

int
main(int argc, char *argv[])
{
	const char *devdir = getenv("DRIVE_DEV");
	const char *lib = getenv("DRIVE_LIB");
	void *handle;
	xcrun_main_fn f;

	if (lib == NULL) {
		fprintf(stderr, "xcrundrive: DRIVE_LIB is not set\n");
		return 2;
	}
	if ((handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL)) == NULL) {
		fprintf(stderr, "xcrundrive: dlopen: %s\n", dlerror());
		return 2;
	}
	dlerror();
	f = dlsym(handle, "xcrun_main");
	if (f == NULL) {
		fprintf(stderr, "xcrundrive: xcrun_main: %s\n", dlerror());
		return 2;
	}
	fflush(NULL);
	f(NULL, argc - 1, argv + 1, devdir);
	return 0;
}