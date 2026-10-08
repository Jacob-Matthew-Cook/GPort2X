/* GPort2X logging and trace switches (spec #23: syscall trace, register-access
 * log, unaligned-access log). Levels and traces are set from the environment
 * (GPORT2X_LOG=error|warn|info|debug, GPORT2X_TRACE=syscall,mmio,cpu,fs,...)
 * or by the command line through gp_log_set_level / gp_log_enable_trace. */
#ifndef GPORT2X_LOG_H
#define GPORT2X_LOG_H

#include <stdbool.h>
#include <stdio.h>

enum gp_log_level { GP_LOG_ERROR = 0, GP_LOG_WARN, GP_LOG_INFO, GP_LOG_DEBUG };

enum gp_trace {
    GP_TRACE_SYSCALL = 1 << 0, /* every guest syscall with arguments and result */
    GP_TRACE_MMIO = 1 << 1,    /* every register-file access */
    GP_TRACE_CPU = 1 << 2,     /* interpreter events: undefined instructions, aborts */
    GP_TRACE_FS = 1 << 3,      /* path resolution and file operations */
    GP_TRACE_UNALIGNED = 1 << 4, /* unaligned loads and stores (spec 5.3, OPEN-13) */
    GP_TRACE_DEV = 1 << 5,     /* device behaviour: flips, audio, input */
};

void gp_log_init_from_env(void);
void gp_log_set_level(enum gp_log_level level);
void gp_log_set_output(FILE *out);
void gp_log_enable_trace(unsigned traces); /* OR of enum gp_trace */
bool gp_log_trace_enabled(unsigned trace);
/* Parses "syscall,mmio,..." into a trace mask; unknown names are ignored. */
unsigned gp_log_parse_traces(const char *list);

void gp_log(enum gp_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void gp_trace(unsigned trace, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define gp_error(...) gp_log(GP_LOG_ERROR, __VA_ARGS__)
#define gp_warn(...) gp_log(GP_LOG_WARN, __VA_ARGS__)
#define gp_info(...) gp_log(GP_LOG_INFO, __VA_ARGS__)
#define gp_debug(...) gp_log(GP_LOG_DEBUG, __VA_ARGS__)

#endif /* GPORT2X_LOG_H */
