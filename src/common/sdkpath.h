/*
 * sdkpath.h -- locate SDKs and toolchains inside a Developer directory.
 *
 * Two layouts exist and both are supported:
 *
 *   Apple's, which is what a stock Xcode ships and what our own
 *   build/release/Developer tree emits:
 *
 *       <dev>/Platforms/<P>.platform/Developer/SDKs/<name>.sdk
 *       <dev>/Toolchains/<name>.xctoolchain
 *
 *   and the flatter one this project used first, still accepted so that
 *   an existing tree keeps working:
 *
 *       <dev>/SDKs/<name>.sdk
 *       <dev>/Toolchains/<name>.toolchain
 *
 * Apple's is tried first in both cases.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XCODE_TOOLS_SDKPATH_H_
#define _XCODE_TOOLS_SDKPATH_H_

/**
 * @func xt_find_sdk -- absolute path of an SDK by name
 * @arg devdir - the Developer directory to search
 * @arg name - SDK name without the .sdk suffix, e.g. "MacOSX"
 * @return: malloc'd path, or NULL when no such SDK exists
 */
char *xt_find_sdk(const char *devdir, const char *name);

/**
 * @func xt_sdk_platform_path -- the .platform bundle an SDK sits inside
 * @arg sdkpath - absolute path of the .sdk directory
 * @return: malloc'd path to the enclosing <name>.platform directory, or
 *          NULL when the SDK is not inside one
 */
char *xt_sdk_platform_path(const char *sdkpath);

/**
 * @func xt_platform_setting -- read a top-level string from a platform's Info.plist
 * @arg platformpath - absolute path of the .platform directory
 * @arg key - key to read, e.g. "Version"
 * @return: malloc'd value, or NULL when the plist or key is absent
 */
char *xt_platform_setting(const char *platformpath, const char *key);

/**
 * @func xt_find_toolchain -- absolute path of a toolchain by name
 * @arg devdir - the Developer directory to search
 * @arg name - toolchain name without suffix, e.g. "XcodeDefault"
 * @return: malloc'd path, or NULL when no such toolchain exists
 */
char *xt_find_toolchain(const char *devdir, const char *name);

/**
 * @func xt_first_sdk_name -- name of any SDK present, for use as a default
 * @arg devdir - the Developer directory to search
 * @return: malloc'd name without the .sdk suffix, or NULL if none
 */
char *xt_first_sdk_name(const char *devdir);

/**
 * @func xt_any_sdk_name -- name of any SDK present, in directory order
 * @arg devdir - the Developer directory to search
 * @return: malloc'd name without the .sdk suffix, or NULL if none
 */
char *xt_any_sdk_name(const char *devdir);

/**
 * @func xt_foreach_sdk -- call back for every SDK in a Developer directory
 * @arg devdir - the Developer directory to search
 * @arg cb - called with the platform name (without ".platform", empty in
 *           the flat layout) and the SDK's absolute path
 * @arg ctx - passed through to the callback
 * @return: number of SDKs found
 */
typedef void (*xt_sdk_cb)(const char *platform, const char *sdkpath, void *ctx);
int xt_foreach_sdk(const char *devdir, xt_sdk_cb cb, void *ctx);

/**
 * @func xt_sdk_setting -- read a top-level string from SDKSettings.plist
 * @arg sdkpath - absolute path of the .sdk directory
 * @arg key - key to read, e.g. "Version" or "CanonicalName"
 * @return: malloc'd value, or NULL when the plist or key is absent
 */
char *xt_sdk_setting(const char *sdkpath, const char *key);

/**
 * @func xt_sdk_default_property -- read a key from SDKSettings' DefaultProperties
 * @arg sdkpath - absolute path of the .sdk directory
 * @arg key - key inside the DefaultProperties dictionary
 * @return: malloc'd value, or NULL when absent
 */
char *xt_sdk_default_property(const char *sdkpath, const char *key);

/**
 * @func xt_toolchain_identifier -- read Identifier from ToolchainInfo.plist
 * @arg tcpath - absolute path of the toolchain directory
 * @return: malloc'd identifier, or NULL when absent
 */
char *xt_toolchain_identifier(const char *tcpath);

#endif /* _XCODE_TOOLS_SDKPATH_H_ */
