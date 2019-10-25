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

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif /* HAVE_STDBOOL_H */
#if defined(HAVE_STDINT_H)
# include <stdint.h>
#elif defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef HAVE_GETADDRINFO
# include "compat/getaddrinfo.h"
#endif

#include "log_server.pb-c.h"
#include "sudo_gettext.h"	/* must be included before sudo_compat.h */
#include "sudo_compat.h"
#include "sudo_conf.h"
#include "sudo_debug.h"
#include "sudo_util.h"
#include "sudo_event.h"
#include "sudo_fatal.h"
#include "iolog_util.h"
#include "sendlog.h"

static gzFile io_fds[IOFD_MAX];

/* I/O log file names relative to iolog_dir. */
/* XXX - duplicated with server */
static const char *iolog_names[] = {
    "stdin",	/* IOFD_STDIN */
    "stdout",	/* IOFD_STDOUT */
    "stderr",	/* IOFD_STDERR */
    "ttyin",	/* IOFD_TTYIN  */
    "ttyout",	/* IOFD_TTYOUT */
    "timing",	/* IOFD_TIMING */
    NULL	/* IOFD_MAX */
};

static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h host] [-p port] /path/to/iolog\n",
	getprogname());
    exit(1);
}

/*
 * Connect to specified host:port
 * If host has multiple addresses, the first one that connects is used.
 * Returns open socket or -1 on error.
 */
static int
connect_server(const char *host, const char *port)
{
    struct addrinfo hints, *res, *res0;
    const char *cause = "getaddrinfo";
    int error, sock, save_errno;
    debug_decl(connect_server, SUDO_DEBUG_UTIL)

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(host, port, &hints, &res0);
    if (error != 0) {
	sudo_warnx("unable to resolve %s:%s: %s", host, port, gai_strerror(error));
	debug_return_int(-1);
    }

    sock = -1;
    for (res = res0; res; res = res->ai_next) {
	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock == -1) {
	    cause = "socket";
	    continue;
	}
	if (connect(sock, res->ai_addr, res->ai_addrlen) == -1) {
	    cause = "connect";
	    save_errno = errno;
	    close(sock);
	    errno = save_errno;
	    sock = -1;
	    continue;
	}
	break;	/* success */
    }
    if (sock != -1) {
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
	    cause = "fcntl(O_NONBLOCK)";
	    save_errno = errno;
	    close(sock);
	    errno = save_errno;
	    sock = -1;
	}
    }

    if (sock == -1)
	sudo_warn("%s", cause);
    freeaddrinfo(res0);

    debug_return_int(sock);
}

/*
 * Free client closure allocated by client_closure_alloc()
 */
static void
client_closure_free(struct client_closure *closure)
{
    debug_decl(client_closure_free, SUDO_DEBUG_UTIL)

    if (closure != NULL) {
	sudo_ev_free(closure->read_ev);
	sudo_ev_free(closure->write_ev);
	free(closure->read_buf.data);
	free(closure->write_buf.data);
	free(closure->buf);
	free(closure);
    }

    debug_return;
}

/*
 * Read the next record from the timing file.
 * Return 0 on success, 1 on EOF and -1 on error.
 */
int
read_timing_record(struct timing_closure *timing)
{
    const char *errstr;
    char line[LINE_MAX];
    int errnum;
    debug_decl(read_timing_record, SUDO_DEBUG_UTIL)

    /* Read next record from timing file. */
    if (gzgets(io_fds[IOFD_TIMING], line, sizeof(line)) == NULL) {
	/* EOF or error reading timing file, we are done. */
	if (gzeof(io_fds[IOFD_TIMING]))
	    debug_return_int(1);	/* EOF */
	if ((errstr = gzerror(io_fds[IOFD_TIMING], &errnum)) == NULL)
	    errstr = strerror(errno);
	sudo_warnx("error reading timing file: %s", errstr);
	debug_return_int(-1);
    }

    /* Parse timing file record. */
    line[strcspn(line, "\n")] = '\0';
    if (!parse_timing(line, timing)) {
	sudo_warnx("invalid timing file line: %s", line);
	debug_return_int(-1);
    }

    debug_return_int(0);
}

/*
 * Read the next I/O buffer as described by closure->timing.
 */
