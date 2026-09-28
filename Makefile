# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Clean-room reimplementation of Apple's xcselect library and the two tools
# that link it: libxcselect, xcrun, xcode-select.
#
# Build layout: every artifact lives under build/; final products go to
# build/release/ or build/debug/ per CONFIG.
#
# Portable to both GNU make and BSD make (bmake): no pattern rules, no
# ifeq/ifdef/.if conditionals and no $(if)/$(shell) functions.  Per-config
# flags come from make/<CONFIG>.mk so both make variants behave identically.
#
# The same three products are also built by xcselect.xcodeproj; the two
# build systems agree on where objects and products land, so either one
# can be used from a clean tree without the other having run.

CONFIG ?= release
SDK    ?= /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.Internal.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
DESTDIR ?=

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

# -fblocks: libxcrun hands xcselect a block for utilities it does not know,
# so the handler registered with it has to be a real block.
CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra \
	  -Wno-unused-parameter -fblocks -I src/common -I src/libxcselect -I src/xcrun

# Apple's libxcselect is a 1.0.0 dylib, so match the version stamps.
DYLIB_VERSION := 1.0.0
DYLIB_COMPAT  := 1.0.0

# Apple's copy records /usr/lib/libxcselect.dylib as its identity, and on any
# macOS that resolves to Apple's own library rather than ours -- so a tool
# built here would silently bind to Apple's implementation and every test of
# our build would be measuring Apple instead.  @rpath plus the search paths
# below binds each tool to the library sitting beside it.  The identity
# string is metadata rather than behaviour: the file still installs to
# /usr/lib and is still loaded from there by path.  Same reasoning as
# BomCmds, which hit the identical problem with Bom.framework.
LIB_INSTALL_NAME := @rpath/libxcselect.dylib

# All three products land in the same directory, so @loader_path finds the
# library in the build tree, and @loader_path/../lib finds it in the
# installed tree -- which keeps a staged or relocated prefix working without
# baking the prefix into the binary.  Deliberately no $(CURDIR) here: that is
# a GNU make variable, and bmake spells it .CURDIR, so asking for it under
# GNU's name expands to nothing in bmake and yields a silently wrong rpath.
# /usr/lib is last, for a LibreDarwin system install alongside Apple's path.
#
# A custom LIBDIR other than $(PREFIX)/lib needs its own -rpath added here.
RPATHS := -Wl,-rpath,@loader_path -Wl,-rpath,@loader_path/../lib \
	  -Wl,-rpath,/usr/lib

LIB      := $(BUILD_DIR)/libxcselect.dylib
LIB_OBJS := $(OBJDIR)/libxcselect.o

# xcrun carries the vendored common/ SDK-settings reader with it; every one of
# these objects is reached, none is dead weight.  devpath.c is the one common/
# file xcrun does not need: it includes devpath.h but never calls into it.
# cfplist.c is the CoreFoundation property list reader sdkpath.c asks for the
# plists in, so xcrun names CoreFoundation at link time.
XCRUN      := $(BUILD_DIR)/xcrun
XCRUN_OBJS := $(OBJDIR)/xcrun.o $(OBJDIR)/ini.o $(OBJDIR)/sdkpath.o \
	      $(OBJDIR)/cfplist.o $(OBJDIR)/json.o

XSELECT      := $(BUILD_DIR)/xcode-select
XSELECT_OBJS := $(OBJDIR)/xcode-select.o

all: $(LIB) $(XCRUN) $(XSELECT)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -dynamiclib -o $@ $(LIB_OBJS) \
	    -install_name $(LIB_INSTALL_NAME) \
	    -current_version $(DYLIB_VERSION) \
	    -compatibility_version $(DYLIB_COMPAT)

$(XCRUN): $(XCRUN_OBJS) $(LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(XCRUN_OBJS) -L $(BUILD_DIR) -lxcselect $(RPATHS) \
		-framework CoreFoundation

$(XSELECT): $(XSELECT_OBJS) $(LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(XSELECT_OBJS) -L $(BUILD_DIR) -lxcselect $(RPATHS)

$(OBJDIR)/libxcselect.o: src/libxcselect/libxcselect.c src/libxcselect/xcselect.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/libxcselect/libxcselect.c

$(OBJDIR)/xcrun.o: src/xcrun/xcrun.c src/xcrun/ini.h src/common/devpath.h \
                   src/common/sdkpath.h src/libxcselect/xcselect.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcrun/xcrun.c

$(OBJDIR)/ini.o: src/xcrun/ini.c src/xcrun/ini.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcrun/ini.c

$(OBJDIR)/sdkpath.o: src/common/sdkpath.c src/common/sdkpath.h src/common/cfplist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/sdkpath.c

$(OBJDIR)/cfplist.o: src/common/cfplist.c src/common/cfplist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/cfplist.c

$(OBJDIR)/json.o: src/common/json.c src/common/json.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/json.c

$(OBJDIR)/xcode-select.o: src/xcode-select/xcode-select.c src/libxcselect/xcselect.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcode-select/xcode-select.c

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(LIBDIR)
	install -m 0755 $(XCRUN) $(DESTDIR)$(PREFIX)/bin/xcrun
	install -m 0755 $(XSELECT) $(DESTDIR)$(PREFIX)/bin/xcode-select
	install -m 0755 $(LIB) $(DESTDIR)$(LIBDIR)/libxcselect.dylib

clean:
	rm -rf build

.PHONY: all install clean
