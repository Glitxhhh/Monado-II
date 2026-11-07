// [XCDE] LINUX-003: Linux fiber lifecycle at the guest-thread/reboot boundary.
#pragma once
#include <stddef.h>
#if defined(__linux__) && defined(__x86_64__)
// Call on the original native stack after guest return or longjmp, before thread exit.
// A fiber abandoned by longjmp cannot be resumed; it may be finalized/reinitialized/reset.
void fibers_thread_cleanup(void);
// Only after every fiber-using thread has cleaned up and joined. Returns reclaimed fibers.
size_t fibers_reset(void);
#endif