static bool
read_io_buf(struct client_closure *closure)
{
    struct timing_closure *timing = &closure->timing;
    size_t nread;
    debug_decl(read_io_buf, SUDO_DEBUG_UTIL)

    if (io_fds[timing->event] == NULL) {
	sudo_warnx("%s file not open", iolog_names[timing->event]);
	debug_return_bool(false);
    }

    /* Expand buf as needed. */
    if (timing->u.nbytes > closure->bufsize) {
	free(closure->buf);
	do {
	    closure->bufsize *= 2;
	} while (timing->u.nbytes > closure->bufsize);
	if ((closure->buf = malloc(closure->bufsize)) == NULL) {
	    sudo_warn("malloc %zu", closure->bufsize);
	    timing->u.nbytes = 0;
	    debug_return_bool(false);
	}
    }

    nread = gzread(io_fds[timing->event], closure->buf, timing->u.nbytes);
    if (nread != timing->u.nbytes) {
	int errnum;
	const char *errstr;

	if ((errstr = gzerror(io_fds[timing->event], &errnum)) == NULL)
	    errstr = strerror(errno);
	sudo_warnx("unable to read %s file: %s", iolog_names[timing->event], errstr);
	debug_return_bool(false);
    }
    debug_return_bool(true);
}

/*
 * Format a ClientMessage and store the wire format message in buf.
 * Returns true on success, false on failure.
 */
static bool
fmt_client_message(struct connection_buffer *buf, ClientMessage *msg)
{
    uint16_t msg_len;
    bool ret = false;
    size_t len;
    debug_decl(fmt_client_message, SUDO_DEBUG_UTIL)

    len = client_message__get_packed_size(msg);
    if (len > UINT16_MAX) {
    	sudo_warnx("client message too large: %zu\n", len);
        goto done;
    }
    /* Wire message size is used for length encoding, precedes message. */
    msg_len = htons((uint16_t)len);
    len += sizeof(msg_len);

    if (len > buf->size) {
	sudo_warnx("client message too big for buffer, %zu > %u", len, buf->size);
	goto done;
    }

    memcpy(buf->data, &msg_len, sizeof(msg_len));
    client_message__pack(msg, buf->data + sizeof(msg_len));
    buf->len = len;
    ret = true;

done:
    debug_return_bool(ret);
}

/*
 * Split command + args into an array of strings.
 * Returns an array containing command and args, reusing space in "command".
 * Note that the returned array does not end with a terminating NULL.
 */
static char **
split_command(char *command, size_t *lenp)
{
    char *cp;
    char **args;
    size_t len;
    debug_decl(split_command, SUDO_DEBUG_UTIL)

    for (cp = command, len = 0;;) {
	len++;
	if ((cp = strchr(cp, ' ')) == NULL)
	    break;
	cp++;
    }
    args = reallocarray(NULL, len, sizeof(char *));
    if (args == NULL)
	debug_return_ptr(NULL);

    for (cp = command, len = 0;;) {
	args[len++] = cp;
	if ((cp = strchr(cp, ' ')) == NULL)
	    break;
	*cp++ = '\0';
    }

    *lenp = len;
    debug_return_ptr(args);
}

/*
 * Build and format an ExecMessage wrapped in a ClientMessage.
 * Stores the wire format message in the closure's write buffer.
 * Returns true on success, false on failure.
 */
