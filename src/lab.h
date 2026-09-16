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

/** * @brief Returns a greeting message.
 *
 * This function returns a string that contains a greeting message.
 * The string is allocated with malloc and should be freed by the caller.
 * @param name The name to include in the greeting.
 * @return A greeting string.
 */
char* get_greeting(const char* restrict name);

#endif // LAB_H
