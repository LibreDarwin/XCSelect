/*
 * sdkpath.c -- locate SDKs and toolchains inside a Developer directory.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "plist.h"
#include "sdkpath.h"

static int
is_dir(const char *path)
{
	struct stat st;

	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static char *
build_path(const char *a, const char *b, const char *c, const char *d)
{
	char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "%s%s%s%s",
		 a ? a : "", b ? b : "", c ? c : "", d ? d : "");
	return strdup(buf);
}

/*
 * Apple keeps each SDK inside its platform bundle, so finding one by
 * name means walking the platforms rather than looking in a single
 * directory.
 */
/*
 * Report the path as the filesystem spells it.
 *
 * Directory lookups here are literal, and macOS volumes are usually
 * case-insensitive, so "macosx.sdk" finds MacOSX.sdk and would other-
 * wise be handed back verbatim -- a path that resolves for the caller
 * by luck of the volume and names no directory that exists.  Resolving
 * it means --show-sdk-path prints what is really there, and the same
 * name works on a case-sensitive volume, where the literal lookup fails
 * and the canonical-name search below answers instead.
 */
static char *
dup_real_path(const char *path)
{
	char resolved[PATH_MAX];

	if (realpath(path, resolved) != NULL)
		return strdup(resolved);

	return strdup(path);
}

static char *
find_sdk_in_platforms(const char *devdir, const char *name)
{
	char platforms[PATH_MAX], candidate[PATH_MAX];
	struct dirent *e;
	DIR *d;
	char *found = NULL;

	snprintf(platforms, sizeof(platforms), "%s/Platforms", devdir);
	if ((d = opendir(platforms)) == NULL)
		return NULL;

	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);

		if (len < 10 || strcmp(e->d_name + len - 9, ".platform") != 0)
			continue;

		snprintf(candidate, sizeof(candidate),
			 "%s/%s/Developer/SDKs/%s.sdk",
			 platforms, e->d_name, name);
		if (is_dir(candidate)) {
			found = dup_real_path(candidate);
			break;
		}
	}

	closedir(d);
	return found;
}

/*
 * Match a name against an SDK's canonical name.
 *
 * An SDK answers to two things: the bundle's directory name, and the
 * CanonicalName in its SDKSettings.plist.  "MacOSX.Internal" is the
 * first, "macosx26.5.internal" the second, and a build system may use
 * either.  A bare family name -- "macosx" -- selects a versioned SDK of
 * that family, which is why the remainder after the family has to be a
 * version and nothing else: otherwise "macosx" would also match
 * "macosx26.5.internal" and asking for the plain macOS SDK could hand
 * back the internal one.
 */
static int
canonical_matches(const char *canonical, const char *name, int family_ok)
{
	size_t n;

	if (canonical == NULL || name == NULL)
		return 0;

	if (strcasecmp(canonical, name) == 0)
		return 1;

	if (!family_ok)
		return 0;

	n = strlen(name);
	if (strncasecmp(canonical, name, n) != 0)
		return 0;

	/* Everything after the family must look like a version. */
	for (canonical += n; *canonical != '\0'; canonical++)
		if (!isdigit((unsigned char)*canonical) && *canonical != '.')
			return 0;

	return 1;
}

struct canonical_search {
	const char *name;
	int family_ok;
	char *found;
};

static void
canonical_probe(const char *platform, const char *sdkpath, void *ctx)
{
	struct canonical_search *search = ctx;
	char *canonical;

	(void)platform;

	if (search->found != NULL)
		return;

	if ((canonical = xt_sdk_setting(sdkpath, "CanonicalName")) == NULL)
		return;

	if (canonical_matches(canonical, search->name, search->family_ok))
		search->found = strdup(sdkpath);

	free(canonical);
}

char *
xt_find_sdk(const char *devdir, const char *name)
{
	struct canonical_search search;
	char *path;

	if (devdir == NULL || name == NULL)
		return NULL;

	if ((path = find_sdk_in_platforms(devdir, name)) != NULL)
		return path;

	/* The flat layout this project used before. */
	path = build_path(devdir, "/SDKs/", name, ".sdk");
	if (path != NULL && is_dir(path)) {
		char *real = dup_real_path(path);

		free(path);
		return real;
	}
	free(path);

	/*
	 * No bundle by that directory name, so ask the SDKs what they
	 * call themselves.  Exact canonical names first, so an explicit
	 * "macosx26.5.internal" is never answered by the family match
	 * below.
	 */
	search.name = name;
	search.found = NULL;

	search.family_ok = 0;
	xt_foreach_sdk(devdir, canonical_probe, &search);
	if (search.found != NULL)
		return search.found;

	search.family_ok = 1;
	xt_foreach_sdk(devdir, canonical_probe, &search);
	return search.found;
}