static bool
fmt_exec_message(struct client_closure *closure)
{
    ClientMessage client_msg = CLIENT_MESSAGE__INIT;
    ExecMessage exec_msg = EXEC_MESSAGE__INIT;
    TimeSpec tv = TIME_SPEC__INIT;
    InfoMessage__StringList runargv = INFO_MESSAGE__STRING_LIST__INIT;
    struct iolog_info *log_info = closure->log_info;
    char hostname[1024];
    bool ret = false;
    size_t n;
    debug_decl(fmt_exec_message, SUDO_DEBUG_UTIL)

    /*
     * Fill in ExecMessage and add it to ClientMessage.
     * TODO: handle buf large than 64K?
     */
    if (gethostname(hostname, sizeof(hostname)) == -1) {
	sudo_warn("gethostname");
	debug_return_bool(false);
    }
    hostname[sizeof(hostname) - 1] = '\0';

    /* Sudo I/O logs only store start time in seconds. */
    tv.tv_sec = log_info->tstamp;
    tv.tv_nsec = 0;
    exec_msg.start_time = &tv;

    /* Split command into a StringList. */
    runargv.strings = split_command(log_info->cmd, &runargv.n_strings);
    if (runargv.strings == NULL)
	sudo_fatal(NULL);

    /* The sudo I/O log info file has limited info. */
    exec_msg.n_info_msgs = 10;
    exec_msg.info_msgs = calloc(exec_msg.n_info_msgs, sizeof(InfoMessage *));
    if (exec_msg.info_msgs == NULL)
	debug_return_bool(false);
    for (n = 0; n < exec_msg.n_info_msgs; n++) {
	exec_msg.info_msgs[n] = malloc(sizeof(InfoMessage));
	if (exec_msg.info_msgs[n] == NULL) {
	    exec_msg.n_info_msgs = n;
	    goto done;
	}
	info_message__init(exec_msg.info_msgs[n]);
    }

    /* Fill in info_msgs */
    n = 0;
    exec_msg.info_msgs[n]->key = "command";
    exec_msg.info_msgs[n]->strval = log_info->cmd;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
    n++;

    exec_msg.info_msgs[n]->key = "columns";
    exec_msg.info_msgs[n]->numval = log_info->cols;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_NUMVAL;
    n++;

    exec_msg.info_msgs[n]->key = "cwd";
    exec_msg.info_msgs[n]->strval = log_info->cwd;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
    n++;

    exec_msg.info_msgs[n]->key = "lines";
    exec_msg.info_msgs[n]->numval = log_info->lines;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_NUMVAL;
    n++;

    exec_msg.info_msgs[n]->key = "runargv";
    exec_msg.info_msgs[n]->strlistval = &runargv;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRLISTVAL;
    n++;

    if (log_info->runas_group != NULL) {
	exec_msg.info_msgs[n]->key = "rungroup";
	exec_msg.info_msgs[n]->strval = log_info->runas_group;
	exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
	n++;
    }

    exec_msg.info_msgs[n]->key = "runuser";
    exec_msg.info_msgs[n]->strval = log_info->runas_user;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
    n++;

    exec_msg.info_msgs[n]->key = "submithost";
    exec_msg.info_msgs[n]->strval = hostname;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
    n++;

    exec_msg.info_msgs[n]->key = "submituser";
    exec_msg.info_msgs[n]->strval = log_info->user;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
    n++;

    exec_msg.info_msgs[n]->key = "ttyname";
    exec_msg.info_msgs[n]->strval = log_info->tty;
    exec_msg.info_msgs[n]->value_case = INFO_MESSAGE__VALUE_STRVAL;
    n++;

    /* Update n_info_msgs. */
    exec_msg.n_info_msgs = n;

    sudo_warnx("sending ExecMessage array length %zu", n); // XXX

    /* Schedule ClientMessage */
    client_msg.exec_msg = &exec_msg;
    client_msg.type_case = CLIENT_MESSAGE__TYPE_EXEC_MSG;
    ret = fmt_client_message(&closure->write_buf, &client_msg);
    if (ret) {
	if (sudo_ev_add(NULL, closure->write_ev, NULL, false) == -1)
	    ret = false;
    }

done:
    for (n = 0; n < exec_msg.n_info_msgs; n++) {
	free(exec_msg.info_msgs[n]);
    }
    free(exec_msg.info_msgs);

    debug_return_bool(ret);
}

/*
 * Build and format an ExitMessage wrapped in a ClientMessage.
 * Stores the wire format message in the closure's write buffer.
 * Returns true on success, false on failure.
 */
static bool
fmt_exit_message(struct client_closure *closure)
{
    ClientMessage client_msg = CLIENT_MESSAGE__INIT;
    ExitMessage exit_msg = EXIT_MESSAGE__INIT;
    bool ret = false;
    debug_decl(fmt_exit_message, SUDO_DEBUG_UTIL)

    /*
     * We don't have enough data in a sudo I/O log to create a real
     * exit message.  For example, the exit value and run time are
     * not known.  This results in a zero-sized message.
     */
    exit_msg.exit_value = 0;

    sudo_warnx("sending ExitMessage"); // XXX

    /* Send ClientMessage */
    client_msg.exit_msg = &exit_msg;
    client_msg.type_case = CLIENT_MESSAGE__TYPE_EXIT_MSG;
    if (!fmt_client_message(&closure->write_buf, &client_msg))
	goto done;

    ret = true;

done:
    debug_return_bool(ret);
}

