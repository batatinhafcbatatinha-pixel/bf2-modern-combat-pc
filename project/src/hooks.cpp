// bf2mc - game-specific mid-asm hooks (declared in bf2mc_config.toml [[midasm_hook]])

#include "generated/default/bf2mc_init.h"

#include <rex/logging.h>

// Before `bl MmAllocatePhysicalMemoryEx` in the XPhysicalAlloc wrapper (0x82195060).
// Protect without PAGE_READONLY/PAGE_READWRITE (e.g. 0x20000400 = WRITECOMBINE only) is
// rejected by the runtime's kernel; treat it as read/write like the console does.
void bf2_fix_physalloc_protect(PPCRegister& r5) {
  if (!(r5.u32 & (0x02 | 0x04))) {
    REXLOG_INFO("bf2mc: physalloc protect {:#x} -> {:#x}", r5.u32, r5.u32 | 0x04);
    r5.u64 = r5.u32 | 0x04;
  }
}

