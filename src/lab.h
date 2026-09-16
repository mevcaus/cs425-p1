#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * The client is split into three layers so that everything except the
 * socket calls themselves can be unit tested without a network:
 *
 *   1. Pure protocol helpers  - strings in, strings or codes out, no I/O.
 *   2. The session            - line reader, reply reader and the SMTP
 *                               command sequence, all done through the
 *                               read/write callbacks in transport_t.
 *   3. The socket transport   - getaddrinfo/connect/recv/send wrappers that
 *                               satisfy those callbacks.
 */

/* RFC 5321 4.5.3.1.5 caps a reply line at 512 octets; allow some slack. */
#define SMTP_LINE_MAX 1024

/* Error values returned by the layer 2 functions. All are negative. */
#define SMTP_ERR_ARG        -1 /* a required argument was NULL or empty */
#define SMTP_ERR_IO         -2 /* the read or write callback failed */
#define SMTP_ERR_CLOSED     -3 /* the server hung up */
#define SMTP_ERR_TOO_LONG   -4 /* a line or reply did not fit its buffer */
#define SMTP_ERR_SYNTAX     -5 /* the server sent something that is not a reply */
#define SMTP_ERR_UNEXPECTED -6 /* a valid reply arrived with the wrong code */

/* ------------------------------------------------------------------------
 * LAYER 1: Pure protocol helpers
 * ------------------------------------------------------------------------ */

/**
 * @brief Checks a string for CR or LF characters.
 *
 * A CR or LF inside an address, subject or host name would let whoever
 * supplied it inject an extra SMTP command or mail header.
 *
 * @param s The string to check, may be NULL.
 * @return true if s contains a CR or LF, false otherwise (including NULL).
 */
bool has_crlf(const char *s);

/**
 * @brief Parses the three digit code at the start of a reply line.
 *
 * The code must match RFC 5321 (first digit 2-5, second 0-5, third 0-9)
 * and be followed by a space, a hyphen or the end of the line.
 *
 * @param reply_line One line of a server reply, with or without its CRLF.
 * @return The reply code, or -1 if the line is not a valid reply line.
 */
int parse_reply_code(const char *reply_line);

/**
 * @brief Decides whether a reply line is the last line of its reply.
 *
 * Every line but the last has a hyphen after the code ("250-PIPELINING").
 * A line that is not a valid reply line is never a continuation.
 *
 * @param reply_line One line of a server reply.
 * @return false only for a valid continuation line, true otherwise.
 */
bool is_final_reply_line(const char *reply_line);

/**
 * @brief Builds a command line such as "HELO example.com\r\n".
 *
 * @param verb The command verb, for example "HELO" or "DATA".
 * @param arg  The argument, or NULL for a command without one.
 * @return A malloc'd CRLF terminated line the caller must free, or NULL if
 *         verb is NULL or either string contains a CR or LF.
 */
char *build_command(const char *verb, const char *arg);

/**
 * @brief Builds an envelope command such as "MAIL FROM:<a@b.com>\r\n".
 *
 * @param verb    "MAIL FROM" or "RCPT TO".
 * @param address The mailbox, without angle brackets.
 * @return A malloc'd CRLF terminated line the caller must free, or NULL if
 *         either argument is NULL or contains a CR or LF.
 */
char *build_envelope_command(const char *verb, const char *address);

/**
 * @brief Converts a message body into the form it takes on the wire.
 *
 * Every line ending (LF, CRLF or a bare CR) becomes CRLF, and every line
 * that starts with a period gets a second one (RFC 5321 4.5.2). The
 * terminating "." line is not added here.
 *
 * @param body The message body, may use any line endings.
 * @return A malloc'd string the caller must free, or NULL if body is NULL.
 */
char *dot_stuff(const char *body);

/**
 * @brief Builds everything the client sends after the 354 reply to DATA.
 *
 * That is the From, To and Subject headers, a blank line, the dot stuffed
 * body ending in CRLF, and the "." line that ends the message.
 *
 * @param from    The From header value.
 * @param to      The To header value.
 * @param subject The Subject header value, NULL is treated as empty.
 * @param body    The message body, NULL is treated as empty.
 * @return A malloc'd string the caller must free, or NULL if from or to is
 *         NULL or any header value contains a CR or LF.
 */
char *build_data_payload(const char *from, const char *to,
                         const char *subject, const char *body);

/* ------------------------------------------------------------------------
 * LAYER 2: The session, over a swappable transport
 * ------------------------------------------------------------------------ */

/**
 * Reads up to len bytes into buf, like recv(2).
 * Returns the number of bytes read, 0 when the peer has hung up, or -1.
 */
typedef ssize_t (*transport_read_fn)(void *ctx, char *buf, size_t len);

/**
 * Writes up to len bytes from data, like send(2).
 * Returns the number of bytes written (possibly fewer than len), or -1.
 */
typedef ssize_t (*transport_write_fn)(void *ctx, const char *data, size_t len);

/**
 * A byte stream plus the buffer the line reader keeps between calls. The
 * real client points the callbacks at a socket; the tests point them at a
 * scripted in-memory server.
 */
