# Differential parity harnesses against Apple's shipped tools and our
# clean-room builds.  Every matrix below is read-only: neither side ever
# writes developer-directory state or asks for an install, so they are
# safe to run from a normal account.  They compare our build/release
# products against Apple's by running the same inputs through both and
# diffing stdout, stderr and exit status, with timestamps/PIDs/errno
# normalized away where a forked tool is involved.

Current results (default fixtures, Apple macOS build):
  xsmatrix.sh    30/30  /usr/bin/xcode-select vs build/release/xcode-select
  libxsmatrix.sh 36/36  /usr/lib/libxcselect.dylib vs build/release/libxcselect.dylib
  ivkmatrix.sh   39/39  invoke_xcrun, Apple lib vs ours, real tools downstream
  xcrunmatrix.sh 142/142  xcrun_main, Apple xcrun vs build/release/libxcrun.dylib,
              every case matching Apple's transcript; run via
              `make -C tests matrix`, not check

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

The wider xcrunmatrix.sh (142 rows, `make -C tests matrix`) matches
Apple on all 142, on both the Xcode.app and the CommandLineTools
layouts.  The three check suites pass 100%.  Getting there took three
things worth knowing, since each is easy to get backwards:

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

11 rows: -s lays down three links and the data file (and the data file is
exactly the path plus one trailing newline, the one the reader strips);
-p reads back the directory -s recorded, which is the row the harness is
for, since a successful -s that -p could not see would leave the system
selecting something nobody asked for; both halves name the same four
system paths; a second -s replaces rather than accumulates; -r clears all
four and writes no default; an empty sandbox reads as the system default
rather than the machine's real selection, which is what proves -p is
really reading the sandbox; a rejected -s writes nothing; and the shipped
build still refuses both options as a normal user, which is the reason
any of this is necessary.

Both halves are recompiled, not just the writer.  libxcselect needed the
write-root macro to be readable in a sandbox at all -- DEVELOPER_DIR is
not a substitute, because it overrides the selection outright, so -p
echoes it back and the run proves nothing.  Recompiling the reader too
is what turns the read-back from a comparison of two hardcoded strings
into an actual read of what was written.