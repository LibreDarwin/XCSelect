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

is_text() { [ "$(LC_ALL=C tr -cd '\000' < "$1" | wc -c | tr -d ' ')" -eq 0 ]; }

# The bundle name is ResultBundle_<date>_<time>-<subsecond>.xcresult, and the
# subsecond part is what makes two runs of the same command name two
# different files.  The rule that used to try to hide that read
# ResultBundle_[0-9-]+_[0-9]+\.xcresult, which cannot match: [0-9-] does
# not contain _, so the split has to happen at the first _, and then the
# dashes in the time part stop [0-9]+.  It had never fired.  The rows that
# depend on it were passing only because both sides happened to land in the
# same thousandth, which is luck, and adding anything to the harness is
# enough to change that -- sixteen rows of the 192 were a coin toss.
norm() {
  is_text "$1" || { cat "$1"; return; }
  sed -E \
  -e 's#ResultBundle_[0-9-]+(_[0-9-]+)*\.xcresult#ResultBundle_X.xcresult#g' \
  -e 's#[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?#TIMESTAMP#g' \
  -e 's#xcodebuild\[[0-9]+:[0-9]+\]#xcodebuild[PID:THR]#g' \
  -e 's#^.*(Terminated|Killed): [0-9]+.*$#JOB NOTIFICATION#' \
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
#
# $CMP_STDIN, when set, names a file that both sides get as their standard
# input.  It is a file name and not a descriptor because each side has to
# open it for itself: sharing one descriptor lets the first side consume
# the bytes and leaves the second reading EOF, which then reads as a
# difference in the tool rather than in the harness.  Unset, a case gets
# /dev/null, so no case can inherit this script's own stdin by accident.
cmp() {
  local label=$1 arun=$2 orun=$3
  shift 3
  local inf=${CMP_STDIN:-/dev/null}
  "$arun" "$@" >"$ST/a.out" 2>"$ST/a.err" <"$inf"; local ar=$?
  "$orun" "$@" >"$ST/o.out" 2>"$ST/o.err" <"$inf"; local or_=$?
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

# ---------------------------------------------------------------------------
# What a tool run through --run actually inherits.  --run is the one option
# that does not answer a question but replaces the process, so everything
# about the replacement belongs to libxcrun: the environment it hands over,
# the descriptor the caller piped in, the working directory, the umask, and
# the status the calling shell ends up with.  None of that had a committed
# row.  It had all been checked by hand, and by hand is not the same as
# being covered.
#
# These go through the driver, which is the only way to be sure that ours
# is the libxcrun under test.  build/release/xcrun is the shim the way
# /usr/bin/xcrun is: it links only libxcselect, which dlopens
# <developer dir>/usr/lib/libxcrun.dylib.  Pointed at a real Xcode it
# therefore loads *Apple's* libxcrun, and a row run that way compares
# Apple's implementation against itself and passes by saying nothing.  The
# two bad-DEVELOPER_DIR sections below are the exception, and rightly so:
# that complaint comes from libxcselect, one layer up, and no libxcrun
# would ever be reached to answer it.
#
# Both sides are handed the same environment by construction -- env -i,
# then the same variables -- so whatever appears in the child's
# environment is something the tool put there rather than something the
# shell happened to be carrying.  The driver takes its own two variables
# out of the environment before calling in, so they are not among them.
run_lib_p() { env -i PATH=/usr/bin:/bin HOME=/tmp TMPDIR="$ST/tmp" \
    XSMATRIX_PROBE=probe-42 DEVELOPER_DIR="$devd" /usr/bin/xcrun "$@"; }
run_ours_lib_p() { env -i PATH=/usr/bin:/bin HOME=/tmp TMPDIR="$ST/tmp" \
    XSMATRIX_PROBE=probe-42 DEVELOPER_DIR="$devd" \
    DRIVE_DEV="$devd" DRIVE_LIB="$LIB" "$DRIVE" "$@"; }

# Two inputs: text with no trailing newline, and something sed must not be
# allowed near.  The second is the reason norm() above checks for NUL.
printf 'line one\nline two\nno trailing newline' >"$ST/in.text"
printf 'a\0b\0c\n' >"$ST/in.bin"
mkdir -p "$ST/tmp" || exit 2

for devd in "$X" "$C"; do
  echo "=== --run inherits ($(basename "$devd")) ==="
  # Standard input reaches the tool because nothing intercepts it, and each
  # side is given the same bytes to read rather than a shared descriptor.
  CMP_STDIN=$ST/in.text \
    cmp "stdin: text piped through --run" run_lib_p run_ours_lib_p --run /bin/cat
  cmp "stdin: nothing piped" run_lib_p run_ours_lib_p --run /bin/cat
  CMP_STDIN=$ST/in.bin \
    cmp "stdin: NUL bytes" run_lib_p run_ours_lib_p --run /bin/cat
  # The tool is replaced, so the status the shell sees is the tool's own --
  # not a status xcrun decided on its behalf.  A signal is reported as the
  # shell reports it, 128+signo, which is the case that would catch a
  # wrapper turning a killed tool into a clean zero.
  cmp "status: exit 42" run_lib_p run_ours_lib_p --run /bin/sh -c 'exit 42'
  cmp "status: killed by SIGTERM" run_lib_p run_ours_lib_p \
    --run /bin/sh -c 'kill -TERM $$'
  cmp "status: killed by SIGKILL" run_lib_p run_ours_lib_p \
    --run /bin/sh -c 'kill -KILL $$'
  # ...and the two things the tool is run in rather than given.
  cmp "inherited: working directory" run_lib_p run_ours_lib_p --run /bin/pwd
  cmp "inherited: umask" run_lib_p run_ours_lib_p --run /bin/sh -c umask
  # The whole environment, sorted, because the order the entries happen to
  # be built in is not a contract and comparing it would be a row that
  # fails for no reason.  This is the row that catches a variable xcrun
  # invented, or one it dropped on the floor.
  cmp "inherited: whole environment" run_lib_p run_ours_lib_p \
    --run /bin/sh -c 'env | sort'
  # ...and the same facts one at a time, so a failure names which variable
  # rather than making it be diffed out of a list.
  for v in SDKROOT DEVELOPER_DIR MANPATH PATH HOME XSMATRIX_PROBE; do
    cmp "inherited: \$$v" run_lib_p run_ours_lib_p \
      --run /bin/sh -c "printf %s \"\$$v\""
  done
  # PATH is never added to by either side: the caller's PATH is the tool's
  # PATH.  A tool that wanted the developer directory on its PATH would have
  # to be found there in the first place.
  cmp "inherited: PATH is not rewritten" run_lib_p run_ours_lib_p \
    --run /bin/sh -c 'printf %s "$PATH"'
  unset CMP_STDIN
done

echo "=== $((total-diffs))/$total match ==="
[ "$diffs" -eq 0 ]