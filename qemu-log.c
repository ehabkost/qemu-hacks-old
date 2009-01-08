/*
 * Qemu logging functions
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *  Copyright (c) 2009 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "qemu-log.h"

FILE *logfile;
int loglevel;

static const char *logfilename = "/tmp/qemu.log";
static int log_append = 0;


void qemu_set_log(int level)
{
    loglevel = level;
    if (loglevel && !logfile) {
        logfile = fopen(logfilename, log_append ? "a" : "w");
        if (!logfile) {
            perror(logfilename);
            _exit(1);
        }
#if !defined(CONFIG_SOFTMMU)
        /* must avoid mmap() usage of glibc by setting a buffer "by hand" */
        {
            static char logfile_buf[4096];
            setvbuf(logfile, logfile_buf, _IOLBF, sizeof(logfile_buf));
        }
#else
        setvbuf(logfile, NULL, _IOLBF, 0);
#endif
        log_append = 1;
    }
    if (!loglevel && logfile) {
        fclose(logfile);
        logfile = NULL;
    }
}

static int log_incomplete_line;
static int qemu_log_line(const char *line, size_t len)
{
    int r;
    if (!log_incomplete_line) {
        time_t t;
        struct tm tm;
        time(&t);
        localtime_r(&t, &tm);
        fprintf(logfile, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    r = fwrite(line, len, 1, logfile);
    if (r < 0)
        return r;

    log_incomplete_line = (line[len-1] != '\n');
    return r;
}



#define LOGBUF_SIZE 1024
int __qemu_log_vprintf(const char *fmt, va_list args)
{
    char logbuf[LOGBUF_SIZE];
    const char *next, *nl;
    int size;

    /* Format the message */
    size = vsnprintf(logbuf, 1024, fmt, args);
    if (size < 0)
        return size;

    /* just in case the output was truncated */
    if (size >= LOGBUF_SIZE)
        logbuf[LOGBUF_SIZE-1] = '\0';


    /* Handle one line a time */
    next = logbuf;
    while ( (nl = strchr(next, '\n')) ) {
        if (qemu_log_line(next, nl-next+1) < 0)
            return -1;
        next = nl+1;
    }
    /* No more newlines. If there is an incomplete line remaining,
     * print it also */
    if (*next)
        if (qemu_log_line(next, strlen(next)) < 0)
            return -1;

    /* Just in case we truncated the message above, print a warning
     * just after the message.
     */
    if (size >= LOGBUF_SIZE)
        if (qemu_log_line("...[truncated]", 14))
            return -1;

    return 0;
}

int __qemu_log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    return __qemu_log_vprintf(fmt, ap);
}

void qemu_set_log_filename(const char *filename)
{
    logfilename = strdup(filename);
    if (logfile) {
        fclose(logfile);
        logfile = NULL;
    }
    qemu_set_log(loglevel);
}

int qemu_log_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qemu_log_vprintf(fmt, ap);
    return 0;
}
