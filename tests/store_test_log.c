// rc_log for the store test DLL: straight to stdout
#include <stdarg.h>
#include <stdio.h>

void rc_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  [store] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
