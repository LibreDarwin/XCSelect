/* libxstest - exercise one libxcselect export at a time, print the
 * result, exit.  The driver dlopens whichever libxcselect it is given
 * (Apple's system copy or our build) so the same call sequence can be
 * run against both and the two transcripts diffed.  Every call is
 * read-only; nothing here asks for an install or rewrites developer
 * directory state, and each run is a fresh process so DEVELOPER_DIR is
 * exactly what the caller set.
 *
 * Usage: libxstest <library> <op> [args...]
 *
 * op:
 *   devdir                  -> get_developer_dir_path, using the caller's env
 *   match <dir-or-NIL> <p>  -> developer_dir_matches_path
 *   find <path>             -> find_developer_contents_from_path
 *   version                 -> get_version
 *   hostsdk <which>         -> host_sdk_path
 *   bundle <id>             -> bundle_is_developer_tool
 *   manpaths                -> get_manpaths(NULL) + walker, using the env
 *
 * Copyright (c) 2026, LibreDarwin.  BSD-3-Clause.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>

typedef bool (*devdir_fn)(char *, int, bool *, bool *, bool *);
typedef bool (*match_fn)(const char *, const char *);
typedef bool (*find_fn)(char *, char *, size_t, bool *);
typedef char *(*version_fn)(void);
typedef int (*hostsdk_fn)(int, char **);
typedef bool (*bundle_fn)(char *);
typedef struct _xcselect_manpaths {
	char **paths;
	uint32_t count;
} xcselect_manpaths;
typedef xcselect_manpaths *(*manpaths_fn)(char *);
typedef uint32_t (*manpaths_num_fn)(xcselect_manpaths *);
typedef const char *(*manpaths_get_fn)(xcselect_manpaths *, uint32_t);
typedef void (*manpaths_free_fn)(xcselect_manpaths *);

static void *lib;

static void *need(const char *sym)
{
	void *p = dlsym(lib, sym);
	if (p == NULL) {
		fprintf(stderr, "NOSYM %s\n", sym);
		exit(2);
	}
	return p;
}

int main(int argc, char *argv[])
{
	char buf[4096];
	bool b0, b1, b2;
	const char *op;
	int n;

	if (argc < 3) {
		fprintf(stderr, "usage: libxstest <library> <op> [args...]\n");
		return 2;
	}

	if ((lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) == NULL) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}

	op = argv[2];

	if (strcmp(op, "devdir") == 0) {
		devdir_fn f = (devdir_fn)need("xcselect_get_developer_dir_path");
		if (f(buf, sizeof(buf), &b0, &b1, &b2))
			printf("devdir=%s env=%d clt=%d def=%d\n", buf, b0, b1, b2);
		else
			printf("devdir=NONE\n");
	} else if (strcmp(op, "match") == 0) {
		match_fn f = (match_fn)need("xcselect_developer_dir_matches_path");
		const char *dir = strcmp(argv[3], "NIL") ? argv[3] : NULL;
		printf("match=%d\n", f(dir, argv[4]));
	} else if (strcmp(op, "find") == 0) {
		find_fn f = (find_fn)need("xcselect_find_developer_contents_from_path");
		char path[4096];
		strlcpy(path, argv[3], sizeof(path));
		if (f(path, buf, sizeof(buf), &b1))
			printf("find=%s clt=%d path=%s\n", buf, b1, path);
		else
			printf("find=NONE\n");
	} else if (strcmp(op, "version") == 0) {
		version_fn f = (version_fn)need("xcselect_get_version");
		printf("version=%s\n", f());
	} else if (strcmp(op, "hostsdk") == 0) {
		hostsdk_fn f = (hostsdk_fn)need("xcselect_host_sdk_path");
		char *out = NULL;
		n = f(atoi(argv[3]), &out);
		printf("hostsdk=%s rc=%d\n", out != NULL ? out : "(null)", n);
		free(out);
	} else if (strcmp(op, "bundle") == 0) {
		bundle_fn f = (bundle_fn)need("xcselect_bundle_is_developer_tool");
		char *id = strcmp(argv[3], "NIL") ? argv[3] : NULL;
		printf("bundle=%d\n", f(id));
	} else if (strcmp(op, "manpaths") == 0) {
		manpaths_fn g = (manpaths_fn)need("xcselect_get_manpaths");
		manpaths_num_fn num = (manpaths_num_fn)need("xcselect_manpaths_get_num_paths");
		manpaths_get_fn get = (manpaths_get_fn)need("xcselect_manpaths_get_path");
		manpaths_free_fn fre = (manpaths_free_fn)need("xcselect_manpaths_free");
		xcselect_manpaths *mp = g(NULL);
		if (mp == NULL) {
			printf("manpaths=NONE\n");
			return 0;
		}
		printf("manpaths=%u\n", num(mp));
		for (uint32_t i = 0; i < num(mp); i++)
			printf("  %s\n", get(mp, i));
		fre(mp);
	} else {
		fprintf(stderr, "libxstest: bad op %s\n", op);
		return 2;
	}

	dlclose(lib);
	return 0;
}