/* newlib entry points the PSP browser image defines itself.

   The image formats text only through snprintf. Two newlib members would
   otherwise link the stream printf core (_vfprintf_r or _vfiprintf_r,
   11-14 KB of .text) for output the browser never produces:

   - __assert_func reports through fiprintf. The browser compiles with
     NDEBUG, so only library asserts (nghttp2, newlib's number conversion)
     reach it.
   - mallinfo lives in one section with malloc_stats and mstats, which print
     through fiprintf, so the heap queries pulled the printers in with it.

   Both keep newlib's behaviour: assert reports the failed expression on
   stderr and aborts, and mallinfo is the reentrant query for this thread.
   Host builds use their C library's. */

#include <assert.h>
#include <malloc.h>
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_func(const char *file, int line, const char *function,
                   const char *expression)
{
    char message[256];
    if (snprintf(message, sizeof(message),
                 "assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
                 expression, file, line,
                 function != NULL ? ", function: " : "",
                 function != NULL ? function : "") > 0)
        (void) fputs(message, stderr);
    abort();
}

struct mallinfo mallinfo(void)
{
    return _mallinfo_r(_REENT);
}
