#!/bin/sh
# The write side of xcode-select, in a sandbox.
#
# -s and -r are the only things this tool does that change state, and they
# change the state the entire system reads to decide which compiler runs.
# That is why they are gated on geteuid() == 0, and why they have never
# been in a matrix: there is no way to run them from a normal account
# without either sudo, which this environment does not have, or editing
# the gate out of the source, which would mean the thing under test is no
# longer the thing that ships.
#
# The gate is not defeated here, it is stood on.  A second xcode-select and
# a second libxcselect are compiled from the same sources with
# XC_SELECT_WRITE_ROOT set to a scratch directory, which lowers all four
# write paths (the three data links and the data file) under it, and the
# tool is linked with rootstub.o, whose geteuid() returns zero.  So the
# check does the write, the gate does its job, and the only bytes anywhere
# near it are in a mktemp directory that is removed on the way out.
# Nothing outside it is read or written, and the real /var/select, /var/db
# and /usr/share selection is not touched -- which is also why this cannot
# prove anything about the real paths, only about the code that uses them.
#
# Both halves are recompiled, not just the writer.  The reader needs the
# macro too, or -p has no way to look at the sandbox: DEVELOPER_DIR is not
# a substitute, because it overrides the selection outright, so -p would
# echo it back and the run would prove nothing.  Recompiling the reader is
# what makes the read-back below an actual read of what was written.
#
# The sandbox needs the four parent directories to exist, because the code
# links the files into paths that are not created for it; the real ones
# are made by the installer.  -s validates the directory it is given, so
# the target has to be a developer directory: the FAKE.app under XS_FIX is
# used when it is there, and one is built in the scratch directory if not.
#
# Runs as a normal user.  Needs the same SDK and CC as the products, or
# set XC_TEST_SDK / XC_TEST_CC to override.

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIX=${XS_FIX:-/tmp/dt/xs}
BIN=$REPO/build/test
CONFIG=${CONFIG:-release}
# Same defaults as the top-level Makefile, overridable for a host whose
# toolchain lives elsewhere.
SDK=${XC_TEST_SDK:-$(sed -n 's/^SDK *?*= *//p' "$REPO/Makefile" | head -1)}
CC=${XC_TEST_CC:-$(sed -n 's/^CC *:= *//p' "$REPO/Makefile" | head -1)}
OPT=$(sed -n 's/^OPT *:= *//p' "$REPO/make/$CONFIG.mk" 2>/dev/null)

if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
    echo "sandbox: no SDK; set XC_TEST_SDK" >&2
    exit 2
fi
if [ -z "$CC" ] || [ ! -x "$CC" ]; then
    echo "sandbox: no compiler; set XC_TEST_CC" >&2
    exit 2
fi

ST=$(mktemp -d "${TMPDIR:-/tmp}/xssandbox.XXXXXX") || exit 2
trap 'rm -rf "$ST"' EXIT
SB=$ST/root
WS=$BIN/xcode-select-sandbox
real=$REPO/build/$CONFIG/xcode-select

# -p with DEVELOPER_DIR removed from the environment.  It has to be, or
# every read row in this script is vacuous: DEVELOPER_DIR overrides the
# selection outright, so -p would echo the caller's own value and agree
# with the expected one no matter where the four paths point.  That is
# the same reason the macro exists rather than the environment being
# leaned on, and it would have made the read-back below pass for the
# wrong reason on any machine whose CI exports it.
runp() { env -u DEVELOPER_DIR "$1" -p 2>&1; }

rows=0
fail=0

say() { echo "$1"; }
ok() { rows=$((rows + 1)); echo "ok   $1"; }
bad() {
    rows=$((rows + 1))
    fail=$((fail + 1))
    echo "DIFF $1"
    shift
    for line in "$@"; do echo "   -- $line"; done
}

