// [XCDE] WINPARITY-001: native thread/fiber lifetime; reset only after joins.
#pragma once
#include <stddef.h>
void fibers_thread_cleanup(void);
void fibers_prepare_unwind(void); // switch to the real Win32 root BEFORE kernel longjmp
size_t fibers_reset(void);
