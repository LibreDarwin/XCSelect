#!/bin/sh
# Compare /usr/bin/xcode-select against build/release/xcode-select for
# every read-only behavior: -p, -v, -h, --show-manpaths, misuse and
# invalid-argument handling, and the --switch argument validation that
# fails before it can touch any state.  Nothing that writes the
# selection (-s as root, -r, --install without a developer directory) is
# exercised; the write paths are covered by sandbox.sh, which stands on
# the root gate rather than defeating it (see README.md).
#
# The checkout root is where the products live (build/release), and the
# fixtures (FAKE.app and any dev-dir layouts) come from XS_FIX, default
# /tmp/dt/xs.  HOME is not set on either side: the active directory must
# not depend on it.
#
# Columns: case name, env token (E0..E5, resolved below), then the
# arguments.  The heredoc is unquoted so $W expands inside the case
# arguments; the E-tokens are bare names and pass through as literals.

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIX=${XS_FIX:-/tmp/dt/xs}
W=$FIX
APPLE=/usr/bin/xcode-select
OURS=$REPO/build/release/xcode-select

XCODE=/Applications/Xcode.app/Contents/Developer
CLT=/Library/Developer/CommandLineTools

rows=0
fail=0
ST=$(mktemp -d "${TMPDIR:-/tmp}/xsmatrix.XXXXXX") || exit 2

while read -r name envname args; do
    [ -z "$name" ] && continue
    rows=$((rows + 1))
    case $envname in
        E0) envdir= ;;
        E1) envdir="DEVELOPER_DIR=$XCODE" ;;
        E2) envdir="DEVELOPER_DIR=$CLT" ;;
        E3) envdir="DEVELOPER_DIR=/nonexistent" ;;
        E4) envdir="DEVELOPER_DIR=$W/FAKE.app" ;;
        E5) envdir="DEVELOPER_DIR=$W" ;;
        *) echo "bad env $envname in case $name" >&2; exit 2 ;;
    esac
    runside() { env -i PATH=/usr/bin:/bin $envdir "$1" $args; }
    a_out=$(runside "$APPLE" 2>$ST/a_err); a_rc=$?
    a_err=$(cat $ST/a_err)
    o_out=$(runside "$OURS" 2>$ST/o_err); o_rc=$?
    o_err=$(cat $ST/o_err)
    if [ "$a_out" = "$o_out" ] && [ "$a_err" = "$o_err" ] && [ "$a_rc" = "$o_rc" ]; then
        echo "ok   $name"
    else
        fail=$((fail + 1))
        echo "DIFF $name  (rc $a_rc/$o_rc)"
        [ "$a_out" != "$o_out" ] && { echo "   -- apple stdout: [$a_out]"; echo "   -- ours  stdout: [$o_out]"; }
        [ "$a_err" != "$o_err" ] && { echo "   -- apple stderr: [$a_err]"; echo "   -- ours  stderr: [$o_err]"; }
    fi
done <<CASES
noargs        E0
h             E0 -h
help          E0 --help
v             E0 -v
version       E0 --version
p-xcode       E1 -p
printpath     E1 --print-path
p-clt         E2 -p
p-system      E0 -p
p-nonexist    E3 -p
p-fakeapp     E4 -p
p-plaindir    E5 -p
p-p           E1 -p -p
v-p           E0 -v -p
p-v           E0 -p -v
s-missing     E0 -s
s-argopt      E0 -s -p
s-bad         E0 -s /nonexistent
switch-bad    E0 --switch /nonexistent
switch-tmp    E0 --switch /tmp
switch-dir    E0 --switch $W
switch-app    E4 --switch $W/FAKE.app
bogus         E0 --bogus
zflag         E0 -Z
trailing      E0 -p foo
operand       E0 foo
man-xcode     E1 --show-manpaths
man-clt       E2 --show-manpaths
man-system    E0 --show-manpaths
install-has   E1 --install
CASES

rm -rf "$ST"

echo "----"
echo "xcode-select parity: $rows rows, $fail diffs"
[ "$fail" -eq 0 ]