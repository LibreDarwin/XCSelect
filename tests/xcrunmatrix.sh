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

  # A toolchain name is answered with the spelling on disk whatever case
  # it was written in, so these are all the one installed name.  A volume
  # that is case-insensitive -- which is the default -- accepts the wrong
  # case at stat() and would hand it back verbatim, printing a path that
  # resolves by luck of the volume and spells no directory that exists.
  # The suffix is stripped case-insensitively too, so its spelling does not
  # matter either, and asking for the older suffix still finds the bundle.
  "--toolchain xcodedefault --show-toolchain-path"
  "--toolchain XCODEDEFAULT --show-toolchain-path"
  "--toolchain XcOdEdefault --show-toolchain-path"
  "--toolchain xcodeDefault.xctoolchain --show-toolchain-path"
  "--toolchain xcodedefault.toolchain --show-toolchain-path"
  "--toolchain xcodeDefault.XCTOOLCHAIN --show-toolchain-path"
  # ...and the name is canonical where it is used rather than only printed,
  # which is what a --find and a -f both resolve through.
  "--toolchain xcodedefault --find clang"
  "--toolchain xcodedefault -f clang"
  "--toolchain xcodedefault --show-sdk-path"
  # A name that is not installed is still answered with the default, and
  # the case of a name that is not installed is not what decides that.
  "--toolchain BOGUSFOO --show-toolchain-path"
  "--toolchain BOGUSFOO --find clang"

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

  # How the options are ended, and where xcrun's own reading of the line
  # stops.  A "-" and a "--" both end the options rather than being a
  # mistake, and on their own they are the same question with no tool in
  # it, so they are answered as one.  The rows that follow are the
  # difference between the two markers: getopt reads a "--" and stops,
  # and reports a lone "-" as an ordinary word, so the count of what was
  # read is one apart between them -- which is what decides where the
  # tool's own arguments begin.
  "--" "-"
  "-- clang -v" "- clang -v"
  "-- /usr/bin/true a b" "- /usr/bin/true a b"
  "-r /usr/bin/true -- a b" "-r /usr/bin/true - a b"
  "-f clang --" "-f clang -"

  # A second tool name, and a --run that follows a --find.  Both are
  # refusals rather than lookups, so both are answered with rc 64 and
  # name what could not be taken: the second name, or the option that
  # asked for a mode the find had already ruled out.  A name that is
  # only repeated is refused too, since Apple compares the two names and
  # not the two flags, and an empty name is still a name.
  "-f clang extra" "-f clang -" "-f clang -- extra"
  "-f clang -f ld" "-f clang -f clang" "-f clang --find ld"
  "--find clang extra"
  "-f '' -f clang" "-f '' --find clang"
  # ...but the rows whose operand really is empty cannot be written that
  # way, because the loop above splits each case into words and an empty
  # operand is not a word.  They are run from the section after the loop,
  # which passes the arguments as they are written.
  "-f clang -r /usr/bin/true" "-f clang --run /usr/bin/true"
  "-r /usr/bin/true -f clang"

  # What a --show-* does with a word that follows it.  There is nothing
  # to hand it, so the word is named as trailing -- but a tool named by
  # a -f alongside the show is not a leftover word, and a show that
  # arrives with a second name is refused by the find before the show is
  # reached, which is why "--show-sdk-path -f clang extra" names a
  # second utility and not a trailing argument.
  "--show-sdk-path extra" "--show-sdk-path -- clang"
  "--show-sdk-version extra" "--show-toolchain-path extra"
  "--show-sdk-path -f clang" "--show-sdk-path -f clang extra"

  # An option that carries its value with an "=".  Apple takes no option
  # that way, so the whole token is named as an option it does not know,
  # "=" and all.  A "=" inside the value of a separate argument is not
  # the same thing and is left alone: "--sdk macosx=foo" is a lookup of
  # an SDK whose name contains one.  What follows the tool name is the
  # tool's own, so an "=" in a tool's arguments is not this either --
  # and that is the row that catches reading through the tool, since
  # getopt permutes by default and would go back for -D after the name.
  "--sdk=macosx" "--sdk=" "--toolchain=XcodeDefault" "--toolchain="
  "-f=clang" "-r=" "--version=1" "--show-sdk-path=1"
  "-DFOO=bar" "-f clang -DFOO=bar"
  # ...and the two that are the tool's own arguments rather than xcrun's
  # options, which is what stopping at the tool name is for.  ("-v" is
  # left out of this pair: a run of a tool named rather than named by path
  # reports one trace line less than Apple does, which is a pre-existing
  # difference recorded in local/xcselect.md and is not about where the
  # options end.)
  "clang -DFOO=bar" "-k clang -DFOO=bar"

  # An empty --sdk or --toolchain names nothing, so it does not turn the
  # environment or the default off: it is the same answer as not asking.
  # The rows are in the section after the loop, where an empty operand
  # survives to be passed.
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
  # CMP_COLD empties the cache file before each side rather than once before
  # the pair.  Once would not do: the first side to run refills what the
  # other was meant to find cold, and the row would compare a warm run with a
  # cold one and call it a difference in the product.
  #
  # The emptying is done by the *first* runner, and that runner has to be the
  # one whose -k empties the file at all: ours sets the flag that stops it
  # reporting a cached answer but leaves the file exactly as it found it, so
  # a row emptied by it would hand the second side a warm file and quietly
  # stop being cold.  That is a real divergence from the shipped library and
  # it is recorded in local/xcselect.md rather than papered over here -- what
  # this section is checking is which transcript a cold lookup produces, not
  # what -k does to the file, and a row that quietly stopped being cold would
  # be worse than no row.
  [ "${CMP_COLD:-0}" = 1 ] && "$arun" -k >/dev/null 2>&1
  "$arun" "$@" >"$ST/a.out" 2>"$ST/a.err" <"$inf"; local ar=$?
  [ "${CMP_COLD:-0}" = 1 ] && "$arun" -k >/dev/null 2>&1
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