typedef struct {
    transport_read_fn read;
    transport_write_fn write;
    void *ctx;
    char buf[SMTP_LINE_MAX]; /* bytes received but not yet returned */
    size_t len;              /* number of valid bytes in buf */
} transport_t;

/** The message a session sends. */
typedef struct {
    const char *helo;    /* host name sent with HELO */
    const char *from;    /* envelope sender and From header */
    const char *to;      /* envelope recipient and To header */
    const char *subject; /* Subject header, NULL means empty */
    const char *body;    /* message body, NULL means empty */
} smtp_message_t;

/**
 * @brief Initializes a transport with an empty read buffer.
 *
 * @param t     The transport to initialize.
 * @param read  The read callback.
 * @param write The write callback.
 * @param ctx   Passed unchanged to both callbacks.
 */
void transport_init(transport_t *t, transport_read_fn read,
                    transport_write_fn write, void *ctx);

/**
 * @brief Reads one line from the transport.
 *
 * The buffer is only refilled when it does not already hold a complete
 * line, so a line split across reads and several lines arriving in one read
 * are both handled. The line ending (LF or CRLF) is removed.
 *
 * @param t    The transport.
 * @param line Receives the NUL terminated line.
 * @param size The size of line.
 * @return The length of the line, or SMTP_ERR_ARG, SMTP_ERR_IO,
 *         SMTP_ERR_CLOSED or SMTP_ERR_TOO_LONG.
 */
int read_line(transport_t *t, char *line, size_t size);

/**
 * @brief Reads a complete reply, following continuation lines.
 *
 * The lines are stored in reply separated by '\n'. When a line is not a
 * valid reply line it is still stored, so the caller can report it.
 *
 * @param t     The transport.
 * @param reply Receives the reply text.
 * @param size  The size of reply.
 * @return The reply code, or SMTP_ERR_ARG, SMTP_ERR_IO, SMTP_ERR_CLOSED,
 *         SMTP_ERR_TOO_LONG or SMTP_ERR_SYNTAX (also used when the lines of
 *         one reply carry different codes).
 */
int read_full_reply(transport_t *t, char *reply, size_t size);

/**
 * @brief Writes all len bytes, retrying after short writes.
 *
 * @param t    The transport.
 * @param data The bytes to write.
 * @param len  The number of bytes to write.
 * @return 0 on success, SMTP_ERR_ARG or SMTP_ERR_IO.
 */
int write_all(transport_t *t, const char *data, size_t len);

/**
 * @brief Reads a reply and checks its code.
 *
 * @param t        The transport.
 * @param expected The code the protocol requires at this point.
 * @param reply    Receives the reply text.
 * @param size     The size of reply.
 * @return 0 if the reply carried the expected code, SMTP_ERR_UNEXPECTED if
 *         it carried another one (the text is in reply), or any error from
 *         read_full_reply.
 */
int expect_reply(transport_t *t, int expected, char *reply, size_t size);

/**
 * @brief Sends one command and checks the code of the reply.
 *
 * @param t        The transport.
 * @param cmd      The complete command, including its CRLF.
 * @param expected The code the protocol requires in reply.
 * @param reply    Receives the reply text.
 * @param size     The size of reply.
 * @return The same values as expect_reply, or SMTP_ERR_ARG / SMTP_ERR_IO
 *         if the command could not be written.
 */
int send_command(transport_t *t, const char *cmd, int expected,
                 char *reply, size_t size);

/**
 * @brief Runs a whole SMTP session: greeting, HELO, MAIL FROM, RCPT TO,
 * DATA, the message and QUIT, requiring 220, 250, 250, 250, 354, 250, 221.
 *
 * The session stops at the first failure without sending anything else.
 *
 * @param t        The transport, already connected.
 * @param msg      The message to send.
 * @param err      Receives a description of the failure, may be NULL.
 * @param err_size The size of err.
 * @return 0 when the server queued the message, 2 otherwise.
 */
int run_smtp_session(transport_t *t, const smtp_message_t *msg,
                     char *err, size_t err_size);

/* ------------------------------------------------------------------------
 * LAYER 3: The socket transport
 * ------------------------------------------------------------------------ */

/**
 * @brief Resolves host with getaddrinfo and connects to the first address
 * that accepts a TCP connection.
 *
 * @param host     Host name or address of the server.
 * @param port     Port number or service name.
 * @param err      Receives a description of the failure, may be NULL.
 * @param err_size The size of err.
 * @return A connected socket the caller must close, or -1.
 */
int socket_connect(const char *host, const char *port,
                   char *err, size_t err_size);

/**
 * @brief transport_read_fn over recv(2). ctx points at an int socket.
 */
ssize_t socket_read(void *ctx, char *buf, size_t len);

/**
 * @brief transport_write_fn over send(2). ctx points at an int socket.
 */
ssize_t socket_write(void *ctx, const char *data, size_t len);

/** * @brief Returns a greeting message.
 *
 * This function returns a string that contains a greeting message.
 * The string is allocated with malloc and should be freed by the caller.
 * @param name The name to include in the greeting.
 * @return A greeting string.
 */
char* get_greeting(const char* restrict name);

#endif // LAB_H
