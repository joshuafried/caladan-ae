#pragma once

#include <asm/mwait.h>

#define mwait(remote, cached)                                                  \
	{                                                                      \
		if (smp_load_acquire(remote) == (cached)) {                    \
			__monitor(remote, 0, 0);                               \
			if (smp_load_acquire(remote) == (cached)) {            \
				__mwait(0, MWAIT_ECX_INTERRUPT_BREAK);         \
			}                                                      \
		}                                                              \
	}