/*
 * The platform bundle an SDK belongs to.
 *
 * Apple's layout is <dev>/Platforms/<name>.platform/Developer/SDKs/
 * <name>.sdk, so the platform is found by walking back up to the first
 * component that ends in ".platform" rather than by counting directories
 * -- the depth is not something to rely on, and an SDK in the old flat
 * layout sits inside no platform at all, which is what NULL reports.
 */
char *
xt_sdk_platform_path(const char *sdkpath)
{
	char buf[PATH_MAX];
	char *slash;

	if (sdkpath == NULL)
		return NULL;

	if (snprintf(buf, sizeof(buf), "%s", sdkpath) >= (int)sizeof(buf))
		return NULL;

	while ((slash = strrchr(buf, '/')) != NULL) {
		size_t len = strlen(slash + 1);

		if (len > 9 && strcmp(slash + 1 + len - 9, ".platform") == 0)
			return strdup(buf);

		*slash = '\0';
	}

	return NULL;
}

char *
xt_find_toolchain(const char *devdir, const char *name)
{
	char *path;

	if (devdir == NULL || name == NULL)
		return NULL;

	path = build_path(devdir, "/Toolchains/", name, ".xctoolchain");
	if (path != NULL && is_dir(path))
		return path;
	free(path);

	path = build_path(devdir, "/Toolchains/", name, ".toolchain");
	if (path != NULL && is_dir(path))
		return path;
	free(path);

	return NULL;
}

/*
 * Used to pick a default when nothing has been selected.
 *
 * The host platform comes first: a Developer directory holds SDKs for
 * every platform Xcode supports, and taking whichever the filesystem
 * happens to return picks something arbitrary -- AppleTVOS, as it turns
 * out, where Apple's xcrun would say macosx.  Only if there is no macOS
 * SDK does anything else get considered.
 */
char *
xt_first_sdk_name(const char *devdir)
{
	static const char *preferred[] = { "MacOSX", NULL };
	size_t p;

	for (p = 0; preferred[p] != NULL; p++) {
		char *path = xt_find_sdk(devdir, preferred[p]);

		if (path != NULL) {
			free(path);
			return strdup(preferred[p]);
		}
	}

	return xt_any_sdk_name(devdir);
}

/*
 * Any SDK at all, platforms first so the answer matches xt_find_sdk's
 * preference.  Versioned names (MacOSX26.5.sdk) are returned as they
 * appear; callers pass them straight back to xt_find_sdk.
 */
char *
xt_any_sdk_name(const char *devdir)
{
	char dirpath[PATH_MAX], inner[PATH_MAX];
	struct dirent *e, *e2;
	DIR *d, *d2;
	char *name = NULL;

	if (devdir == NULL)
		return NULL;

	snprintf(dirpath, sizeof(dirpath), "%s/Platforms", devdir);
	if ((d = opendir(dirpath)) != NULL) {
		while (name == NULL && (e = readdir(d)) != NULL) {
			size_t len = strlen(e->d_name);

			if (len < 10 || strcmp(e->d_name + len - 9, ".platform") != 0)
				continue;

			snprintf(inner, sizeof(inner), "%s/%s/Developer/SDKs",
				 dirpath, e->d_name);
			if ((d2 = opendir(inner)) == NULL)
				continue;

			while ((e2 = readdir(d2)) != NULL) {
				size_t l2 = strlen(e2->d_name);
				char *dot;

				if (l2 < 5 || strcmp(e2->d_name + l2 - 4, ".sdk") != 0)
					continue;
				name = strdup(e2->d_name);
				if (name != NULL && (dot = strstr(name, ".sdk")) != NULL)
					*dot = '\0';
				break;
			}
			closedir(d2);
		}
		closedir(d);
	}
	if (name != NULL)
		return name;

	snprintf(dirpath, sizeof(dirpath), "%s/SDKs", devdir);
	if ((d = opendir(dirpath)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			size_t len = strlen(e->d_name);
			char *dot;

			if (len < 5 || strcmp(e->d_name + len - 4, ".sdk") != 0)
				continue;
			name = strdup(e->d_name);
			if (name != NULL && (dot = strstr(name, ".sdk")) != NULL)
				*dot = '\0';
			break;
		}
		closedir(d);
	}

	return name;
}

/*
 * Walk every SDK in the directory: each platform bundle's SDKs first,
 * then the flat layout.  Symlinked SDKs (MacOSX26.sdk -> MacOSX.sdk, as
 * a stock Xcode ships) are reported like any other, which is what
 * xcodebuild -showsdks lists.
 */
