/* GPort2X logging: see include/gport2x/log.h. */
#include "gport2x/log.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static enum gp_log_level current_level = GP_LOG_WARN;
static unsigned current_traces;
static FILE *output; /* NULL = stderr */

static const char *const level_names[] = {"error", "warn", "info", "debug"};

static const struct {
    const char *name;
    unsigned bit;
} trace_names[] = {
    {"syscall", GP_TRACE_SYSCALL}, {"mmio", GP_TRACE_MMIO}, {"cpu", GP_TRACE_CPU},
    {"fs", GP_TRACE_FS},           {"unaligned", GP_TRACE_UNALIGNED}, {"dev", GP_TRACE_DEV},
};

void gp_log_set_level(enum gp_log_level level)
{
    current_level = level;
}

void gp_log_set_output(FILE *out)
{
    output = out;
}

void gp_log_enable_trace(unsigned traces)
{
    current_traces |= traces;
}

bool gp_log_trace_enabled(unsigned trace)
{
    return (current_traces & trace) != 0;
}

unsigned gp_log_parse_traces(const char *list)
{
    unsigned mask = 0;
    if (!list)
        return 0;
    while (*list) {
        size_t n = strcspn(list, ",");
        for (size_t i = 0; i < sizeof trace_names / sizeof trace_names[0]; i++)
            if (strlen(trace_names[i].name) == n && strncmp(list, trace_names[i].name, n) == 0)
                mask |= trace_names[i].bit;
        if (n == 3 && strncmp(list, "all", 3) == 0)
            mask = ~0u;
        list += n;
        if (*list == ',')
            list++;
    }
    return mask;
}

void gp_log_init_from_env(void)
{
    const char *lvl = getenv("GPORT2X_LOG");
    if (lvl) {
        for (size_t i = 0; i < sizeof level_names / sizeof level_names[0]; i++)
            if (strcmp(lvl, level_names[i]) == 0)
                current_level = (enum gp_log_level)i;
    }
    gp_log_enable_trace(gp_log_parse_traces(getenv("GPORT2X_TRACE")));
}

static void emit(const char *tag, const char *fmt, va_list ap)
{
    FILE *out = output ? output : stderr;
    fprintf(out, "[gport2x %s] ", tag);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    fflush(out);
}

void gp_log(enum gp_log_level level, const char *fmt, ...)
{
    if (level > current_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    emit(level_names[level], fmt, ap);
    va_end(ap);
}

void gp_trace(unsigned trace, const char *fmt, ...)
{
    if (!(current_traces & trace))
        return;
    const char *tag = "trace";
    for (size_t i = 0; i < sizeof trace_names / sizeof trace_names[0]; i++)
        if (trace_names[i].bit == trace)
            tag = trace_names[i].name;
    va_list ap;
    va_start(ap, fmt);
    emit(tag, fmt, ap);
    va_end(ap);
}