/*
 * Build and format an IoBuffer wrapped in a ClientMessage.
 * Stores the wire format message in buf.
 * Returns true on success, false on failure.
 */
static bool
fmt_io_buf(int type, struct client_closure *closure,
    struct connection_buffer *buf)
{
    ClientMessage client_msg = CLIENT_MESSAGE__INIT;
    IoBuffer iobuf_msg = IO_BUFFER__INIT;
    TimeSpec delay = TIME_SPEC__INIT;
    bool ret = false;
    debug_decl(fmt_io_buf, SUDO_DEBUG_UTIL)

    if (!read_io_buf(closure))
	goto done;

    /* Fill in IoBuffer. */
    delay.tv_sec = closure->timing.delay.tv_sec;
    delay.tv_nsec = closure->timing.delay.tv_nsec;
    iobuf_msg.delay = &delay;
    iobuf_msg.data.data = (void *)closure->buf;
    iobuf_msg.data.len = closure->timing.u.nbytes;

    /* TODO: split buffer if it is too large */
    sudo_warnx("sending IoBuffer length %zu, type %d, size %zu", iobuf_msg.data.len, type, io_buffer__get_packed_size(&iobuf_msg)); // XXX

    /* Send ClientMessage, it doesn't matter which IoBuffer we set. */
    client_msg.ttyout_buf = &iobuf_msg;
    client_msg.type_case = type;
    if (!fmt_client_message(buf, &client_msg))
        goto done;

    ret = true;

done:
    debug_return_bool(ret);
}

/*
 * Build and format a ChangeWindowSize message wrapped in a ClientMessage.
 * Stores the wire format message in buf.
 * Returns true on success, false on failure.
 */
static bool
fmt_winsize(struct client_closure *closure, struct connection_buffer *buf)
{
    ClientMessage client_msg = CLIENT_MESSAGE__INIT;
    ChangeWindowSize winsize_msg = CHANGE_WINDOW_SIZE__INIT;
    TimeSpec delay = TIME_SPEC__INIT;
    struct timing_closure *timing = &closure->timing;
    bool ret = false;
    debug_decl(fmt_winsize, SUDO_DEBUG_UTIL)

    /* Fill in ChangeWindowSize message. */
    delay.tv_sec = timing->delay.tv_sec;
    delay.tv_nsec = timing->delay.tv_nsec;
    winsize_msg.delay = &delay;
    winsize_msg.rows = timing->u.winsize.lines;
    winsize_msg.cols = timing->u.winsize.cols;

    sudo_warnx("sending ChangeWindowSize, %dx%d, size %zu", winsize_msg.rows, winsize_msg.cols, change_window_size__get_packed_size(&winsize_msg)); // XXX

    /* Send ClientMessage */
    client_msg.winsize_event = &winsize_msg;
    client_msg.type_case = CLIENT_MESSAGE__TYPE_WINSIZE_EVENT;
    if (!fmt_client_message(buf, &client_msg))
        goto done;

    ret = true;

done:
    debug_return_bool(ret);
}

/*
 * Build and format a CommandSuspend message wrapped in a ClientMessage.
 * Stores the wire format message in buf.
 * Returns true on success, false on failure.
 */
static bool
fmt_suspend(struct client_closure *closure, struct connection_buffer *buf)
{
    ClientMessage client_msg = CLIENT_MESSAGE__INIT;
    CommandSuspend suspend_msg = COMMAND_SUSPEND__INIT;
    TimeSpec delay = TIME_SPEC__INIT;
    struct timing_closure *timing = &closure->timing;
    bool ret = false;
    debug_decl(fmt_suspend, SUDO_DEBUG_UTIL)

    /* Fill in CommandSuspend message. */
    delay.tv_sec = timing->delay.tv_sec;
    delay.tv_nsec = timing->delay.tv_nsec;
    suspend_msg.delay = &delay;
    if (sig2str(timing->u.signo, closure->buf) == -1)
	goto done;
    suspend_msg.signal = closure->buf;

    sudo_warnx("sending CommandSuspend, %s, size %zu", suspend_msg.signal, command_suspend__get_packed_size(&suspend_msg)); // XXX

    /* Send ClientMessage */
    client_msg.suspend_event = &suspend_msg;
    client_msg.type_case = CLIENT_MESSAGE__TYPE_SUSPEND_EVENT;
    if (!fmt_client_message(buf, &client_msg))
        goto done;

    ret = true;

done:
    debug_return_bool(ret);
}

