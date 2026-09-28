// Copyright (c) 2026, LibreDarwin
// SPDX-License-Identifier: BSD-3-Clause
//
// xcrun is a shim.  All of the work happens in the developer directory:
// libxcselect works out which directory is active, loads that directory's
// usr/lib/libxcrun.dylib, and calls its xcrun_main.  The system /usr/bin/xcrun
// is a shim in exactly this shape, linking only libxcselect and libSystem.
//
// Matching that means the behaviour of xcrun is whatever the active developer
// directory's libxcrun says, which is the point: a Command Line Tools install
// and a full Xcode can then disagree, exactly as they do on a system where
// both ship their own copy of the library.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xcselect.h"

int
main(int argc, char *argv[])
{
	const char *name;

	/*
	 * getprogname, not argv[0]: the name has been set by whoever invoked
	 * us, and it may carry a leading '-' when the process was started
	 * from a setuid position.  That dash is not part of the name.
	 */
	name = getprogname();
	if (name[0] == '-')
		name++;

	/*
	 * A NULL name is how libxcrun is told this is the xcrun interface
	 * itself, the one caller for which no tool was named.  Every other
	 * name is a real tool name, which makes this the multicall entry
	 * point: installed under another name, xcrun runs that tool.
	 */
	if (strcasecmp(name, "xcrun") == 0)
		name = NULL;

	/*
	 * require_xcode is false.  A Command Line Tools directory is a
	 * complete enough developer directory to run tools from, and the
	 * system xcrun runs from it: that is what a machine with only the
	 * Command Line Tools installed does.
	 *
	 * argc and argv drop the xcrun argument, leaving the tool's own.
	 * Does not return on success.
	 */
	xcselect_invoke_xcrun((char *)name, argc - 1, argv + 1, false);
}
