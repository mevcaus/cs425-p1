#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

// send() raises SIGPIPE when the peer has gone away; ask for EPIPE instead.
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

__attribute__((format(printf, 3, 4)))
static void set_error(char *err, size_t err_size, const char *fmt, ...) {
    if (!err || err_size == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

/**
 * LAYER 1: Pure Protocol Helpers
 */

bool has_crlf(const char *s) {
    return s && strpbrk(s, "\r\n") != NULL;
}

int parse_reply_code(const char *reply_line) {
    if (!reply_line) return -1;
    const char *s = reply_line;

    // Reply-code = %x32-35 %x30-35 %x30-39 (RFC 5321 section 4.2)
    if (s[0] < '2' || s[0] > '5' || s[1] < '0' || s[1] > '5' ||
        s[2] < '0' || s[2] > '9') {
        return -1;
    }
    // The code is followed by SP, "-" or the end of the line.
    if (s[3] != ' ' && s[3] != '-' && s[3] != '\r' && s[3] != '\n' && s[3] != '\0') {
        return -1;
    }
    return (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
}

bool is_final_reply_line(const char *reply_line) {
    // Only a valid line with a hyphen after the code is a continuation.
    return parse_reply_code(reply_line) < 0 || reply_line[3] != '-';
}

char *build_command(const char *verb, const char *arg) {
    if (!verb || has_crlf(verb) || has_crlf(arg)) return NULL;

    int len = arg ? snprintf(NULL, 0, "%s %s\r\n", verb, arg)
                  : snprintf(NULL, 0, "%s\r\n", verb);
    if (len < 0) { // GCOVR_EXCL_START
        return NULL;
    } // GCOVR_EXCL_STOP

    size_t size = (size_t)len + 1;
    char *res = malloc(size);
    if (!res) { // GCOVR_EXCL_START
        return NULL;
    } // GCOVR_EXCL_STOP

    if (arg) {
        snprintf(res, size, "%s %s\r\n", verb, arg);
    } else {
        snprintf(res, size, "%s\r\n", verb);
    }
    return res;
}

char *build_envelope_command(const char *verb, const char *address) {
    if (!verb || !address || has_crlf(verb) || has_crlf(address)) return NULL;

    int len = snprintf(NULL, 0, "%s:<%s>\r\n", verb, address);
    if (len < 0) { // GCOVR_EXCL_START
        return NULL;
    } // GCOVR_EXCL_STOP

    size_t size = (size_t)len + 1;
    char *res = malloc(size);
    if (!res) { // GCOVR_EXCL_START
        return NULL;
    } // GCOVR_EXCL_STOP

    snprintf(res, size, "%s:<%s>\r\n", verb, address);
    return res;
}

// Writes the wire form of body into out and returns its length. With out
// NULL it only counts, so dot_stuff can allocate exactly once.
static size_t stuff_into(const char *body, char *out) {
    size_t n = 0;
    bool line_start = true;

    for (const char *p = body; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            // LF, CRLF and a bare CR all end a line, and all become CRLF.
            if (p[0] == '\r' && p[1] == '\n') p++;
            if (out) {
                out[n] = '\r';
                out[n + 1] = '\n';
            }
            n += 2;
            line_start = true;
            continue;
        }
        // A line that starts with a period gets a second one.
        if (line_start && *p == '.') {
            if (out) out[n] = '.';
            n++;
        }
        if (out) out[n] = *p;
        n++;
        line_start = false;
    }
    return n;
}

char *dot_stuff(const char *body) {
    if (!body) return NULL;

    size_t len = stuff_into(body, NULL);
    char *res = malloc(len + 1);
    if (!res) { // GCOVR_EXCL_START
        return NULL;
    } // GCOVR_EXCL_STOP

    stuff_into(body, res);
    res[len] = '\0';
    return res;
}

// Headers, blank line, body, and the "." line that ends the message.
#define PAYLOAD_FMT "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n%s%s.\r\n"

char *build_data_payload(const char *from, const char *to, const char *subject, const char *body) {
    if (!subject) subject = "";
    if (!from || !to || has_crlf(from) || has_crlf(to) || has_crlf(subject)) {
        return NULL;
    }

    char *stuffed = dot_stuff(body ? body : "");
    if (!stuffed) { // GCOVR_EXCL_START
        return NULL;
    } // GCOVR_EXCL_STOP

    // The final "." has to start a line, so a body that does not already
    // end with CRLF gets one.
    size_t stuffed_len = strlen(stuffed);
    const char *eol = (stuffed_len > 0 && stuffed[stuffed_len - 1] != '\n') ? "\r\n" : "";

    int len = snprintf(NULL, 0, PAYLOAD_FMT, from, to, subject, stuffed, eol);
    if (len < 0) { // GCOVR_EXCL_START
        free(stuffed);
        return NULL;
    } // GCOVR_EXCL_STOP

    size_t size = (size_t)len + 1;
    char *res = malloc(size);
    if (!res) { // GCOVR_EXCL_START
        free(stuffed);
        return NULL;
    } // GCOVR_EXCL_STOP

    snprintf(res, size, PAYLOAD_FMT, from, to, subject, stuffed, eol);
    free(stuffed);
    return res;
}

/**
 * LAYER 2: The Session
 */

void transport_init(transport_t *t, transport_read_fn read, transport_write_fn write, void *ctx) {
    if (!t) return;
    t->read = read;
    t->write = write;
    t->ctx = ctx;
    t->len = 0;
}

int read_line(transport_t *t, char *line, size_t size) {
    if (!t || !t->read || !line || size == 0) return SMTP_ERR_ARG;

    while (1) {
        // Hand out a line if the buffer already holds a complete one.
        char *nl = memchr(t->buf, '\n', t->len);
        if (nl) {
            size_t consumed = (size_t)(nl - t->buf) + 1;
            size_t n = consumed - 1;
            if (n > 0 && t->buf[n - 1] == '\r') n--;
            if (n >= size) return SMTP_ERR_TOO_LONG;

            memcpy(line, t->buf, n);
            line[n] = '\0';
            t->len -= consumed;
            memmove(t->buf, t->buf + consumed, t->len);
            return (int)n;
        }

        // Otherwise refill it, unless a whole buffer holds no line ending.
        size_t room = sizeof(t->buf) - t->len;
        if (room == 0) return SMTP_ERR_TOO_LONG;

        ssize_t got = t->read(t->ctx, t->buf + t->len, room);
        if (got == 0) return SMTP_ERR_CLOSED;
        if (got < 0 || (size_t)got > room) return SMTP_ERR_IO;
        t->len += (size_t)got;
    }
}

int read_full_reply(transport_t *t, char *reply, size_t size) {
    if (!t || !reply || size == 0) return SMTP_ERR_ARG;

    char line[SMTP_LINE_MAX];
    size_t used = 0;
    int first_code = -1;
    reply[0] = '\0';

    while (1) {
        int n = read_line(t, line, sizeof(line));
        if (n < 0) return n;

        // Keep every line, even a bad one, so the caller can report it.
        size_t sep = used > 0 ? 1 : 0;
        if (used + sep + (size_t)n >= size) return SMTP_ERR_TOO_LONG;
        if (sep) reply[used++] = '\n';
        memcpy(reply + used, line, (size_t)n + 1);
        used += (size_t)n;

        // Every line of a multi-line reply must carry the same code.
        int code = parse_reply_code(line);
        if (code < 0 || (first_code >= 0 && code != first_code)) {
            return SMTP_ERR_SYNTAX;
        }
        first_code = code;

        if (is_final_reply_line(line)) return code;
    }
}

int write_all(transport_t *t, const char *data, size_t len) {
    if (!t || !t->write || !data) return SMTP_ERR_ARG;

    size_t sent = 0;
    while (sent < len) {
        ssize_t n = t->write(t->ctx, data + sent, len - sent);
        if (n <= 0 || (size_t)n > len - sent) return SMTP_ERR_IO;
        sent += (size_t)n;
    }
    return 0;
}

int expect_reply(transport_t *t, int expected, char *reply, size_t size) {
    int code = read_full_reply(t, reply, size);
    if (code < 0) return code;
    return code == expected ? 0 : SMTP_ERR_UNEXPECTED;
}

int send_command(transport_t *t, const char *cmd, int expected, char *reply, size_t size) {
    if (!cmd) return SMTP_ERR_ARG;

    int rc = write_all(t, cmd, strlen(cmd));
    if (rc < 0) return rc;
    return expect_reply(t, expected, reply, size);
}

// Turns a failed step into a message that says what the server sent.
static void describe_failure(char *err, size_t err_size, const char *step,
                             int expected, int rc, const char *reply) {
    switch (rc) {
        case SMTP_ERR_UNEXPECTED:
            set_error(err, err_size, "%s: expected %d but the server replied: %s",
                      step, expected, reply);
            break;
        case SMTP_ERR_SYNTAX:
            set_error(err, err_size, "%s: the server sent a malformed reply: %s", step, reply);
            break;
        case SMTP_ERR_CLOSED:
            set_error(err, err_size, "%s: the server closed the connection", step);
            break;
        case SMTP_ERR_TOO_LONG:
            set_error(err, err_size, "%s: the server's reply is too long", step);
            break;
        default:
            set_error(err, err_size, "%s: error reading from or writing to the connection", step);
            break;
    }
}

int run_smtp_session(transport_t *t, const smtp_message_t *msg, char *err, size_t err_size) {
    set_error(err, err_size, "%s", "");
    if (!t || !msg) {
        set_error(err, err_size, "no transport or message");
        return 2;
    }

    enum { STEPS = 7 };
    const char *names[STEPS] = {
        "greeting", "HELO", "MAIL FROM", "RCPT TO", "DATA", "end of data", "QUIT"
    };
    const int expected[STEPS] = { 220, 250, 250, 250, 354, 250, 221 };

    // Build every command up front so a bad message sends nothing at all.
    char *cmds[STEPS] = {
        NULL, // the greeting is only read
        msg->helo ? build_command("HELO", msg->helo) : NULL,
        build_envelope_command("MAIL FROM", msg->from),
        build_envelope_command("RCPT TO", msg->to),
        build_command("DATA", NULL),
        build_data_payload(msg->from, msg->to, msg->subject, msg->body),
        build_command("QUIT", NULL),
    };

    int result = 0;
    for (int i = 1; i < STEPS; i++) {
        if (!cmds[i]) {
            set_error(err, err_size, "invalid message: a value is missing, or an address, "
                                     "subject or host name contains a CR or LF");
            result = 2;
            break;
        }
    }

    char reply[4 * SMTP_LINE_MAX];
    reply[0] = '\0';
    for (int i = 0; i < STEPS && result == 0; i++) {
        int rc = cmds[i] ? send_command(t, cmds[i], expected[i], reply, sizeof(reply))
                         : expect_reply(t, expected[i], reply, sizeof(reply));
        if (rc != 0) {
            describe_failure(err, err_size, names[i], expected[i], rc, reply);
            result = 2;
        }
    }

    for (int i = 0; i < STEPS; i++) {
        free(cmds[i]);
    }
    return result;
}

/**
 * LAYER 3: The Socket Transport
 */

int socket_connect(const char *host, const char *port, char *err, size_t err_size) {
    if (!host || !port) {
        set_error(err, err_size, "no server or port given");
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        set_error(err, err_size, "cannot resolve %s port %s: %s", host, port, gai_strerror(rc));
        return -1;
    }

    // Try each address in turn until one accepts the connection.
    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { // GCOVR_EXCL_START
            saved_errno = errno;
            continue;
        } // GCOVR_EXCL_STOP
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;

        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        set_error(err, err_size, "cannot connect to %s port %s: %s", host, port, strerror(saved_errno));
    }
    return fd;
}

ssize_t socket_read(void *ctx, char *buf, size_t len) {
    return recv(*(int *)ctx, buf, len, 0);
}

ssize_t socket_write(void *ctx, const char *data, size_t len) {
    return send(*(int *)ctx, data, len, SEND_FLAGS);
}

char *get_greeting(const char *restrict name)
{
  if (name == NULL)
  {
    return NULL;
  }

  // Allocate memory for the greeting message
  int length = snprintf(NULL, 0, "Hello, %s!", name);
  if (length < 0) // GCOVR_EXCL_START
  {
    return NULL; // snprintf failed
  } // GCOVR_EXCL_STOP

  //Casting is safe here because we know length is non-negative
  size_t alloc_size = (size_t) length + 1; // +1 for the null terminator
  char *greeting = malloc( alloc_size);


  if (greeting == NULL) // GCOVR_EXCL_START
  {
    return NULL; // Memory allocation failed
  }  // GCOVR_EXCL_STOP


  // Create the greeting message
  snprintf(greeting, alloc_size, "Hello, %s!", name);

  return greeting;
}
