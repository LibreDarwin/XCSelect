#!/bin/bash
# Apple xcrun vs our libxcrun, per developer directory, across the full
# argument surface.  xcrundrive dlopens the libxcrun named by DRIVE_LIB
# (our build) and calls xcrun_main with the same args and devdir, so the
# two transcripts can be diffed without touching real xcode-select state.
# The fixture dev-dir layouts live under XS_FIX, default /tmp/dt/xs.
#
# Requires bash (arrays, $() and sed -E).  Warm-up runs Apple's plist-
# sensitive calls once so the captured run is its steady, database-learned
# state, then each side is captured and normalized; only timestamps,
# result-bundle names and the fixture CWD are folded, errno stays visible.
#
# This is the exploratory suite, not part of `make check`: most named-SDK
# failures and the -n/-k verbose SDK-resolution trace genuinely diverge
# from Apple by design (Apple re-execs xcodebuild there; we stay in
# process).  See README.md and local/xcselect.md for the per-family note.

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIX=${XS_FIX:-/tmp/dt/xs}
FIXR=$(CDPATH= cd -- "$FIX" && pwd -P)
LIB=$REPO/build/release/libxcrun.dylib
DRIVE=$REPO/build/test/xcrundrive
X=/Applications/Xcode.app/Contents/Developer
C=/Library/Developer/CommandLineTools
ST=$(mktemp -d "${TMPDIR:-/tmp}/xcrunmatrix.XXXXXX") || exit 2
trap 'rm -rf "$ST"' EXIT

norm() { sed -E \
  -e 's#ResultBundle_[0-9-]+_[0-9]+\.xcresult#ResultBundle_X.xcresult#g' \
  -e 's#[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?#TIMESTAMP#g' \
  -e "s#$FIXR#CWD#g" -e "s#$FIX#CWD#g" "$1"; }

CASES=(
  "--version" "-h"
  "--sdk macosx --show-sdk-path" "--sdk macosx15 --show-sdk-path"
  "--sdk macosx15.4 --show-sdk-path" "--sdk macosx26 --show-sdk-path"
  "--sdk macosx26.5 --show-sdk-path" "--sdk macosx26.0 --show-sdk-path"
  "--sdk macosx999 --show-sdk-path" "--sdk iphoneos --show-sdk-path"
  "--show-sdk-path" "--show-sdk-version" "--show-sdk-build-version"
  "--show-sdk-platform-path" "--show-sdk-platform-version"
  "--sdk bogusfoo --show-sdk-path" "--sdk macosx15.4 --show-sdk-version"
  "--sdk macosx26 --show-sdk-build-version"
  "--find clang" "--find ld" "--find cups-config" "--find definitely-not-a-tool"
  "--sdk macosx --find clang" "--sdk macosx15.4 --find clang"
  "--sdk macosx15.4 --find ld" "--sdk macosx15.4 --find cups-config"
  "--sdk macosx26 --find clang" "--sdk bogusfoo --find clang"
  "--sdk bogusfoo --find definitely-not-a-tool"
  "--find /usr/bin/clang" "--find /nonexistent/tool" "--find ./relative-tool"
  "--find sub/dir/tool" "--find /" "--find a/" "--find usr/bin/clang"
  "--find /usr/bin/" "--sdk bogusfoo --find /usr/bin/clang"
  "-f /usr/bin/clang" "-f ./relative-tool" "-r /usr/bin/true"
  "--sdk macosx --print-sdk" "--log" "--no-cache --find clang"

  # Verbose trace: manpath/environment/tool keys.  The "lookup resolved"
  # notes report the resolution this run performed, which is what Apple
  # reports once its database has learned the answer (its warm state, and
  # the state the warm-up below produces).  Bad-name SDKs are avoided here:
  # Apple's diagnostics for a name it cannot locate differ independently of
  # the trace (see the --sdk bogusfoo cases above).
  "-v" "--log" "-k" "--no-cache"
  "--verbose --show-sdk-path" "-v --show-sdk-version"
  "-v --sdk macosx --show-sdk-path" "-v --sdk macosx --show-sdk-version"
  "-v --sdk macosx --show-sdk-build-version"
  "-v --show-sdk-platform-path" "-v --show-sdk-platform-version"
  "-v --sdk macosx --show-sdk-platform-path"
  "-v --no-cache --show-sdk-path" "-v --show-toolchain-path"
  "-v -n --show-toolchain-path" "-v -k --show-toolchain-path"
  "-v --log --show-sdk-path"
  "-v --find clang" "-v --no-cache --find clang"
  "-v --sdk macosx --find clang" "-v --sdk macosx --no-cache --find clang"
  "-v --toolchain XcodeDefault --find clang"
  "-v --find /usr/bin/true" "-v /usr/bin/true"
  "-v --kill-cache --find clang" "-v --kill-cache --find nosuchtool"
  "-v --sdk bogusfoo --find clang"
)

total=0; diffs=0
for d in "$X" "$C"; do
  echo "=== $(basename "$d") ==="
  for c in "${CASES[@]}"; do
    # shellcheck disable=SC2086
    set -- $c
    # warm plist-sensitive Apple calls; the second run is the steady state
    DEVELOPER_DIR=$d /usr/bin/xcrun "$@" >/dev/null 2>&1
    DEVELOPER_DIR=$d /usr/bin/xcrun "$@" >"$ST/a.out" 2>"$ST/a.err"; ar=$?
    DRIVE_DEV=$d DRIVE_LIB=$LIB "$DRIVE" "$@" >"$ST/o.out" 2>"$ST/o.err"; or_=$?
    norm "$ST/a.out" > "$ST/a.out.n"; norm "$ST/o.out" > "$ST/o.out.n"
    norm "$ST/a.err" > "$ST/a.err.n"; norm "$ST/o.err" > "$ST/o.err.n"
    total=$((total+1))
    if [ "$ar" = "$or_" ] && diff -q "$ST/a.out.n" "$ST/o.out.n" >/dev/null \
       && diff -q "$ST/a.err.n" "$ST/o.err.n" >/dev/null; then
      printf "  ok    %s\n" "$c"
    else
      diffs=$((diffs+1))
      printf "  DIFF  %-42s rc %d/%d\n" "$c" "$ar" "$or_"
      diff "$ST/a.out.n" "$ST/o.out.n" | rtk sed 's/^/          out| /' | head -6
      diff "$ST/a.err.n" "$ST/o.err.n" | rtk sed 's/^/          err| /' | head -8
    fi
  done
done
echo "=== $((total-diffs))/$total match ==="