# Differential parity harnesses against Apple's shipped tools and our
# clean-room builds.  Every matrix below is read-only: neither side ever
# writes developer-directory state or asks for an install, so they are
# safe to run from a normal account.  They compare our build/release
# products against Apple's by running the same inputs through both and
# diffing stdout, stderr and exit status, with timestamps/PIDs/errno
# normalized away where a forked tool is involved.

Current results (default fixtures, Apple macOS build):
  xsmatrix.sh   120/120  /usr/bin/xcode-select vs build/release/xcode-select
  libxsmatrix.sh 36/36  /usr/lib/libxcselect.dylib vs build/release/libxcselect.dylib
  ivkmatrix.sh   39/39  invoke_xcrun, Apple lib vs ours, real tools downstream
  sandbox.sh     14/14  -s and -r against a scratch tree, read back off disk
  xcrunmatrix.sh 224/224  the wide argument sweep plus the shipped
              xcrun binary's DEVELOPER_DIR edges: Apple xcrun vs
              build/release/libxcrun.dylib through the driver, and vs
              build/release/xcrun where the dev dir is the variable;
              every case matching Apple's transcript; run via
              `make -C tests matrix`, not check

The first four are `make -C tests check`; the last is `make -C tests
matrix`, and `make -C tests test` runs both.  sandbox.sh is in check even
though it is the one harness that compiles rather than diffs: it needs
the same SDK and CC the products were built with, which `check: all` has
already established, and it takes about 0.7s.  It was separate for a
while, which meant the only harness proving the -s/-p contract, and the
only one checking the root gate is real, was not run by the default
target.

xsmatrix.sh is the one that carries the argument grammar, so it is where
a parser is pinned down: an option is one fixed spelling, and clustering
(`-pv`), `--`, any long prefix (`--print-p`), and attached or `=` forms
(`-s/path`, `--switch=/path`) are each invalid arguments rather than
accepted.  It also names the first bad token left to right, keeps `-h`
out of the action count, and pins what `-s`/`--switch` will and will not
switch to (a plain file, a path with a file in it, a missing component,
a directory that is not developer contents, a symlink followed to what
it lands on).  Those `--switch` rows name directories a root run would
accept, so the script refuses to run as root rather than move the real
selection; the write paths are sandbox.sh's job.  One shape cannot be a
row: the case table word-splits its argument column, so an empty
argument (`-s ''`) is not expressible and is covered by hand.

The --run section of xcrunmatrix.sh covers the other half of what it
means to replace a process: the environment the tool is handed (all
thirteen entries of it, sorted, plus the six that matter named one at a
time so a failure says which), the bytes piped in, the working
directory, the umask, and the status the shell ends up with for an exit
code and for a signal.  Both sides get the same starting environment by
construction -- `env -i` and then the same variables -- so what the row
compares is what the tool did, not what the shell was carrying.

