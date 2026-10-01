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
#
# The argument column is word-split when the command is built, and nothing
# removes quotes afterwards, so an empty argument cannot be written here:
# -s '' would reach the tool as the two characters ''.  --toolchain '' and
# -s '' are therefore covered by hand rather than by a row, and the gap
# closes if the format ever grows a second column for the empty case.

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

# Refuse to run as root.  The --switch rows in the case table name
# directories that a root run would accept, so running this file as root
# would move the real selection -- which is exactly the state this matrix
# is supposed to leave alone to compare it.  The write paths are
# sandbox.sh's job and it gets past the root gate honestly, by linking
# ours against a rootstub that reports a root euid and by pointing the
# write root at a scratch tree.  Read-only coverage has no business being
# the thing that breaks.
if [ "$(id -u)" -eq 0 ]; then
    echo "xsmatrix: refusing to run as root: the --switch rows would change" \
         "the real selection (run tests/sandbox.sh for the write paths)" >&2
    exit 2
fi

# Two shapes for --switch that the fixture root does not have: a plain
# file, and a symlink resolving to a real developer directory.  Built here
# rather than assumed, for the reason sandbox.sh gives -- a harness that
# only runs on a machine already set up by hand is not a harness.
: > "$ST/plainfile" || exit 2
ln -sf "$XCODE" "$ST/link-to-xcode" || exit 2

while read -r name envname args; do
    # Blank lines and the comments inside the case list are skipped here, so
    # the case table can say why a group of rows exists; they are not cases
    # and are not counted as any.
    case $name in ''|\#*) continue ;; esac
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
# What -s and --switch accept as the thing to switch to.  A path is
# refused for being a missing component, a plain file, a path with a file
# where a directory belongs, or a directory that is not developer contents;
# a symlink is followed and then judged by what it lands on.  ~ is spelled
# literally, not expanded: HOME is unset on both sides, and a user name
# would only reach the tool if something expanded it first.
switch-plain     E0 --switch $ST/plainfile
switch-filechild E0 -s $ST/plainfile/child
switch-symlink   E0 --switch $ST/link-to-xcode
switch-s-xcode   E0 -s $XCODE
switch-deepmiss  E0 -s /nonexistent/deep/path
switch-dot       E0 -s .
switch-dotdot    E0 -s ..
switch-root      E0 -s /
switch-trail     E0 -s $XCODE/
switch-tilde     E0 -s ~
switch-tildeslas E0 -s ~/
switch-rel       E0 -s dt/xs
bogus         E0 --bogus
zflag         E0 -Z
trailing      E0 -p foo
operand       E0 foo
man-xcode     E1 --show-manpaths
man-clt       E2 --show-manpaths
man-system    E0 --show-manpaths
install-has   E1 --install

# The whole-token rule.  An option is one of a fixed set spelled exactly as
# the help spells it: no clustering (-pv), no end-of-options marker (--),
# no abbreviation (--print for --print-path), no attached or = argument
# (-s/path, --switch=/path).  Each of those is an invalid argument naming
# the token as it was written.  getopt cannot be held to that -- it
# clusters single letters, takes any unique long prefix and reads -- as
# the end of the options -- so the rows below are what keep a parser from
# drifting back into being one.  They were added with the fix; before it,
# 15 of them were differences.
clust-pv      E0 -pv
clust-vp      E0 -vp
clust-pp      E0 -pp
clust-hv      E0 -hv
clust-vh      E0 -vh
clust-pvv     E0 -pvv
clust-vv      E0 -vv
clust-hh      E0 -hh
clust-vx      E0 -vx
clust-xv      E0 -xv
clust-px      E0 -px
clust-hp      E0 -hp
dashdash      E0 --
dashdash-p    E0 -p --
dashdash-p2   E0 -- -p
dashdash-h    E0 -h --
abbrev-p      E0 --p
abbrev-print  E0 --print
abbrev-printp E0 --print-p
abbrev-vers   E0 --vers
abbrev-ver    E0 --ver
abbrev-v      E0 --v
abbrev-he     E0 --he
abbrev-hel    E0 --hel
abbrev-sw     E0 --sw /tmp
abbrev-inst   E0 --inst
abbrev-res    E0 --res
abbrev-man    E0 --show-man
eq-sw         E0 --sw=/tmp
eq-switch     E0 --switch=/tmp
eq-printpath  E0 --print-path=x
eq-install    E0 --install=x
eq-version    E0 --version=x
eq-reset      E0 --reset=x
attach-s      E0 -s/tmp
attach-s-eq   E0 -s=/tmp
attach-p      E0 -p=x
# -i is here because getopt_long_only read it as an abbreviation of
# --install, so ours would open the install dialog where the shipped tool
# says invalid argument.  -I is the mirror image: --install is long-only.
letter-i      E0 -i
letter-I      E0 -I
letter-x      E0 -x
letter-c      E0 -c
letter-m      E0 -m
letter-M      E0 -M
letter-N      E0 -N
letter-1      E0 -1
letter-p1     E0 -p1
letter-P      E0 -P
letter-pz     E0 -pz
# The complaint used to be built from argv[optind - 1], which is argv[0]
# when getopt stops on the first token, so this one printed our own path.
pdash         E0 -p-
# -h is answered after the whole line has been read, not where it is read,
# so -h --nonsense complains about the nonsense.  It is not exempt from the
# action count either: -h -v is one action and prints the help, -h -v -p is
# two and is refused first.
h-operand     E0 -h x
h-bogus       E0 -h --bogus
help-bogus    E0 --help --bogus
h-s           E0 -h -s
h-v           E0 -h -v
h-p           E0 -h -p
h-h           E0 -h -h
h-v-p         E0 -h -v -p
h-v-p-last    E0 -v -p -h
h-p-r         E0 -h -p -r
h-v-man       E0 -h -v --show-manpaths
# --show-manpaths is not in the help but is a counted action.
man-count-p   E0 -p --show-manpaths
man-count-p2  E0 --show-manpaths -p
man-count-rep E0 --show-manpaths --show-manpaths
man-count-v   E0 --show-manpaths -v
# The complaint names the first token that is wrong, scanning left to
# right, so -p x --nonsense names x and not --nonsense.
first-bad-px  E0 -p x --bogus
first-bad-xb  E0 --bogus x
first-bad-xb2 E0 x --bogus
first-bad-pxv E0 -p x -v
first-bad-vxp E0 -v x -p
first-bad-two E0 --bogus --alsobogus
first-bad-pb  E0 -p --bogus
first-bad-pab E0 -p a b
first-bad-ab  E0 a b
# -s takes whatever follows it, including something that looks like an
# option, and the last one given is the one that counts.
swallow-bogus E0 -s --bogus
swallow-p     E0 -s -p
swallow-v     E0 -s -v
switch-swallow E0 --switch --bogus
s-twice       E0 -s /tmp -s /etc
CASES

rm -rf "$ST"

echo "----"
echo "xcode-select parity: $rows rows, $fail diffs"
[ "$fail" -eq 0 ]