/*
 * Read the next entry for the I/O log timing file and format a ClientMessage.
 * Stores the wire format message in the closure's write buffer.
 * Returns true on success, false on failure.
 */ 
static bool
fmt_next_iolog(struct client_closure *closure)
{
    struct timing_closure *timing = &closure->timing;
    struct connection_buffer *buf = &closure->write_buf;
    bool ret = false;
    debug_decl(fmt_next_iolog, SUDO_DEBUG_UTIL)

    if (buf->len != 0) {
	sudo_warnx("%s: write buffer already in use", __func__);
	debug_return_bool(false);
    }

    /* TODO: fill write buffer with multiple messages */
    switch (read_timing_record(timing)) {
    case 0:
	/* OK */
	break;
    case 1:
	/* no more IO buffers */
	closure->state = SEND_EXIT;
	debug_return_bool(fmt_exit_message(closure));
    case -1:
    default:
	debug_return_bool(false);
    }

    switch (timing->event) {
    case IO_EVENT_STDIN:
	ret = fmt_io_buf(CLIENT_MESSAGE__TYPE_STDIN_BUF, closure, buf);
	break;
    case IO_EVENT_STDOUT:
	ret = fmt_io_buf(CLIENT_MESSAGE__TYPE_STDOUT_BUF, closure, buf);
	break;
    case IO_EVENT_STDERR:
	ret = fmt_io_buf(CLIENT_MESSAGE__TYPE_STDERR_BUF, closure, buf);
	break;
    case IO_EVENT_TTYIN:
	ret = fmt_io_buf(CLIENT_MESSAGE__TYPE_TTYIN_BUF, closure, buf);
	break;
    case IO_EVENT_TTYOUT:
	ret = fmt_io_buf(CLIENT_MESSAGE__TYPE_TTYOUT_BUF, closure, buf);
	break;
    case IO_EVENT_WINSIZE:
	ret = fmt_winsize(closure, buf);
	break;
    case IO_EVENT_SUSPEND:
	ret = fmt_suspend(closure, buf);
	break;
    default:
	sudo_warnx("unexpected I/O event %d", timing->event);
	break;
    }

    /* Track elapsed time for comparison with commit points. */
    sudo_timespecadd(&timing->delay, &closure->elapsed, &closure->elapsed);

    debug_return_bool(ret);
}

/*
 * Additional work to do after a ClientMessage was sent to the server.
 * Advances state and formats the next ClientMessage (if any).
 */
static bool
client_message_completion(struct client_closure *closure)
{
    debug_decl(client_message_completion, SUDO_DEBUG_UTIL)

    switch (closure->state) {
    case SEND_EXEC:
	closure->state = SEND_IO;
	/* FALLTHROUGH */
    case SEND_IO:
	/* fmt_next_iolog() will advance state on EOF. */
	if (!fmt_next_iolog(closure))
	    debug_return_bool(false);
	break;
    case SEND_EXIT:
	/* Done writing, just waiting for final commit point. */
	sudo_ev_del(NULL, closure->write_ev);
	closure->state = CLOSING;
	break;
    default:
	sudo_warnx("%s: unexpected state %d", __func__, closure->state);
	debug_return_bool(false);
    }
    debug_return_bool(true);
}

/*
 * Respond to a ServerHello message from the server.
 * Returns true on success, false on error.
 */
