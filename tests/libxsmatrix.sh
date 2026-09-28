#!/bin/sh
# Differential matrix for the libxcselect exports that are not reached
# by exercising xcrun or xcode-select as commands: run the same direct
# call against Apple's /usr/lib/libxcselect.dylib and our build/release
# copy and diff the transcripts.  Every call is read-only.
# trigger_install_request is deliberately absent (it would ask the
# system to put an installer up) and invoke_xcrun lives in
# ivkmatrix.sh, which drives it through libxcselect with real tool runs.
#
# The checkout root is where the products live (build/release), the
# drivers under build/test, and the fixtures (FAKE.app) under XS_FIX,
# default /tmp/dt/xs.
#
# Columns: case name, DEVELOPER_DIR value (NONE for unset), then the
# driver op and its arguments.  The heredoc is unquoted so $W, $DD and
# $CLT expand inside both the env column and the arguments.

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIX=${XS_FIX:-/tmp/dt/xs}
W=$FIX
APPLE=/usr/lib/libxcselect.dylib
OURS=$REPO/build/release/libxcselect.dylib
TEST=$REPO/build/test/libxstest
DD=/Applications/Xcode.app/Contents/Developer
CLT=/Library/Developer/CommandLineTools
ARS=$FIX/FAKE.app/Contents/Developer

rows=0
fail=0

while read -r name envstr args; do
    [ -z "$name" ] && continue
    rows=$((rows + 1))
    [ "$envstr" = NONE ] && envstr=
    runside() {
        if [ -n "$envstr" ]; then
            env -i PATH=/usr/bin:/bin HOME="$FIX" DEVELOPER_DIR="$envstr" "$1" "$2" $args 2>&1
        else
            env -i PATH=/usr/bin:/bin HOME="$FIX" "$1" "$2" $args 2>&1
        fi
    }
    a_out=$(runside "$TEST" "$APPLE"); a_rc=$?
    o_out=$(runside "$TEST" "$OURS"); o_rc=$?
    if [ "$a_out" = "$o_out" ] && [ "$a_rc" = "$o_rc" ]; then
        echo "ok   $name"
    else
        fail=$((fail + 1))
        echo "DIFF $name  (rc $a_rc/$o_rc)"
        echo "$a_out" | sed 's/^/   -- apple: /'
        echo "$o_out" | sed 's/^/   -- ours : /'
    fi
done <<CASES
devdir-unset    NONE           devdir
devdir-xcode    $DD            devdir
devdir-clt      $CLT           devdir
devdir-nonexist /nonexistent   devdir
devdir-fakeapp  $ARS           devdir
devdir-plaindir $W             devdir
match-active-x  $DD            match NIL $DD/usr/bin/xcrun
match-active-c  $CLT           match NIL $CLT/usr/bin
match-explicit  $DD            match $DD $DD/usr
match-explicitc $CLT           match $CLT $CLT/usr/bin
match-bundleeq  $DD            match $DD/Contents/Developer $DD
match-outside   $DD            match $DD /tmp
match-nodir-tt  $DD            match NIL /tmp
match-clt-xcod  $CLT           match $CLT $DD
find-xcode      NONE           find $DD
find-clt        NONE           find $CLT
find-bundle     NONE           find /Applications/Xcode.app
find-nonexist   NONE           find /nonexistent
find-plaindir   NONE           find $W
find-usr        NONE           find /usr
version         NONE           version
hostsdk-match   NONE           hostsdk 1
hostsdk-only    NONE           hostsdk 2
hostsdk-newest  NONE           hostsdk 3
hostsdk-zero    NONE           hostsdk 0
hostsdk-four    NONE           hostsdk 4
hostsdk-cltl    $CLT           hostsdk 3
bundle-xcode    NONE           bundle com.apple.dt.Xcode
bundle-clt      NONE           bundle com.apple.dt.CommandLineTools
bundle-sim      NONE           bundle com.apple.iphonesimulator
bundle-other    NONE           bundle com.apple.Finder
bundle-short    NONE           bundle com.x
bundle-prefix   NONE           bundle com.apple.
bundle-nil      NONE           bundle NIL
man-xcode       $DD            manpaths
man-clt         $CLT           manpaths
CASES

echo "----"
echo "libxcselect direct parity: $rows rows, $fail diffs"
[ "$fail" -eq 0 ]