# Warm a row to a state the cache has actually settled into, rather than
# assuming one run was enough.
#
# One run is not enough, and the reason is Apple's, not ours: a run whose SDK
# lookup misses does not always file what it learned.  Six of twelve
# identical cold runs of `--sdk bogusfoo --find clang` left a key behind and
# six did not, so the run after a warm-up can still be the first one to find
# the answer.  A row that compared that against ours -- which was warm, because
# the key it did not write had been written by an earlier run in the pair --
# was measuring which run happened to write the file, not the two
# implementations, and it failed about one time in six.
#
# So the warm-up repeats until two consecutive runs print the same thing,
# which is the same as saying the cache has stopped changing what a run does,
# and it is checked on the normalized transcript because that is what gets
# compared.  Deliberately not a checksum of the cache file: that would need
# this harness to know Apple's key format and file framing, and the property
# under test is that a run's output has stopped moving.  Bounded at four runs,
# and a row that never settles falls through to the comparison anyway rather
# than being skipped -- both sides then see whatever state the cache is
# really in, which is the honest comparison.
#
# diff -q and not cmp -s: cmp is this script's own comparison function, so
# `cmp -s` here would call it with two file paths as its two runners, and it
# would try to execute them.
prime_stable() {
  local arun=$1; shift
  local i prc=""
  for i in 1 2 3 4; do
    "$arun" "$@" >"$ST/pa.raw" 2>"$ST/pe.raw"; local rc=$?
    norm "$ST/pa.raw" >"$ST/pa.out"; norm "$ST/pe.raw" >"$ST/pa.err"
    if [ "$i" -gt 1 ] && [ "$rc" = "$prc" ] \
       && diff -q "$ST/pa.out" "$ST/pb.out" >/dev/null \
       && diff -q "$ST/pa.err" "$ST/pb.err" >/dev/null
    then
      return 0
    fi
    prc=$rc
    cp "$ST/pa.out" "$ST/pb.out"; cp "$ST/pa.err" "$ST/pb.err"
  done
  return 0
}

