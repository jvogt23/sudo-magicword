/*
 * Copyright (c) 2019 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SUDO_LOGSRVD_H
#define SUDO_LOGSRVD_H

#if PROTOBUF_C_VERSION_NUMBER < 1003000
# error protobuf-c version 1.30 or higher required
#endif

#define DEFAULT_PORT	30344
#define IOLOG_DIR	"/var/tmp/iologs"
#define RUNAS_DEFAULT	"root"

/* How often to send an ACK to the client (commit point) in seconds */
#define ACK_FREQUENCY	10

/* Shutdown timeout (in seconds) in case client connections time out. */
#define SHUTDOWN_TIMEO	10

/*
 * I/O log details from the ExecMessage + iolog path and sessid.
 */
struct iolog_details {
    char *command;
    char *cwd;
    char *iolog_dir;
    char *rungroup;
    char *runuser;
    char *submithost;
    char *submituser;
    char *submitgroup;
    char *ttyname;
    char **argv;
    time_t start_time;
    int argc;
    int lines;
    int columns;
    char sessid[7];
};

/*
 * Connection status.
 * In the RUNNING state we expect I/O log buffers.
 */
enum connection_status {
    INITIAL,
    RUNNING,
    EXITED,
    SHUTDOWN,
    FLUSHED,
    ERROR
};

struct connection_buffer {
    uint8_t *data;		/* pre-allocated data buffer */
    unsigned int size;		/* currently always UINT16_MAX + 2 */
    unsigned int len;
    unsigned int off;
};

/*
 * Per-connection state.
 * TODO: iolog_compress
 */
struct connection_closure {
    TAILQ_ENTRY(connection_closure) entries;
    struct timespec start_time;
    struct timespec elapsed_time;
    struct connection_buffer read_buf;
    struct connection_buffer write_buf;
    struct sudo_event *commit_ev;
    struct sudo_event *read_ev;
    struct sudo_event *write_ev;
    char *iolog_dir;
    struct iolog_file iolog_files[IOFD_MAX];
#if 0
    int iolog_dir_fd;
#endif
    int sock;
    enum connection_status state;
};

/* iolog_writer.c */
bool iolog_init(ExecMessage *msg, struct connection_closure *closure);
bool iolog_restart(RestartMessage *msg, struct connection_closure *closure);
int store_iobuf(int iofd, IoBuffer *msg, struct connection_closure *closure);
int store_suspend(CommandSuspend *msg, struct connection_closure *closure);
int store_winsize(ChangeWindowSize *msg, struct connection_closure *closure);
void iolog_close_all(struct connection_closure *closure);

/* logsrvd_conf.c */
void logsrvd_conf_read(const char *path);
const char *logsrvd_conf_iolog_dir(void);
const char *logsrvd_conf_iolog_file(void);

#endif /* SUDO_LOGSRVD_H */