static bool
handle_server_hello(ServerHello *msg, struct client_closure *closure)
{
    size_t n;
    debug_decl(handle_server_hello, SUDO_DEBUG_UTIL)

    if (closure->state != RECV_HELLO) {
	sudo_warnx("%s: unexpected state %d", __func__, closure->state);
	debug_return_bool(false);
    }

    /* Sanity check ServerHello message. */
    if (msg->server_id == NULL || msg->server_id[0] == '\0') {
	sudo_warnx("invalid ServerHello");
	debug_return_bool(false);
    }

    printf("Server: %s\n", msg->server_id);
    /* TODO: handle redirect */
    if (msg->redirect != NULL && msg->redirect[0] != '\0')
	printf("Redirect: %s\n", msg->redirect);
    for (n = 0; n < msg->n_servers; n++) {
	printf("Server %zu: %s\n", n + 1, msg->servers[n]);
    }

    debug_return_bool(true);
}

/*
 * Respond to a CommitPoint message from the server.
 * Returns true on success, false on error.
 */
static bool
handle_commit_point(TimeSpec *commit_point, struct client_closure *closure)
{
    debug_decl(handle_commit_point, SUDO_DEBUG_UTIL)

    /* Only valid after we have sent an IO buffer. */
    if (closure->state < SEND_IO) {
	sudo_warnx("%s: unexpected state %d", __func__, closure->state);
	debug_return_bool(false);
    }

    closure->committed.tv_sec = commit_point->tv_sec;
    closure->committed.tv_nsec = commit_point->tv_nsec;

    printf("commit point: %lld %d\n", (long long)commit_point->tv_sec,
	commit_point->tv_nsec); /* XXX */
    debug_return_bool(true);
}

/*
 * Respond to a LogId message from the server.
 * Always returns true.
 */
static bool
handle_log_id(char *id, struct client_closure *closure)
{
    debug_decl(handle_log_id, SUDO_DEBUG_UTIL)

    sudo_warnx("remote log ID: %s", id);
    if ((closure->iolog_dir = strdup(id)) == NULL)
	sudo_fatal(NULL);
    debug_return_bool(true);
}

/*
 * Respond to a ServerError message from the server.
 * Always returns false.
 */
static bool
handle_server_error(char *errmsg, struct client_closure *closure)
{
    debug_decl(handle_server_error, SUDO_DEBUG_UTIL)

    sudo_warnx("server error: %s", errmsg);
    debug_return_bool(false);
}

/*
 * Respond to a ServerAbort message from the server.
 * Always returns false.
 */
static bool
handle_server_abort(char *errmsg, struct client_closure *closure)
{
    debug_decl(handle_server_abort, SUDO_DEBUG_UTIL)

    sudo_warnx("server abort: %s", errmsg);
    debug_return_bool(false);
}

/*
 * Respond to a ServerMessage from the server.
 * Returns true on success, false on error.
 */
static bool
handle_server_message(uint8_t *buf, size_t len,
    struct client_closure *closure)
{
    ServerMessage *msg;
    bool ret = false;
    debug_decl(handle_server_message, SUDO_DEBUG_UTIL)

    sudo_warnx("unpacking ServerMessage"); // XXX
    msg = server_message__unpack(NULL, len, buf);
    if (msg == NULL) {
	sudo_warnx("unable to unpack ServerMessage");
	debug_return_bool(false);
    }

    switch (msg->type_case) {
    case SERVER_MESSAGE__TYPE_HELLO:
	if ((ret = handle_server_hello(msg->hello, closure))) {
	    closure->state = SEND_EXEC;
	    ret = fmt_exec_message(closure);
	}
	break;
    case SERVER_MESSAGE__TYPE_COMMIT_POINT:
	ret = handle_commit_point(msg->commit_point, closure);
	if (sudo_timespeccmp(&closure->elapsed, &closure->committed, ==)) {
	    sudo_ev_del(NULL, closure->read_ev);
	    sudo_ev_loopexit(NULL);
	    closure->state = FINISHED;
	}
	break;
    case SERVER_MESSAGE__TYPE_LOG_ID:
	ret = handle_log_id(msg->log_id, closure);
	break;
    case SERVER_MESSAGE__TYPE_ERROR:
	ret = handle_server_error(msg->error, closure);
	closure->state = ERROR;
	break;
    case SERVER_MESSAGE__TYPE_ABORT:
	ret = handle_server_abort(msg->abort, closure);
	closure->state = ERROR;
	break;
    default:
	/* XXX */
	sudo_warnx("unexpected type_case value %d", msg->type_case);
	break;
    }

    server_message__free_unpacked(msg, NULL);
    debug_return_bool(ret);
}

