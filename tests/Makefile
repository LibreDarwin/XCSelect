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

all: $(DRIVERS)

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
check: all
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh xsmatrix.sh
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh libxsmatrix.sh
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} bash ivkmatrix.sh

# The wide Apple-vs-ours surface.  Not part of check: it is a per-argument
# sweep, and it needs both a full Xcode and the command line tools.  Every
# case in it is compared against Apple's own transcript.  Needs bash.
matrix: all
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} bash xcrunmatrix.sh

# The write side, -s and -r, against a sandbox.  Also not part of check:
# it compiles a second xcode-select and libxcselect with
# XC_SELECT_WRITE_ROOT lowered into a scratch tree and links
# tests/rootstub.c's geteuid(), so it needs the SDK and CC the products
# were built with, and it is the one harness here that is not a plain
# differential script.
sandbox: all
	XS_FIX=$${XS_FIX:-/tmp/dt/xs} sh sandbox.sh

.PHONY: all check matrix sandbox