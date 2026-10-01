/* Host driver for the JavaScript engine micro-benchmark (src/js_bench.c):
   the same kernels the PSP validation build runs under boot.cfg
   validation_js_bench=N, on the host, for per-kernel device/host ratios.

   tilefinch-js-bench [--scale N] [--repeat R] [--trace DIR] [--lazy BYTES]
                      [--heap-mb MB] [--only a,b,c] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/js_bench.h"

static void emit(void *opaque, const char *line)
{
    (void) opaque;
    puts(line);
}

int main(int argc, char **argv)
{
    JsBenchOptions options;
    js_bench_default_options(&options);
    options.repeat = 3;
    for (int at = 1; at < argc; at++) {
        const char *flag = argv[at];
        const char *value = at + 1 < argc ? argv[at + 1] : NULL;
        if (strcmp(flag, "--scale") == 0 && value != NULL) {
            options.scale = (unsigned) strtoul(value, NULL, 10);
            at++;
        } else if (strcmp(flag, "--repeat") == 0 && value != NULL) {
            options.repeat = (unsigned) strtoul(value, NULL, 10);
            at++;
        } else if (strcmp(flag, "--trace") == 0 && value != NULL) {
            options.trace_dir = value;
            at++;
        } else if (strcmp(flag, "--lazy") == 0 && value != NULL) {
            options.lazy_threshold = (uint32_t) strtoul(value, NULL, 10);
            at++;
        } else if (strcmp(flag, "--heap-mb") == 0 && value != NULL) {
            options.memory_limit = (size_t) strtoul(value, NULL, 10)
                                   * 1024u * 1024u;
            at++;
        } else if (strcmp(flag, "--only") == 0 && value != NULL) {
            options.only = value;
            at++;
        } else {
            fprintf(stderr,
                    "usage: tilefinch-js-bench [--scale N] [--repeat R] "
                    "[--trace DIR] [--lazy BYTES] [--heap-mb MB] "
                    "[--only a,b,c]\n");
            return 2;
        }
    }
    return js_bench_run(&options, emit, NULL) == 0 ? 0 : 1;
}
