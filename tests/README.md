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
  xcrunmatrix.sh 119/142  xcrun_main, Apple xcrun vs build/release/libxcrun.dylib;
              the 23 divergences are documented (see below); run via
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

The 23 documented divergences on a real machine live in the wider
xcrunmatrix.sh (142 rows, `make -C tests matrix`; 119 match, all on the
Xcode.app layout — CommandLineTools matches 100%) and are unchanged by
anything in this repo except for narrowing.  The three check suites
pass 100%.  The 23 split into the failed named-SDK families,
where Apple re-execs xcodebuild and its diagnostics carry a timestamped
result bundle that cannot be reproduced verbatim, and the -n/-k verbose
SDK-resolution trace, where Apple resolves the SDK by re-running
xcodebuild and we stay in process, so the leading "looking up SDK with"
block of its trace is absent by design.  Our own xcodebuild -find notes
("looking up with", "lookup resolved with") are emitted and match, since
they describe a subprocess we really do run.

Write-side parity (-s / -r) is NOT in these matrices: it must run as
root and would rewrite real selection state.  It is covered by
compiling xcode-select.c with -DXC_SELECT_WRITE_ROOT='"/tmp/.../sbox"'
(lowering every /usr/share path into the sandbox), linking tests-built
rootstub.o (uid_t geteuid(void){return 0;}), and running -s with a
fixture path and -r: three symlinks plus the data file are created,
reset clears all four.  The scratch script sandbox_test.sh that drives
this sequence stays in the fixture root (it writes nothing else and
needs the stub+link recipe above, which is machine-local), so it is not
committed here.