/*
 * Read and unpack a ServerMessage (read callback).
 */
static void
server_msg_cb(int fd, int what, void *v)
{
    struct client_closure *closure = v;
    struct connection_buffer *buf = &closure->read_buf;
    ssize_t nread;
    uint16_t msg_len;
    debug_decl(server_msg_cb, SUDO_DEBUG_UTIL)

    sudo_warnx("reading server message"); // XXX

    nread = recv(fd, buf->data + buf->len, buf->size - buf->len, 0);
    sudo_warnx("received %zd bytes from server", nread);
    switch (nread) {
    case -1:
	if (errno == EAGAIN)
	    debug_return;
	sudo_warn("recv");
	goto bad;
    case 0:
	sudo_warnx("premature EOF");
	goto bad;
    default:
	break;
    }
    buf->len += nread;

    while (buf->len - buf->off >= sizeof(msg_len)) {
	/* Read wire message size (uint16_t in network byte order). */
	memcpy(&msg_len, buf->data + buf->off, sizeof(msg_len));
	msg_len = ntohs(msg_len);

	if (msg_len + sizeof(msg_len) > buf->len - buf->off) {
	    /* Incomplete message, we'll read the rest next time. */
	    /* TODO: realloc if max message size increases */
	    if (buf->off > 0)
		memmove(buf->data, buf->data + buf->off, buf->len - buf->off);
	    break;
	}

	/* Parse ServerMessage, could be zero bytes. */
	sudo_warnx("parsing ServerMessage, size %hu", msg_len); // XXX
	buf->off += sizeof(msg_len);
	if (!handle_server_message(buf->data + buf->off, msg_len, closure)) {
	    /* XXX - do something on error */
	    goto bad;
	}
	buf->off += msg_len;
    }
    buf->len -= buf->off;
    buf->off = 0;
    debug_return;
bad:
    close(fd);
    client_closure_free(closure);
    debug_return;
}

/*
 * Send a ClientMessage to the server (write callback).
 */
static void
client_msg_cb(int fd, int what, void *v)
{
    struct client_closure *closure = v;
    struct connection_buffer *buf = &closure->write_buf;
    ssize_t nwritten;
    debug_decl(client_msg_cb, SUDO_DEBUG_UTIL)

    sudo_warnx("sending %u bytes to server", buf->len - buf->off);

    nwritten = send(fd, buf->data + buf->off, buf->len - buf->off, 0);
    if (nwritten == -1) {
	sudo_warn("send");
	goto bad;
    }
    buf->off += nwritten;

    if (buf->off == buf->len) {
	/* sent entire message */
	sudo_warnx("finished sending %u bytes to server", buf->len); // XXX
	buf->off = 0;
	buf->len = 0;
	if (!client_message_completion(closure))
	    goto bad;
    }
    debug_return;

bad:
    close(fd);
    client_closure_free(closure);
    debug_return;
}

/*
 * Allocate a new connection closure.
 */
static struct client_closure *
client_closure_alloc(int sock, struct iolog_info *log_info)
{
    struct client_closure *closure;
    debug_decl(client_closure_alloc, SUDO_DEBUG_UTIL)

    if ((closure = calloc(1, sizeof(*closure))) == NULL)
	debug_return_ptr(NULL);

    closure->state = RECV_HELLO;
    closure->log_info = log_info;

    closure->bufsize = 10240;
    closure->buf = malloc(closure->bufsize);
    if (closure->buf == NULL)
	goto bad;

    closure->read_buf.size = UINT16_MAX + sizeof(uint16_t);
    closure->read_buf.data = malloc(closure->read_buf.size);
    if (closure->read_buf.data == NULL)
	goto bad;

    closure->write_buf.size = UINT16_MAX + sizeof(uint16_t);
    closure->write_buf.data = malloc(closure->write_buf.size);
    if (closure->write_buf.data == NULL)
	goto bad;

    closure->read_ev = sudo_ev_alloc(sock, SUDO_EV_READ|SUDO_EV_PERSIST,
	server_msg_cb, closure);
    if (closure->read_ev == NULL)
	goto bad;

    closure->write_ev = sudo_ev_alloc(sock, SUDO_EV_WRITE|SUDO_EV_PERSIST,
	client_msg_cb, closure);
    if (closure->write_ev == NULL)
	goto bad;

    debug_return_ptr(closure);
bad:
    client_closure_free(closure);
    debug_return_ptr(NULL);
}

