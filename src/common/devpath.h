/*
 * devpath.h -- locate our own Developer directory at runtime.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XCODE_TOOLS_DEVPATH_H_
#define _XCODE_TOOLS_DEVPATH_H_

/**
 * @func xt_default_developer_dir -- Developer directory this binary lives in
 *
 * Our tools are installed at <developer_dir>/usr/bin/<tool>, so the
 * Developer directory is three levels above the executable.  Deriving it
 * at runtime rather than compiling in an absolute prefix keeps the
 * release tree relocatable: it can be moved, renamed, or selected with
 * xcode-select without rebuilding.
 *
 * @return: the directory, or NULL if it cannot be determined or does not
 *          look like a Developer directory.  The storage is static.
 */
const char *xt_default_developer_dir(void);

#endif /* _XCODE_TOOLS_DEVPATH_H_ */