# Builds the sandboxed tool.  The write root is absolute so the four
# lowered paths are the same strings the real tool would use with a root
# prepended, and the links it makes can be read back and compared.
#
# Both halves of the read/write pair are rebuilt against the same root:
# libxcselect is compiled here rather than linked from build/release,
# because the reader's own copy of the four paths is what -p consults, and
# pointing only the writer at the sandbox would leave -p reading the
# system's real selection -- a comparison that would pass while proving
# nothing.
mkdir -p "$BIN" || exit 2
WROOT=-DXC_SELECT_WRITE_ROOT="\"$SB\""
"$CC" $OPT \
    -std=c11 -D_DARWIN_C_SOURCE -isysroot "$SDK" -Wall -Wextra \
    -Wno-unused-parameter -fblocks \
    -I "$REPO/src/common" -I "$REPO/src/libxcselect" \
    "$WROOT" -dynamiclib -o "$BIN/libxcselect-sandbox.dylib" \
    "$REPO/src/libxcselect/libxcselect.c" 2>$ST/cc.err
if [ $? -ne 0 ]; then
    echo "sandbox: library build failed" >&2
    cat $ST/cc.err >&2
    exit 2
fi

"$CC" $OPT \
    -std=c11 -D_DARWIN_C_SOURCE -isysroot "$SDK" -Wall -Wextra \
    -Wno-unused-parameter -fblocks \
    -I "$REPO/src/common" -I "$REPO/src/libxcselect" \
    "$WROOT" \
    -o "$WS" "$REPO/src/xcode-select/xcode-select.c" \
    "$REPO/tests/rootstub.c" \
    -L "$BIN" -lxcselect-sandbox \
    -Wl,-rpath,"$BIN" 2>$ST/cc.err
if [ $? -ne 0 ]; then
    echo "sandbox: build failed" >&2
    cat $ST/cc.err >&2
    exit 2
fi

# The four write paths, in the order xcode-select lays them down.
LINK1=$SB/var/select/developer_dir
LINK2=$SB/var/db/xcode_select_link
LINK3=$SB/usr/share/xcode-select/xcode_dir_link
DATA=$SB/usr/share/xcode-select/xcode_dir_path

mkroot() {
    rm -rf "$SB"
    mkdir -p "$SB/var/select" "$SB/var/db" "$SB/usr/share/xcode-select" || return 1
    return 0
}

# What is in the sandbox right now, as one comparable line each.
inspect() {
    for p in "$LINK1" "$LINK2" "$LINK3"; do
        if [ -L "$p" ]; then
            echo "link $(basename "$p") -> $(readlink "$p")"
        elif [ -e "$p" ]; then
            echo "notalink $(basename "$p")"
        else
            echo "absent $(basename "$p")"
        fi
    done
    if [ -f "$DATA" ]; then
        # The trailing newline is part of what is written, so show it.
        echo "data xcode_dir_path [$(cat "$DATA")] bytes=$(wc -c < "$DATA" | tr -d ' ')"
    else
        echo "absent xcode_dir_path"
    fi
}

# -s validates the path it is given before writing anything, so a target
# has to be a directory libxcselect recognizes.  The fixture root's
# FAKE.app is used when it is there; otherwise one is built here, because
# a harness that only runs on a machine that has already been set up by
# hand is not a harness.  The shape is the smallest that passes: a
# Contents/Developer that exists, since that is what
# xcselect_find_developer_contents_from_path looks for.
TARGET=$FIX/FAKE.app
if [ ! -d "$TARGET/Contents/Developer" ]; then
    TARGET=$ST/FAKE.app
    mkdir -p "$TARGET/Contents/Developer" || exit 2
fi

# 1. A fresh sandbox has nothing in it, and -r on that is a no-op that
#    still succeeds: clearing what is not there is clearing it.
mkroot || exit 2
if out=$("$WS" -r 2>&1) && [ -z "$out" ]; then
    ok "reset-on-empty is quiet and succeeds"
else
    bad "reset-on-empty is quiet and succeeds" "rc/out: [$out]"
fi

# 2. -s lays down exactly four things: three links pointing at the
#    resolved developer directory, and the path in the data file.
mkroot || exit 2
if out=$("$WS" -s "$TARGET" 2>&1) && [ -z "$out" ]; then
    got=$(inspect)
    # The data file is the path and exactly one trailing newline, which is
    # the newline libxcselect's reader strips, so the byte count is the
    # path's length plus one and not one byte more.
    want="link developer_dir -> $TARGET/Contents/Developer