/*
 * Open any I/O log files that are present.
 * The timing file must always exist.
 */
bool
iolog_open_all(const char *iolog_path)
{
    char fname[PATH_MAX];
    int i, len;
    debug_decl(iolog_open_all, SUDO_DEBUG_UTIL)

    for (i = 0; iolog_names[i] != NULL; i++) {
	len = snprintf(fname, sizeof(fname), "%s/%s", iolog_path,
	    iolog_names[i]);
	if (len < 0 || len >= ssizeof(fname)) {
	    errno = ENAMETOOLONG;
	    sudo_warn("%s/%s", iolog_path, iolog_names[i]);
	}
	io_fds[i] = gzopen(fname, "r");
	if (io_fds[i] == NULL && i == IOFD_TIMING) {
	    /* The timing file is not optional. */
	    sudo_warn("unable to open %s/%s", iolog_path, iolog_names[i]);
	    debug_return_bool(false);
	}
    }
    debug_return_bool(true);
}

int
main(int argc, char *argv[])
{
    struct client_closure *closure;
    struct sudo_event_base *evbase;
    struct iolog_info *log_info;
    const char *host = "localhost";
    const char *port = DEFAULT_PORT_STR;
    char fname[PATH_MAX];
    char *iolog_path;
    int ch, sock;
    debug_decl_vars(main, SUDO_DEBUG_MAIN)

    initprogname(argc > 0 ? argv[0] : "sendlog");
    setlocale(LC_ALL, "");
    bindtextdomain("sudo", LOCALEDIR); /* XXX - add logsrvd domain */
    textdomain("sudo");

    /* Read sudo.conf and initialize the debug subsystem. */
    if (sudo_conf_read(NULL, SUDO_CONF_DEBUG) == -1)
        exit(EXIT_FAILURE);
    sudo_debug_register(getprogname(), NULL, NULL,
        sudo_conf_debug_files(getprogname()));

    if (protobuf_c_version_number() < 1003000)
	sudo_fatalx("Protobuf-C version 1.3 or higher required");

    while ((ch = getopt(argc, argv, "h:p:")) != -1) {
	switch (ch) {
	case 'h':
	    host = optarg;
	    break;
	case 'p':
	    port = optarg;
	    break;
	default:
	    usage();
	}
    }
    argc -= optind;
    argv += optind;

    /* Remaining arg should be path to I/O log file to send. */
    if (argc != 1)
	usage();
    iolog_path = argv[0];

    signal(SIGPIPE, SIG_IGN);

    /* Parse I/O info log file. */
    snprintf(fname, sizeof(fname), "%s/log", iolog_path);
    if ((log_info = parse_logfile(fname)) == NULL)
	goto bad;
    sudo_warnx("parsed log file %s", fname); // XXX

    /* Open the I/O log files. */
    if (!iolog_open_all(iolog_path))
	goto bad;

    /* Connect to server, setup events. */
    sock = connect_server(host, port);
    if (sock == -1)
	goto bad;
    sudo_warnx("connected to %s:%s", host, port); // XXX

    if ((evbase = sudo_ev_base_alloc()) == NULL)
	sudo_fatal(NULL);
    sudo_ev_base_setdef(evbase);

    if ((closure = client_closure_alloc(sock, log_info)) == NULL)
	goto bad;

    /* Add read event for the server hello message and enter event loop. */
    if (sudo_ev_add(evbase, closure->read_ev, NULL, false) == -1)
	goto bad;
    sudo_ev_dispatch(evbase);

    if (!sudo_timespeccmp(&closure->elapsed, &closure->committed, ==)) {
	sudo_warnx("commit point mismatch, expected [%lld, %ld], got [%lld, %ld]",
	    (long long)closure->elapsed.tv_sec, closure->elapsed.tv_nsec, 
	    (long long)closure->committed.tv_sec, closure->committed.tv_nsec);
    }

    if (closure->state != FINISHED) {
	sudo_warnx("exited prematurely with state %d", closure->state);
	goto bad;
    }

    debug_return_int(EXIT_SUCCESS);
bad:
    debug_return_int(EXIT_FAILURE);
}
