#!/bin/bash
# invoke matrix: Apple libxcselect's xcselect_invoke_xcrun vs ours, per dev dir.
# The driver dlopens the library named by DRIVE_LIB (our build) or the Apple
# /usr/lib one, so both sides run the same fixture's libxcrun downstream and
# any difference is libxcselect's own.  The fixture set (dev-dir layouts the
# call must traverse) lives under XS_FIX, default /tmp/dt/xs.
#
# Requires bash: the driver is driven with $() and arrays, and the norm
# pipeline is sed -E.  nproc/malloc noise and timestamps are normalized away.

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIX=${XS_FIX:-/tmp/dt/xs}
FIXR=$(CDPATH= cd -- "$FIX" && pwd -P)
LIB=$REPO/build/release/libxcselect.dylib
IVK=$REPO/build/test/ivk
X=/Applications/Xcode.app/Contents/Developer
C=/Library/Developer/CommandLineTools
ST=$(mktemp -d "${TMPDIR:-/tmp}/ivkmatrix.XXXXXX") || exit 2

norm() { sed -E \
  -e 's#ResultBundle_[A-Za-z0-9._-]+\.xcresult#ResultBundle_X.xcresult#g' \
  -e 's#[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?#TIMESTAMP#g' \
  -e 's#\[[0-9]+:[0-9]+\]#[PID:PID]#g' \
  -e 's#\(errno=[^)]*\)#(errno=X)#g' \
  -e "s#$FIXR#CWD#g" -e "s#$FIX#CWD#g" "$1"; }

CASES=(
  # shim-equivalent calls: tool_name NULL, arguments are the tool's own
  "- --version" "- -h" "- -v" "- --log"
  "- --sdk macosx --show-sdk-path" "- --show-sdk-path"
  "- --find clang" "- --find /usr/bin/clang"
  "- --sdk bogusfoo --find clang"
  # a real tool name (progname), so the unknown-utility handler is installed
  "-v true" "true" "-v /usr/bin/true" "clang --version"
  "nosuchtool"
  # require_xcode: a tool run is refused against a CLT install
  "-X clang"
)

# dev-dir edge cases: paths invoke_xcrun must traverse without a libxcrun
EDGE=(
  "/nonexistent/xcrun"
  "$FIX/devdir"
  "$FIX/fakedd"
  "$FIX/FAKE.app"
)

total=0; diffs=0

run() { # $1=case  $2=devdir  $3=extra-env
  env -i PATH=/usr/bin:/bin $3 DRIVE_DEV="$2" "$IVK" $1 \
    >$ST/a.out 2>$ST/a.err; echo $?
}
run_ours() {
  env -i PATH=/usr/bin:/bin $3 DRIVE_DEV="$2" DRIVE_LIB=$LIB "$IVK" $1 \
    >$ST/o.out 2>$ST/o.err; echo $?
}
cmpcase() { # $1=label  $2=case  $3=devdir  $4=extra-env
  local ar or_
  # warm each side once: Apple's lib answers some bad inputs differently
  # on a cache it has not learned yet, which is not a difference of ours
  run "$2" "$3" "$4" >/dev/null 2>&1
  run_ours "$2" "$3" "$4" >/dev/null 2>&1
  ar=$(run "$2" "$3" "$4")
  or_=$(run_ours "$2" "$3" "$4")
  total=$((total+1))
  norm $ST/a.out > $ST/a.out.n; norm $ST/o.out > $ST/o.out.n
  norm $ST/a.err > $ST/a.err.n; norm $ST/o.err > $ST/o.err.n
  if [ "$ar" = "$or_" ] && diff -q $ST/a.out.n $ST/o.out.n >/dev/null \
     && diff -q $ST/a.err.n $ST/o.err.n >/dev/null; then
    printf "  ok    %-46s rc %s\n" "$1" "$ar"
  else
    diffs=$((diffs+1))
    printf "  DIFF  %-46s rc %s/%s\n" "$1" "$ar" "$or_"
    diff $ST/a.out.n $ST/o.out.n | sed 's/^/          out| /' | head -5
    diff $ST/a.err.n $ST/o.err.n | sed 's/^/          err| /' | head -8
  fi
}

for d in "$X" "$C"; do
  echo "=== $(basename "$d") ==="
  for c in "${CASES[@]}"; do
    # shellcheck disable=SC2086
    cmpcase "$c" "$c" "$d" ""
  done
done

echo "=== dev-dir edges ==="
for f in "${EDGE[@]}"; do
  cmpcase "DRIVE_DEV=$f -" "-" "$f" ""
  cmpcase "DRIVE_DEV=$f -v" "-v" "$f" ""
done
cmpcase "DRIVE_DEV=/nonexistent -X" "-X" "/nonexistent/xcrun" ""

rm -rf "$ST"

echo "=== $((total-diffs))/$total match ==="