for d in "$X" "$C"; do
  echo "=== $(basename "$d") ==="
  for c in "${CASES[@]}"; do
    # shellcheck disable=SC2086
    set -- $c
    devd=$d
    # warm plist-sensitive Apple calls; the steady state is the one compared
    prime_stable run_lib "$@"
    cmp "$c" run_lib run_ours_lib "$@"
  done
done

# Cases whose operand is an empty string.  They are here rather than in
# CASES because the loop above turns each case into words, and an empty
# operand is not one of the words it produces: written as "--sdk '' ..." it
# arrives as "--sdk ...", which is a different question entirely and one the
# CASES list already asks.  Each row below is written as the arguments
# themselves, so the empty one is passed as an empty one.
#
# What is being checked is that an empty operand is not mistaken for a
# name.  A name would be resolved and, not existing, would be reported as
# a name that cannot be located -- so the answer to "--sdk ''" is the
# default SDK and the answer to "--toolchain ''" is the default toolchain,
# neither of which is an error at all.  The same applies to a find whose
# tool name is empty: an empty name is still a name, so it counts as the
# one a find is allowed, which is why "-f '' --find clang" is a refusal
# of the second name rather than a lookup of the first.
echo "=== empty operands ==="
for d in "$X" "$C"; do
  devd=$d
  prime_stable run_lib --sdk ""
  cmp "--sdk '' --show-sdk-path" run_lib run_ours_lib --sdk "" --show-sdk-path
  cmp "--sdk '' --find clang" run_lib run_ours_lib --sdk "" --find clang
  cmp "--toolchain '' --find clang" run_lib run_ours_lib --toolchain "" --find clang
  cmp "--toolchain '' --show-toolchain-path" run_lib run_ours_lib --toolchain "" --show-toolchain-path
  # "-f ''" is not here.  An empty name is a name for the purposes of a
  # find, which is the row below, but naming one for a lookup is a case
  # this unit did not touch: Apple answers it from the toolchain's own
  # directory under a full Xcode and reports the name it cannot find under
  # the command line tools, and ours answers the other way round in each.
  # That difference predates the work here and is recorded in
  # local/xcselect.md; a row that cannot pass yet does not belong in a
  # matrix whose every row is supposed to match.
  cmp "-f '' --find clang" run_lib run_ours_lib -f "" --find clang
done

# Every row above compares a warm Apple against ours, because the warm-up
# above primes Apple's side first.  That is what makes them stable, and it is
# also why they cannot see a lookup that reports where its answer came from:
# Apple is never the cold side of the comparison, and ours reported a
# database answer for every lookup whether or not the file had one.  Those two
# facts are the same fact -- nothing in the file had to be true for ours to
# say so -- and nothing in the rows above could have told.
#
# "Warm" here is a state prime_stable verified rather than one warm-up run
# assumed, which it has to be: a single run leaves Apple's own cache in a state
# that decides the next row.
#
# So the cold half is here, where the file is emptied before each side and
# both are made to answer the same question from nothing.  A lookup answered
# from the file names the file; one that had to ask xcodebuild says what it
# asked and what came back; one answered by walking a flat layout's
# directories says nothing at all, because that is not a question anyone
# asked.  Those are three different transcripts for one query, and which one
# a run produces is the whole of what these rows check.
echo "=== cold cache, emptied before each side ==="
CMP_COLD=1
for d in "$X" "$C"; do
  echo "=== $(basename "$d") ==="
  for c in "-v --find clang" "-v --find ld" "-v --find cups-config" \
           "-v --find definitely-not-a-tool" "--find clang" \
           "-v --sdk macosx --find clang" "-v --sdk bogusfoo --find clang" \
           "-v --kill-cache --find clang" "-v --no-cache --find clang"; do
    # shellcheck disable=SC2086
    set -- $c
    devd=$d
    cmp "$c" run_lib run_ours_lib "$@"
  done
done
unset CMP_COLD

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