int
xt_foreach_sdk(const char *devdir, xt_sdk_cb cb, void *ctx)
{
	char dirpath[PATH_MAX], inner[PATH_MAX], sdkpath[PATH_MAX];
	struct dirent *e, *e2;
	DIR *d, *d2;
	int found = 0;

	if (devdir == NULL || cb == NULL)
		return 0;

	snprintf(dirpath, sizeof(dirpath), "%s/Platforms", devdir);
	if ((d = opendir(dirpath)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			size_t len = strlen(e->d_name);
			char platform[PATH_MAX];

			if (len < 10 || strcmp(e->d_name + len - 9, ".platform") != 0)
				continue;

			snprintf(platform, sizeof(platform), "%.*s",
				 (int)(len - 9), e->d_name);
			snprintf(inner, sizeof(inner), "%s/%s/Developer/SDKs",
				 dirpath, e->d_name);
			if ((d2 = opendir(inner)) == NULL)
				continue;

			while ((e2 = readdir(d2)) != NULL) {
				size_t l2 = strlen(e2->d_name);

				if (l2 < 5 || strcmp(e2->d_name + l2 - 4, ".sdk") != 0)
					continue;
				snprintf(sdkpath, sizeof(sdkpath), "%s/%s",
					 inner, e2->d_name);
				cb(platform, sdkpath, ctx);
				found++;
			}
			closedir(d2);
		}
		closedir(d);
	}

	snprintf(dirpath, sizeof(dirpath), "%s/SDKs", devdir);
	if ((d = opendir(dirpath)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			size_t len = strlen(e->d_name);

			if (len < 5 || strcmp(e->d_name + len - 4, ".sdk") != 0)
				continue;
			snprintf(sdkpath, sizeof(sdkpath), "%s/%s", dirpath, e->d_name);
			cb("", sdkpath, ctx);
			found++;
		}
		closedir(d);
	}

	return found;
}

/*
 * Read a plist wholesale.  These files are small -- a few kilobytes at
 * most -- so there is no reason to stream them.
 */
static plist_node *
read_plist(const char *path)
{
	plist_node *root;
	struct stat st;
	char *text;
	size_t got;
	FILE *fp;

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return NULL;
	if ((fp = fopen(path, "r")) == NULL)
		return NULL;
	if ((text = malloc((size_t)st.st_size + 1)) == NULL) {
		fclose(fp);
		return NULL;
	}

	got = fread(text, 1, (size_t)st.st_size, fp);
	fclose(fp);
	text[got] = '\0';

	root = plist_parse_any(text, got);
	free(text);

	return root;
}

static char *
sdk_string(const char *sdkpath, const char *section, const char *key)
{
	char path[PATH_MAX];
	plist_node *root, *dict, *node;
	char *value = NULL;

	if (sdkpath == NULL || key == NULL)
		return NULL;

	snprintf(path, sizeof(path), "%s/SDKSettings.plist", sdkpath);
	if ((root = read_plist(path)) == NULL)
		return NULL;

	dict = root;
	if (section != NULL && (dict = plist_dict_get(root, section)) == NULL) {
		plist_free(root);
		return NULL;
	}

	if ((node = plist_dict_get(dict, key)) != NULL && node->string != NULL)
		value = strdup(node->string);

	plist_free(root);
	return value;
}

char *
xt_sdk_setting(const char *sdkpath, const char *key)
{
	return sdk_string(sdkpath, NULL, key);
}

char *
xt_sdk_default_property(const char *sdkpath, const char *key)
{
	return sdk_string(sdkpath, "DefaultProperties", key);
}

/*
 * A top-level string from a platform bundle's Info.plist.
 *
 * Apple's platform carries the same version under three keys; "Version"
 * is the one that says what it means, so callers ask for that first and
 * fall back themselves.
 */
char *
xt_platform_setting(const char *platformpath, const char *key)
{
	char path[PATH_MAX];
	plist_node *root, *node;
	char *value = NULL;

	if (platformpath == NULL || key == NULL)
		return NULL;

	snprintf(path, sizeof(path), "%s/Info.plist", platformpath);
	if ((root = read_plist(path)) == NULL)
		return NULL;

	if ((node = plist_dict_get(root, key)) != NULL && node->string != NULL)
		value = strdup(node->string);

	plist_free(root);
	return value;
}

char *
xt_toolchain_identifier(const char *tcpath)
{
	char path[PATH_MAX];
	plist_node *root, *node;
	char *value = NULL;

	if (tcpath == NULL)
		return NULL;

	snprintf(path, sizeof(path), "%s/ToolchainInfo.plist", tcpath);
	if ((root = read_plist(path)) == NULL)
		return NULL;

	if ((node = plist_dict_get(root, "Identifier")) != NULL &&
	    node->string != NULL)
		value = strdup(node->string);

	plist_free(root);
	return value;
}
