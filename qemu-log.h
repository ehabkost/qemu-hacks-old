#ifndef QEMU_LOG_H
#define QEMU_LOG_H

#ifndef QEMU_STDIO_DEFS
#include <stdio.h>
#endif

/* The deprecated global variables: */
extern FILE *logfile;
extern int loglevel;

typedef int (*qemu_fprintf_fn)(FILE *f, const char *fmt, ...);


/* 
 * The new API:
 *
 */

/* Log settings checking macros: */

/* Returns true if qemu_log() will really write somewhere
 */
#define qemu_log_enabled() (logfile != NULL)

/* Returns true if a bit is set in the current loglevel mask
 */
#define qemu_loglevel_mask(b) ((loglevel & (b)) != 0)


/* Logging functions: */

/* main logging function
 */
#define qemu_log(...) do {                 \
        if (logfile)                       \
            fprintf(logfile, ## __VA_ARGS__); \
    } while (0)

/* vfprintf-like logging function
 */
#define qemu_log_vprintf(fmt, va) do {     \
        if (logfile)                       \
            vfprintf(logfile, fmt, va);    \
    } while (0)

/* log only if a bit is set on the current loglevel mask
 */
#define qemu_log_mask(b, ...) do {         \
        if (loglevel & (b))                \
            fprintf(logfile, ## __VA_ARGS__); \
    } while (0)




/* Special cases: */

/* cpu_dump_state() logging functions: */
#define log_cpu_state(env, f) cpu_dump_state((env), NULL, qemu_log_fprintf, (f));
#define log_cpu_state_mask(b, env, f) do {           \
      if (loglevel & (b)) log_cpu_state((env), (f)); \
  } while (0)

/* disas() and target_disas() to logfile: */
#define log_target_disas(start, len, flags) \
        target_disas(NULL, qemu_log_fprintf, (start), (len), (flags))
#define log_disas(start, len) \
        disas(NULL, qemu_log_fprintf, (start), (len))

/* page_dump() output to the log file: */
#define log_page_dump() page_dump(NULL, qemu_log_fprintf)



/* Maintenance: */

/* fflush() the log file */
#define qemu_log_flush() fflush(logfile)

/* Close the log file */
#define qemu_log_close() do { \
        fclose(logfile);      \
        logfile = NULL;       \
    } while (0)

/* Set up a new log file */
#define qemu_log_set_file(f) do { \
        logfile = (f);            \
    } while (0)

/* Set up a new log file, only if none is set */
#define qemu_log_try_set_file(f) do { \
        if (!logfile)                 \
            logfile = (f);            \
    } while (0)


/* Setup code: */
void qemu_set_log(int level);
void qemu_set_log_filename(const char *filename);


/** fprintf-like logging function, for cpu_dump_state() & others
 */
int qemu_log_fprintf(FILE *f, const char *fmt, ...);

#endif