link xcode_select_link -> $TARGET/Contents/Developer
link xcode_dir_link -> $TARGET/Contents/Developer
data xcode_dir_path [$TARGET/Contents/Developer] bytes=$(( ${#TARGET} + 19 + 1 ))"
    if [ "$got" = "$want" ]; then
        ok "switch writes three links and the data file"
    else
        bad "switch writes three links and the data file" \
            "got:  [$got]" "want: [$want]"
    fi
else
    bad "switch writes three links and the data file" "rc/out: [$out]"
fi

# 3. The tool reads its own selection back.  This is the row the whole
#    harness exists for: -s and -p are the two halves of one contract,
#    and a -s that succeeded while -p disagreed would leave the system
#    selecting something nobody asked for.  Both halves read and write the
#    same sandbox root here, so this is a real read-back and not a
#    comparison of two hardcoded strings.
mkroot || exit 2
"$WS" -s "$TARGET" >/dev/null 2>&1
got=$(runp "$WS")
want=$TARGET/Contents/Developer
if [ "$got" = "$want" ]; then
    ok "print-path reads back what switch wrote"
else
    bad "print-path reads back what switch wrote" \
        "got: [$got]" "want: [$want]"
fi

# 3b. The two lists are still compared directly, because the read-back
#     above would pass even if both sides moved to a fifth path together:
#     the read-back proves the halves agree, not that they are the paths
#     the system uses.  Only this does that.
#     the paths the system uses.  Only this does that.
#
# Each side's four paths are read out of its own source -- the three in
# dev_dir_links and the one behind the data-file macro -- and reduced to
# basenames, since a macro only prepends a root and renames nothing.  The
# system paths are spelled out here rather than taken from either source,
# so a path edited on one side and copied to the other is caught instead
# of being confirmed twice.
paths_of() {
    sed -n '/dev_dir_links\[\] *= *{/,/};/p' "$1" |
        sed -n 's/.*WRITE_ROOT *"\([^"]*\)".*/\1/p'
    grep -A1 '^#define *\(XC_SELECT_DEV_DIR_FILE\|XCSELECT_DEV_DIR_FILE\)' "$1" |
        sed -n 's/.*WRITE_ROOT *"\([^"]*\)".*/\1/p'
}
basenames() { while read -r p; do basename "$p"; done | tr '\n' ' '; }
w=$(paths_of "$REPO/src/xcode-select/xcode-select.c" | basenames)
r=$(paths_of "$REPO/src/libxcselect/libxcselect.c" | basenames)
sys="developer_dir xcode_select_link xcode_dir_link xcode_dir_path "
if [ "$w" = "$r" ] && [ -n "$w" ] && [ "$w" = "$sys" ]; then
    ok "both halves name the same four system paths"
else
    bad "both halves name the same four system paths" \
        "writer: [$w]" "reader: [$r]" "system: [$sys]"
fi

# 4. A second -s replaces the first, and does not accumulate: the links
#    are unlinked first, so a stale link pointing at the old directory is
#    not left behind beside the new one.
mkroot || exit 2
"$WS" -s "$TARGET" >/dev/null 2>&1
if out=$("$WS" -s "$TARGET" 2>&1) && [ -z "$out" ]; then
    n=$(inspect | grep -c '^link ')
    if [ "$n" -eq 3 ]; then
        ok "second switch replaces rather than accumulates"
    else
        bad "second switch replaces rather than accumulates" "links: $n"
    fi
else
    bad "second switch replaces rather than accumulates" "rc/out: [$out]"
fi

# 5. -r clears all four and is itself a clearing: it does not write a
#    default anywhere, since nothing being selected is what makes the
#    library fall back.
mkroot || exit 2
"$WS" -s "$TARGET" >/dev/null 2>&1
before=$(inspect | grep -c 'link \|^data ')
if out=$("$WS" -r 2>&1) && [ -z "$out" ]; then
    got=$(inspect)
    want="absent developer_dir
absent xcode_select_link
absent xcode_dir_link
absent xcode_dir_path"
    if [ "$got" = "$want" ]; then
        ok "reset clears all four and writes nothing ($before before)"
    else
        bad "reset clears all four and writes nothing" \
            "got: [$got]" "want: [$want]"
    fi
else
    bad "reset clears all four and writes nothing" "rc/out: [$out]"
fi

# 5b. With nothing selected, the sandboxed tool and the shipped one must
#     report the same thing, and it must not come from the sandbox.  This
#     is the row that keeps the read-back above honest: it compares against
#     the shipped build rather than against a hardcoded path, so it says
#     something on any machine.  An earlier version asserted only that the
#     output did not start with the sandbox root, which passed on this
#     host whether or not the macro was applied at all, because there is
#     no selection in /var/select here to pick up either way.
mkroot || exit 2
got=$(runp "$WS")
if [ -x "$real" ]; then
    want=$(runp "$real")
    if [ "$got" = "$want" ]; then
        ok "an empty sandbox agrees with the shipped build's -p"
    else
        bad "an empty sandbox agrees with the shipped build's -p" \
            "sandbox: [$got]" "shipped: [$want]"
    fi
else
    # No shipped build to compare against.  Say so rather than passing a
    # weaker assertion: this row is only worth anything as a comparison.
    say "note  build/$CONFIG/xcode-select absent; empty-sandbox -p not compared"
fi

# 6. -s refuses a path that is not a developer directory, and refuses it
#    before writing: the sandbox is untouched by a rejected switch.
mkroot || exit 2
"$WS" -s /nonexistent >/dev/null 2>&1
got=$(inspect)
if [ "$got" = "absent developer_dir
absent xcode_select_link
absent xcode_dir_link
absent xcode_dir_path" ]; then
    ok "a rejected switch writes nothing"
else
    bad "a rejected switch writes nothing" "got: [$got]"
fi

# 7. A second developer directory, switched to from the first, replaces
#    it in both places.  The sandbox's own directory is used rather than
#    the fixture's so the two are certainly different, and it is given the
#    xcrun that makes a directory a developer directory -- the same thing
#    -s validates before it writes anything, and a directory without it
#    is refused, which case 6 covers.
mkroot || exit 2
"$WS" -s "$TARGET" >/dev/null 2>&1
ALT=$SB/alt
mkdir -p "$ALT/usr/bin" || exit 2
printf '#!/bin/sh\nexit 0\n' > "$ALT/usr/bin/xcrun"
chmod +x "$ALT/usr/bin/xcrun"
if out=$("$WS" -s "$ALT" 2>&1) && [ -z "$out" ]; then
    got=$(readlink "$LINK1" 2>/dev/null)
    data=$(cat "$DATA" 2>/dev/null)
    if [ "$got" = "$ALT" ] && [ "$data" = "$ALT" ]; then
        ok "links and data file agree on the second directory"
    else
        bad "links and data file agree on the second directory" \
            "link [$got] data [$data]" "want [$ALT]"
    fi
else
    bad "links and data file agree on the second directory" "rc/out: [$out]"
fi

# 8. The gate is real: the shipped build still refuses, as a normal user,
#    and says so the way Apple does.  This is the whole reason the
#    sandbox exists, so it is checked rather than assumed.
if [ -x "$real" ]; then
    got=$("$real" -s "$TARGET" 2>&1)
    case $got in
        *"must be"*"run as root"*) ok "the shipped build still refuses -s" ;;
        *) bad "the shipped build still refuses -s" "got: [$got]" ;;
    esac
    got=$("$real" -r 2>&1)
    case $got in
        *"must be"*"run as root"*) ok "the shipped build still refuses -r" ;;
        *) bad "the shipped build still refuses -r" "got: [$got]" ;;
    esac
else
    say "note  build/$CONFIG/xcode-select absent; gate not checked"
fi

rm -rf "$SB"

echo "----"
echo "xcode-select write sandbox: $rows rows, $fail diffs"
[ "$fail" -eq 0 ]