Products and drivers
--------------------
`make` (top-level) puts the products in build/release; `make` in tests/
puts the three dlopen drivers in build/test:
  ivk      dlopens a libxcselect named by DRIVE_LIB (Apple's by default)
           and calls xcselect_invoke_xcrun; DRIVE_DEV becomes
           DEVELOPER_DIR.  Measures libxcselect's own invoke behavior
           with exactly the same downstream libxcrun on both sides.
  libxstest <library> <op> [args...]
           exercises one export at a time (devdir, match, find, version,
           hostsdk, bundle, manpaths), printing the result for diffing.
           Nothing here is write-side.
  xcrundrive
           dlopens the libxcrun named by DRIVE_LIB (our build) and
           calls xcrun_main with the caller's argv, the way the xcrun
           front-end does, so the full argument surface can be diffed
           against Apple's /usr/bin/xcrun without touching real state.
           DRIVE_DEV and DRIVE_LIB are removed from the environment
           before the call, because libxcrun hands the tool it runs the
           caller's environment and a tool would otherwise inherit
           DRIVE_LIB on this side and nothing on Apple's.

Fixtures (XS_FIX, default /tmp/dt/xs)
-------------------------------------
The matrices need a working /Applications/Xcode.app/Contents/Developer
and /Library/Developer/CommandLineTools present; everything else below
is read by both sides and only needs to exist so the traversal happens
against a missing/odd layout as well as a real one:
  FAKE.app   a minimal Xcode.app shape (Contents/Developer with an
             usr/bin/{clang,clang++,ld,ar} that print a banner) so an
             uninstalled .app path is resolvable; it is a real directory
             fixture, safe to commit.
  fakedd     a developer directory with usr/bin/xcrun but no usr/lib, to
             exercise the missing-libxcrun fallback.
  devdir     a machine-local copy of a full developer directory (about
             800 MB) and is NOT committed; when absent, ivkmatrix's
             devdir edge and the large-dir comparisons degrade, so keep
             it out of a fresh fixture root unless you rebuild it.
  libxcrun.c.vtest  a snapshot of src/libxcrun/libxcrun.c kept in the
             fixture root for a difftool-style review; refresh it by
             re-copying the source, no commit involved.

The wider xcrunmatrix.sh (192 rows, `make -C tests matrix`) matches
Apple on all 192, on both the Xcode.app and the CommandLineTools
layouts.  The three check suites pass 100%.  Getting there took three
things worth knowing, since each is easy to get backwards:

  * Coverage is the reason the score is believable.  --help and --run
    had no cases at all, --toolchain had one, and every -l/--log case
    paired it with something other than --run, which is where --log
    actually prints.  A toolchain name that is not installed is
    answered by Apple with the *default* toolchain rather than with the
    path the requested name would have occupied, and --log has to write
    to stderr, because stdout is block-buffered when it is not a
    terminal and the execve that follows would discard it.  Both were
    wrong here and both were invisible at 142/142.

  * `make -C tests matrix` builds the products first.  The drivers are
    dlopening libxcselect and libxcrun by path, so a stale library
    would otherwise be measured silently -- which is how a fixed --log
    line still read as missing until that was fixed.

  * A bad `DEVELOPER_DIR` is rejected by libxcselect, not libxcrun, so
    no row that reaches libxcrun through the driver can see it.  That
    leaves the shipped `xcrun` binary, which nothing compared to
    Apple's at all until the last section of xcrunmatrix.sh.  A stale
    `DEVELOPER_DIR` is an ordinary event -- unmounted volume, renamed
    Xcode, an environment baked at image-build time -- and the answer
    is a specific message and exit status that scripts branch on, so
    those rows exist to keep it that way.  The selection-sourced form
    of the same error ("active developer" rather than "DEVELOPER_DIR")
    is still untested: reaching it means moving the real selection.

  * Apple re-execs xcodebuild for a named SDK it cannot place, and its
    own stderr about that carries a timestamped result bundle path, so
    xcodebuild -version is asked the same two or three questions Apple
    asks and the answers are reported the same way.  The transcript
    matches because the subprocesses are real, not because the text is
    reconstructed.

  * A run that names an SDK which is not there is asked about once and
    remembered.  Apple files the name under the same database key a name
    that resolved is filed under, valued with the manual page path it
    would have used, so the second run of the same question reports what
    it already knows and repeats neither the property-list read nor the
    item lookup.  Ours reads that key and skips the same two steps, which
    is why the first run of a bad name and the ones after it differ.

  * Whether a tool lookup goes out to xcodebuild is a question about the
    cache, not about the developer directory: a warm run answers from
    what it has learned, and only --no-cache or --kill-cache re-asks.
    Delegating unconditionally is what the first version of this did,
    and it fails the warm cases.

Write-side parity (-s / -r) is NOT in these matrices: -s rewrites the one
selection the whole system reads, so it must run as root, and there is no
way to run it from a normal account without either sudo or editing the
gate out of the source -- and editing the gate out means the thing under
test is no longer the thing that ships.  sandbox.sh has its own `sandbox`
target and stands on the gate instead of defeating it:

  rootstub.c   a geteuid() returning 0, linked into the sandboxed build
  xcode-select.c
  libxcselect.c
               both recompiled with XC_SELECT_WRITE_ROOT set to a mktemp
               directory, which lowers all four write paths under it
  sandbox.sh   builds the two together, then drives -s and -r against
               the scratch tree and reads the result back off disk

So the gate does its job, the write really happens, and the only bytes
involved are removed on the way out.  The real /var/select, /var/db and
/usr/share selection is never touched -- which is also the limit of what
it proves: it exercises the code that writes those paths, not the paths
themselves.

14 rows: -s lays down three links and the data file (and the data file is
exactly the path plus one trailing newline, the one the reader strips);
-p reads back the directory -s recorded, which is the row the harness is
for, since a successful -s that -p could not see would leave the system
selecting something nobody asked for; each of the four paths in turn,
with the other three deleted, still yields the selection; a second -s
replaces rather than accumulates; -r clears all four and writes no
default; an empty sandbox's -p agrees with the shipped build's -p; a
rejected -s writes nothing; and the shipped build still refuses both
options as a normal user, which is the reason any of this is necessary.

Those four single-path rows are what tie the four names to the system
paths, and they do it by exercising the compiled code rather than by
reading either source file.  An earlier version compared the writer's and
the reader's path lists by scraping the sources with sed and grep, which
is the kind of check that breaks when a macro moves onto another line --
it did, twice -- and it could not see past the reader's fallback order
anyway.  The single-path rows are stronger: dev_dir_links is consulted in
order and the first one that resolves wins, so the plain read-back cannot
notice a renamed third link, because the first one still answers.  Only
deleting the later paths puts each one in reach.  Renaming a path in one
half, in both halves, in the first link and in the data file are all
caught, and none of it reads the source.

That empty-sandbox row is a comparison against the shipped binary rather
than a hardcoded path, which is what makes it worth anything: on a host
with no selection in /var/select, an earlier version that only asserted
"the output does not start with the sandbox root" passed whether or not
the macro was applied, because the reader had nothing to pick up either
way.  Pointing the reader's fallback at a bogus directory makes the
comparison fail, which is how it was checked.

Every -p call in the script goes through runp, which removes DEVELOPER_DIR
from the environment first.  It has to: DEVELOPER_DIR overrides the
selection outright, so -p would echo the caller's own value and agree
with the expected one regardless of where the four paths point.  That
would have made the read-back pass for the wrong reason on any machine
whose CI exports it -- the same reason the macro exists rather than the
environment being leaned on.

Both halves are recompiled, not just the writer.  libxcselect needed the
write-root macro to be readable in a sandbox at all -- DEVELOPER_DIR is
not a substitute, because it overrides the selection outright, so -p
echoes it back and the run proves nothing.  Recompiling the reader too
is what turns the read-back from a comparison of two hardcoded strings
into an actual read of what was written.