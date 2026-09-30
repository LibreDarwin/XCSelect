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
# This is the exploratory suite, not part of `make check`: it is a wide
# per-argument surface rather than a regression net, and it needs both a
# full Xcode and the command line tools installed to be meaningful.  Every
# case in it matches Apple's transcript.  See README.md and
# local/xcselect.md for the per-family notes.

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIX=${XS_FIX:-/tmp/dt/xs}
FIXR=$(CDPATH= cd -- "$FIX" && pwd -P)
LIB=$REPO/build/release/libxcrun.dylib
DRIVE=$REPO/build/test/xcrundrive
XCRUN=$REPO/build/release/xcrun
X=/Applications/Xcode.app/Contents/Developer
C=/Library/Developer/CommandLineTools
ST=$(mktemp -d "${TMPDIR:-/tmp}/xcrunmatrix.XXXXXX") || exit 2
trap 'rm -rf "$ST"' EXIT

norm() { sed -E \
  -e 's#ResultBundle_[0-9-]+_[0-9-]+\.xcresult#ResultBundle_X.xcresult#g' \
  -e 's#[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?#TIMESTAMP#g' \
  -e 's#xcodebuild\[[0-9]+:[0-9]+\]#xcodebuild[PID:THR]#g' \
  -e "s#$FIXR#CWD#g" -e "s#$FIX#CWD#g" "$1"; }

CASES=(
  "--version" "-h" "--help"
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
  "--run /usr/bin/true" "--run /usr/bin/false" "--run nosuchtool-xyz"

  # --toolchain, and --log outside the verbose family.  These two flags
  # were the only ones in Apple's option list with almost no coverage here
  # -- --help and --run had none at all -- which is how a difference in
  # both of them went unnoticed: a toolchain name that is not installed
  # is answered by Apple with the default toolchain, and --run is where
  # --log actually has something to print.  Both are now in the matrix;
  # the cases that catch those two differences were added with the fixes.
  "-l --find clang" "-l --show-sdk-version" "-l --show-sdk-build-version"
  "--log --show-sdk-build-version"
  "--toolchain XcodeDefault --show-toolchain-path"
  "--toolchain XcodeDefault --find clang"
  "--toolchain XcodeDefault.xctoolchain --find clang"
  "--toolchain XcodeDefault -f clang"
  "--toolchain XcodeDefault --run /usr/bin/true"
  "--toolchain bogusfoo --show-sdk-path"
  # ...and the two that Apple answers with the default toolchain rather
  # than with the name that was asked for.  --log's line is lost to stdio
  # buffering if it is written to stdout and then execve'd over, so these
  # two only pass when it goes to stderr.
  "--toolchain bogusfoo --show-toolchain-path"
  "--toolchain bogusfoo --find clang"
  "--log --run /usr/bin/true"
  "-l -r /usr/bin/true"

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

# One comparison, so the pass/fail rule and the label width live in one
# place rather than in every section that needs them.  $1 is the label to
# print, $2 and $3 are the two runners, and the rest is the command; the
# sections below differ only in which pair of runners they name, which is
# the whole point of naming them here.
cmp() {
  local label=$1 arun=$2 orun=$3
  shift 3
  "$arun" "$@" >"$ST/a.out" 2>"$ST/a.err"; local ar=$?
  "$orun" "$@" >"$ST/o.out" 2>"$ST/o.err"; local or_=$?
  norm "$ST/a.out" > "$ST/a.out.n"; norm "$ST/o.out" > "$ST/o.out.n"
  norm "$ST/a.err" > "$ST/a.err.n"; norm "$ST/o.err" > "$ST/o.err.n"
  total=$((total+1))
  if [ "$ar" = "$or_" ] && diff -q "$ST/a.out.n" "$ST/o.out.n" >/dev/null \
     && diff -q "$ST/a.err.n" "$ST/o.err.n" >/dev/null; then
    printf "  ok    %-46s rc %s\n" "$label" "$ar"
  else
    diffs=$((diffs+1))
    printf "  DIFF  %-46s rc %s/%s\n" "$label" "$ar" "$or_"
    diff "$ST/a.out.n" "$ST/o.out.n" | rtk sed 's/^/          out| /' | head -6
    diff "$ST/a.err.n" "$ST/o.err.n" | rtk sed 's/^/          err| /' | head -8
  fi
}

# Through the driver, into libxcrun, with a developer directory libxcselect
# has already vouched for -- which is the only way libxcrun is called in
# anger, and why $X and $C are the only directories it is given below.
run_lib() { DEVELOPER_DIR=$devd /usr/bin/xcrun "$@"; }
run_ours_lib() { DRIVE_DEV=$devd DRIVE_LIB=$LIB "$DRIVE" "$@"; }

for d in "$X" "$C"; do
  echo "=== $(basename "$d") ==="
  for c in "${CASES[@]}"; do
    # shellcheck disable=SC2086
    set -- $c
    devd=$d
    # warm plist-sensitive Apple calls; the second run is the steady state
    run_lib "$@" >/dev/null 2>&1
    cmp "$c" run_lib run_ours_lib "$@"
  done
done

# The rows above reach libxcrun directly, so they cannot see a developer
# directory being rejected: that check is in libxcselect, one layer up, and
# the only thing that exercises it is the shipped xcrun binary.  Nothing
# compared that binary to Apple's before, which is a real gap rather than a
# hypothetical one -- a stale DEVELOPER_DIR is an ordinary event (an
# unmounted volume, a renamed Xcode, an environment baked at image-build
# time) and it is answered with a specific message and a specific exit
# status, which scripts do branch on.
#
# These pass today.  They are here so that they say so, and so that a change
# to that message cannot go unnoticed the way the two differences in the
# previous two commits did.
#
# The selection-sourced form of the same complaint -- "active developer"
# rather than "DEVELOPER_DIR", when the value came from xcode-select -s
# rather than the environment -- is not here, because reaching it means
# moving the real selection, which none of these tests do.
echo "=== shipped xcrun, bad DEVELOPER_DIR ==="
run_bin() { DEVELOPER_DIR=$devd /usr/bin/xcrun "$@"; }
run_ours_bin() { DEVELOPER_DIR=$devd "$XCRUN" "$@"; }
for c in "--version" "--show-sdk-path" "--find clang" "--help"; do
  # shellcheck disable=SC2086
  set -- $c
  for devd in /nonexistent/DD /tmp; do
    cmp "DEVELOPER_DIR=$devd $c" run_bin run_ours_bin "$@"
  done
done

echo "=== shipped xcrun, DEVELOPER_DIR empty or unset ==="
run_bin_n() { env -u DEVELOPER_DIR /usr/bin/xcrun "$@"; }
run_ours_bin_n() { env -u DEVELOPER_DIR "$XCRUN" "$@"; }
for c in "--version" "--show-sdk-path" "--find clang"; do
  # shellcheck disable=SC2086
  set -- $c
  # An empty DEVELOPER_DIR is not a path that happens not to exist, it is
  # no setting at all, and both sides have to fall through to the selection.
  devd=''
  cmp "DEVELOPER_DIR='' $c" run_bin run_ours_bin "$@"
  # ...and since the real selection is never touched, unset and empty are
  # expected to be the same answer rather than a coincidence.
  cmp "DEVELOPER_DIR unset $c" run_bin_n run_ours_bin_n "$@"
done

echo "=== $((total-diffs))/$total match ==="
[ "$diffs" -eq 0 ]