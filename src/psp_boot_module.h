#ifndef TILEFINCH_PSP_BOOT_MODULE_H
#define TILEFINCH_PSP_BOOT_MODULE_H

#include "tilefinch/psp_boot_handoff.h"

/* Returns only before launcher heap retirement, so ordinary LoadExec remains
   safe. Successful startup retires this thread and never returns. */
void psp_boot_module_start(const char *path, const void *arguments, size_t bytes,
                           bool (*retire_callbacks)(void *), void *context);

/* Raw syscall fallback also works before libc startup or after its teardown. */
_Noreturn void psp_boot_module_exec_fallback(const char *path);

#ifdef __GNUC__
/* Present only in the relocatable browser; the static diagnostic EBOOT has
   no module receipt. Reports after the browser has opened its usual log. */
void psp_boot_module_report(void) __attribute__((weak));
#endif

#endif
