/*
 * A geteuid() that says zero.
 *
 * The write side of xcode-select is gated on being root, and rightly so:
 * -s rewrites the one selection the whole system reads.  That is exactly
 * what makes it untestable from a normal account, and exactly what must
 * not be worked around by trying harder.
 *
 * So the gate is not defeated, it is stood on: this object is linked into
 * a test build whose every write path is compiled into a sandbox
 * (XC_SELECT_WRITE_ROOT, see src/xcode-select/xcode-select.c), and the
 * sandbox is the only thing this run can write.  The check answers the
 * question the gate asks -- "is this the user we are allowed to do this
 * to" -- and the sandbox makes the answer safe to give yes to.
 *
 * A call this does not override still reaches libc's, so nothing here
 * depends on the whole process believing it is root.
 */
#include <sys/types.h>
#include <unistd.h>

uid_t
geteuid(void)
{

	return 0;
}
