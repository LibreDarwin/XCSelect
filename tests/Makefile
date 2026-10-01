# Differential parity harnesses.  The drivers dlopen a libxcselect or
# libxcrun by path, so they are built with the same toolchain the products
# use, but into build/test so they never sit next to the library or tools
# they measure (nothing in this tree uses them).  See README.md for the
# fixture contract and the four matrix scripts.
#
# Portable to both GNU make and BSD make (bmake).  CONFIG/SDK/CC default
# to the top-level Makefile's; make check runs all three read-only
# matrices against Apple's binaries using the default fixture root.
# xcrunmatrix.sh is the wide per-argument sweep and has its own `matrix`
# target; it needs both an Xcode and the Command Line Tools installed, so
# it is kept out of check, which runs the three read-only ones.

CONFIG ?= release
SDK    ?= /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.Internal.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include ../make/$(CONFIG).mk

BUILD_DIR := ../build/test

# The drivers need only dlopen and libc; -D_DARWIN_C_SOURCE is for
# setenv/strlcpy, which the vanilla clang headers leave out.
CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra

DRIVERS := $(BUILD_DIR)/ivk $(BUILD_DIR)/libxstest $(BUILD_DIR)/xcrundrive

# The matrices dlopen libxcselect and libxcrun *by path*, so those two have
# to be current before any of them runs -- and the rule for the drivers
# above is not enough on its own, because it only builds the drivers.  A
# stale library is measured silently, which is the worst failure mode a
# differential harness has: it cannot tell you it is looking at the wrong
# build, it just disagrees with you about the code you are looking at.
# That is not hypothetical -- a fixed --log line still read as missing from
# the matrix until this was added, because the library had not been
# rebuilt since the edit.
#
# Recursing is cheap because the top level is incremental, and it keeps the
# build flags from being restated here where they would drift.
.PHONY: products
products:
	$(MAKE) -C .. CONFIG=$(CONFIG)

all: $(DRIVERS) products

$(BUILD_DIR)/ivk: ivk.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ ivk.c

$(BUILD_DIR)/libxstest: libxstest.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ libxstest.c

$(BUILD_DIR)/xcrundrive: xcrundrive.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ xcrundrive.c

# The matrices are read-only, so they are safe to run from a normal user
# account; set XS_FIX if your fixtures live somewhere other than
# /tmp/dt/xs.  ivkmatrix.sh and xcrunmatrix.sh run xcrun shim-style
# invocations and need bash; the other two are plain sh.
#
# sandbox.sh is in here too, and used to be a separate target on the
# grounds that it is the one harness that is not a plain differential
# script: it compiles a second xcode-select and libxcselect with
# XC_SELECT_WRITE_ROOT lowered into a scratch tree and links
# tests/rootstub.c's geteuid().  Both of those reasons turn out to be
# already covered -- all depends on the products, so the SDK and CC are
# known good by the time any target runs, and the libraries the matrices
# measure are rebuilt rather than whatever was there last -- and it takes
# about 0.7s.
# Leaving it out of the default gate meant the only harness that proves
# the -s/-p contract, and the only one that checks the root gate is real,
# was not run by `make check`.
check: all
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh xsmatrix.sh
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh libxsmatrix.sh
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} bash ivkmatrix.sh
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh sandbox.sh

# The wide Apple-vs-ours surface.  Not part of check: it is a per-argument
# sweep of all 242 cases, and it needs both a full Xcode and the command
# line tools.  Every case in it is compared against Apple's own
# transcript.  Needs bash.
matrix: all
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} bash xcrunmatrix.sh

# Everything, for when a run is meant to be exhaustive.  Same caveat as
# matrix: needs a full Xcode and the command line tools, not just an SDK.
test: check matrix

# Just the write side, if that is all that changed.  Kept as its own
# target so a write-side edit can be re-run without the read-side sweep;
# check runs it too.
sandbox: all
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh sandbox.sh

.PHONY: all check matrix test sandbox