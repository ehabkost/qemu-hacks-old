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

void qemu_set_log_filename(const char *filename)
{
    logfilename = strdup(filename);
    if (logfile) {
        fclose(logfile);
        logfile = NULL;
    }
    qemu_set_log(loglevel);
}
