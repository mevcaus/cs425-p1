#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "harness/unity.h"
#include "../src/lab.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------------
 * A scripted in-memory server
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *script;   // everything the server will send
    size_t len;
    size_t pos;
    size_t chunk;         // most bytes handed out per read, 0 = no limit
    bool fail_at_end;     // read fails instead of reporting a hang-up
    int reads;

    char sent[8192];      // everything the client wrote
    size_t sent_len;
    size_t write_chunk;   // most bytes accepted per write, 0 = no limit
    int fail_write_at;    // this write call fails, 0 = never
    int writes;
} mock_t;

static ssize_t mock_read(void *ctx, char *buf, size_t len) {
    mock_t *m = (mock_t *)ctx;
    m->reads++;
    size_t n = m->len - m->pos;
    if (n == 0 && m->fail_at_end) return -1;
    if (m->chunk && n > m->chunk) n = m->chunk;
    if (n > len) n = len;
    memcpy(buf, m->script + m->pos, n);
    m->pos += n;
    return (ssize_t)n;
}

static ssize_t mock_write(void *ctx, const char *data, size_t len) {
    mock_t *m = (mock_t *)ctx;
    m->writes++;
    if (m->fail_write_at == m->writes) return -1;
    if (m->write_chunk && len > m->write_chunk) len = m->write_chunk;
    if (len >= sizeof(m->sent) - m->sent_len) return -1;
    memcpy(m->sent + m->sent_len, data, len);
    m->sent_len += len;
    m->sent[m->sent_len] = '\0';
    return (ssize_t)len;
}

static ssize_t failing_read(void *ctx, char *buf, size_t len) {
    (void)ctx; (void)buf; (void)len;
    return -1;
}

static ssize_t oversized_read(void *ctx, char *buf, size_t len) {
    (void)ctx; (void)buf;
    return (ssize_t)len + 1;
}

static ssize_t failing_write(void *ctx, const char *data, size_t len) {
    (void)ctx; (void)data; (void)len;
    return -1;
}

static ssize_t zero_write(void *ctx, const char *data, size_t len) {
    (void)ctx; (void)data; (void)len;
    return 0;
}

static ssize_t oversized_write(void *ctx, const char *data, size_t len) {
    (void)ctx; (void)data;
    return (ssize_t)len + 1;
}

static void mock_start(mock_t *m, transport_t *t, const char *script, size_t chunk) {
    memset(m, 0, sizeof(*m));
    m->script = script;
    m->len = strlen(script);
    m->chunk = chunk;
    transport_init(t, mock_read, mock_write, m);
}

/* The session from the assignment, one reply and one command per step. */
enum { STEPS = 7 };

static const char *good_replies[STEPS] = {
    "220 smtp.example.com ESMTP ready\r\n",
    "250 smtp.example.com\r\n",
    "250 2.1.0 Ok\r\n",
    "250 2.1.5 Ok\r\n",
    "354 End data with <CR><LF>.<CR><LF>\r\n",
    "250 2.0.0 Ok: queued\r\n",
    "221 2.0.0 Bye\r\n",
};

static const char *good_commands[STEPS] = {
    "",
    "HELO onyx.boisestate.edu\r\n",
    "MAIL FROM:<me@boisestate.edu>\r\n",
    "RCPT TO:<you@example.com>\r\n",
    "DATA\r\n",
    "From: me@boisestate.edu\r\n"
    "To: you@example.com\r\n"
    "Subject: hello\r\n"
    "\r\n"
    "This is the message body.\r\n"
    ".\r\n",
    "QUIT\r\n",
};

static const char *step_names[STEPS] = {
    "greeting", "HELO", "MAIL FROM", "RCPT TO", "DATA", "end of data", "QUIT"
};

static const smtp_message_t good_msg = {
    .helo = "onyx.boisestate.edu",
    .from = "me@boisestate.edu",
    .to = "you@example.com",
    .subject = "hello",
    .body = "This is the message body.\n", // what echo pipes in
};

// The server's side of the session: the replies before step `upto`, then
// `replacement` in place of the reply to that step (NULL to stop there),
// then the rest of the replies.
static void build_script(char *out, size_t size, int upto, const char *replacement) {
    out[0] = '\0';
    for (int i = 0; i < STEPS; i++) {
        const char *r = good_replies[i];
        if (i == upto) {
            if (!replacement) break;
            r = replacement;
        }
        strncat(out, r, size - strlen(out) - 1);
    }
}

// What the client should have sent by the time it reads the reply to `step`.
static void build_transcript(char *out, size_t size, int step) {
    out[0] = '\0';
    for (int i = 1; i <= step && i < STEPS; i++) {
        strncat(out, good_commands[i], size - strlen(out) - 1);
    }
}

/* ------------------------------------------------------------------------
 * LAYER 1: Pure protocol helpers
 * ------------------------------------------------------------------------ */

void test_has_crlf(void) {
    TEST_ASSERT_FALSE(has_crlf(NULL));
    TEST_ASSERT_FALSE(has_crlf(""));
    TEST_ASSERT_FALSE(has_crlf("you@example.com"));
    TEST_ASSERT_TRUE(has_crlf("a\rb"));
    TEST_ASSERT_TRUE(has_crlf("a\nb"));
    TEST_ASSERT_TRUE(has_crlf("you@example.com>\r\nRCPT TO:<other@example.com"));
}

void test_parse_reply_code(void) {
    TEST_ASSERT_EQUAL_INT(220, parse_reply_code("220 smtp.example.com ESMTP ready"));
    TEST_ASSERT_EQUAL_INT(250, parse_reply_code("250-PIPELINING"));
    TEST_ASSERT_EQUAL_INT(354, parse_reply_code("354 End data with ."));
    TEST_ASSERT_EQUAL_INT(221, parse_reply_code("221"));
    TEST_ASSERT_EQUAL_INT(250, parse_reply_code("250\r\n"));
    TEST_ASSERT_EQUAL_INT(421, parse_reply_code("421\n"));
    TEST_ASSERT_EQUAL_INT(559, parse_reply_code("559 highest code"));
    TEST_ASSERT_EQUAL_INT(200, parse_reply_code("200 lowest code"));

    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code(NULL));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code(""));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("2"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("22"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("abc"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code(" 250 leading space"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("150 first digit too low"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("650 first digit too high"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("2/0 second digit not a digit"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("260 second digit too high"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("25/ third digit not a digit"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("25a third digit not a digit"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("2500 four digits"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("250x"));
}

void test_is_final_reply_line(void) {
    TEST_ASSERT_TRUE(is_final_reply_line("250 smtp.example.com"));
    TEST_ASSERT_TRUE(is_final_reply_line("250 SIZE 10240000"));
    TEST_ASSERT_FALSE(is_final_reply_line("250-smtp.example.com"));
    TEST_ASSERT_FALSE(is_final_reply_line("250-"));
    // A bare code is a complete reply line (RFC 5321 section 4.2).
    TEST_ASSERT_TRUE(is_final_reply_line("250"));
    TEST_ASSERT_TRUE(is_final_reply_line("250\r\n"));
    // Lines that are not replies are never continuations.
    TEST_ASSERT_TRUE(is_final_reply_line(NULL));
    TEST_ASSERT_TRUE(is_final_reply_line(""));
    TEST_ASSERT_TRUE(is_final_reply_line("25-x"));
    TEST_ASSERT_TRUE(is_final_reply_line("abc-def"));
}

void test_build_command(void) {
    char *cmd = build_command("HELO", "localhost");
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("HELO localhost\r\n", cmd);
    free(cmd);

    cmd = build_command("DATA", NULL);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("DATA\r\n", cmd);
    free(cmd);

    cmd = build_command("QUIT", NULL);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("QUIT\r\n", cmd);
    free(cmd);

    TEST_ASSERT_NULL(build_command(NULL, NULL));
    TEST_ASSERT_NULL(build_command(NULL, "localhost"));
    TEST_ASSERT_NULL(build_command("HE\rLO", "localhost"));
    TEST_ASSERT_NULL(build_command("HELO", "localhost\r\nRSET"));
    TEST_ASSERT_NULL(build_command("HELO", "localhost\n"));
}

void test_build_envelope_command(void) {
    char *cmd = build_envelope_command("MAIL FROM", "me@boisestate.edu");
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("MAIL FROM:<me@boisestate.edu>\r\n", cmd);
    free(cmd);

    cmd = build_envelope_command("RCPT TO", "you@example.com");
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("RCPT TO:<you@example.com>\r\n", cmd);
    free(cmd);

    // The null reverse-path is legal.
    cmd = build_envelope_command("MAIL FROM", "");
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("MAIL FROM:<>\r\n", cmd);
    free(cmd);

    // An address longer than any fixed buffer is not truncated.
    char address[600];
    memset(address, 'a', sizeof(address) - 1);
    address[sizeof(address) - 1] = '\0';
    cmd = build_envelope_command("RCPT TO", address);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_size_t(strlen("RCPT TO:<>\r\n") + strlen(address), strlen(cmd));
    free(cmd);

    TEST_ASSERT_NULL(build_envelope_command(NULL, "me@boisestate.edu"));
    TEST_ASSERT_NULL(build_envelope_command("MAIL FROM", NULL));
    TEST_ASSERT_NULL(build_envelope_command("MAIL\nFROM", "me@boisestate.edu"));
    TEST_ASSERT_NULL(build_envelope_command("RCPT TO", "a@b.com>\r\nRCPT TO:<c@d.com"));
}

static void assert_stuffed(const char *expected, const char *body) {
    char *stuffed = dot_stuff(body);
    TEST_ASSERT_NOT_NULL(stuffed);
    TEST_ASSERT_EQUAL_STRING(expected, stuffed);
    free(stuffed);
}

void test_dot_stuff(void) {
    TEST_ASSERT_NULL(dot_stuff(NULL));
    assert_stuffed("", "");
    assert_stuffed("Hello", "Hello");
    assert_stuffed("a.b. c.", "a.b. c.");

    // Leading periods are doubled, on the first line and every later one.
    assert_stuffed("..Hello\r\nWorld", ".Hello\nWorld");
    assert_stuffed("Hello\r\n..World\r\n...", "Hello\n.World\n..");
    assert_stuffed("..\r\n", ".\n");
    assert_stuffed("\r\n..\r\n", "\n.\n");
    // A body containing the end-of-data sequence cannot end the message early.
    assert_stuffed("one\r\n..\r\ntwo\r\n", "one\r\n.\r\ntwo\r\n");

    // Every line ending goes out as CRLF.
    assert_stuffed("one\r\ntwo\r\n", "one\ntwo\n");
    assert_stuffed("one\r\ntwo\r\n", "one\r\ntwo\r\n");
    assert_stuffed("bare\r\ncr", "bare\rcr");
    assert_stuffed("\r\n\r\n", "\r\r\n");
    assert_stuffed("\r\n\r\n..x", "\n\n.x");
    assert_stuffed("..\r\n..", ".\r.");
}

void test_build_data_payload(void) {
    // A body piped in by echo ends with a bare LF.
    char *payload = build_data_payload("me@boisestate.edu", "you@example.com", "hello",
                                       "This is the message body.\n");
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_STRING("From: me@boisestate.edu\r\n"
                             "To: you@example.com\r\n"
                             "Subject: hello\r\n"
                             "\r\n"
                             "This is the message body.\r\n"
                             ".\r\n", payload);
    free(payload);

    payload = build_data_payload("me@example.com", "you@example.com", "Test", "Hello\n.World");
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_STRING("From: me@example.com\r\n"
                             "To: you@example.com\r\n"
                             "Subject: Test\r\n"
                             "\r\n"
                             "Hello\r\n"
                             "..World\r\n"
                             ".\r\n", payload);
    free(payload);

    // A body that already ends with CRLF gets no extra blank line.
    payload = build_data_payload("me@example.com", "you@example.com", "", "Ends with CRLF\r\n");
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_STRING("From: me@example.com\r\n"
                             "To: you@example.com\r\n"
                             "Subject: \r\n"
                             "\r\n"
                             "Ends with CRLF\r\n"
                             ".\r\n", payload);
    free(payload);

    // A NULL subject is the same as an empty one, a NULL body the same as "".
    const char *empty = "From: me@example.com\r\n"
                        "To: you@example.com\r\n"
                        "Subject: \r\n"
                        "\r\n"
                        ".\r\n";
    payload = build_data_payload("me@example.com", "you@example.com", NULL, NULL);
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_STRING(empty, payload);
    free(payload);

    payload = build_data_payload("me@example.com", "you@example.com", "", "");
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_STRING(empty, payload);
    free(payload);

    // A body that is only a period.
    payload = build_data_payload("a@b.c", "d@e.f", "s", ".");
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_STRING("From: a@b.c\r\nTo: d@e.f\r\nSubject: s\r\n\r\n..\r\n.\r\n", payload);
    free(payload);

    TEST_ASSERT_NULL(build_data_payload(NULL, "you@example.com", "s", "b"));
    TEST_ASSERT_NULL(build_data_payload("me@example.com", NULL, "s", "b"));
    TEST_ASSERT_NULL(build_data_payload("me@example.com\r\nBcc: x", "you@example.com", "s", "b"));
    TEST_ASSERT_NULL(build_data_payload("me@example.com", "you@example.com\n", "s", "b"));
    TEST_ASSERT_NULL(build_data_payload("me@example.com", "you@example.com", "hi\r\nBcc: x", "b"));
}

/* ------------------------------------------------------------------------
 * LAYER 2: The session
 * ------------------------------------------------------------------------ */

void test_transport_init(void) {
    mock_t m;
    transport_t t;
    t.len = 42;
    transport_init(&t, mock_read, mock_write, &m);
    TEST_ASSERT_TRUE(t.read == mock_read);
    TEST_ASSERT_TRUE(t.write == mock_write);
    TEST_ASSERT_TRUE(t.ctx == &m);
    TEST_ASSERT_EQUAL_size_t(0, t.len);

    transport_init(NULL, mock_read, mock_write, &m); // must not crash
}

void test_read_line(void) {
    mock_t m;
    transport_t t;
    char line[64];

    // CRLF and bare LF both end a line, and neither is returned.
    mock_start(&m, &t, "220 hello\r\n250 bare lf\n\r\n\n", 0);
    TEST_ASSERT_EQUAL_INT(9, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("220 hello", line);
    TEST_ASSERT_EQUAL_INT(11, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250 bare lf", line);
    TEST_ASSERT_EQUAL_INT(0, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("", line);
    TEST_ASSERT_EQUAL_INT(0, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("", line);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, read_line(&t, line, sizeof(line)));
}

void test_read_line_several_lines_in_one_read(void) {
    mock_t m;
    transport_t t;
    char line[64];

    mock_start(&m, &t, "250-one\r\n250-two\r\n250 three\r\n", 0);
    TEST_ASSERT_EQUAL_INT(7, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250-one", line);
    TEST_ASSERT_EQUAL_INT(7, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250-two", line);
    TEST_ASSERT_EQUAL_INT(9, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250 three", line);
    // The buffer is refilled only when it does not already hold a line.
    TEST_ASSERT_EQUAL_INT(1, m.reads);
}

void test_read_line_a_few_bytes_at_a_time(void) {
    for (size_t chunk = 1; chunk <= 12; chunk++) {
        mock_t m;
        transport_t t;
        char line[64];

        mock_start(&m, &t, "220 smtp.example.com ESMTP\r\n250 OK\r\n", chunk);
        TEST_ASSERT_EQUAL_INT(26, read_line(&t, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING("220 smtp.example.com ESMTP", line);
        TEST_ASSERT_EQUAL_INT(6, read_line(&t, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING("250 OK", line);
        TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, read_line(&t, line, sizeof(line)));
    }

    // A CR and its LF split across two reads are still one line ending.
    mock_t m;
    transport_t t;
    char line[64];
    mock_start(&m, &t, "250 OK\r\n", 7);
    TEST_ASSERT_EQUAL_INT(6, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250 OK", line);
}

void test_read_line_too_long_for_caller(void) {
    mock_t m;
    transport_t t;
    char line[8];

    // "250 OK1" needs 8 bytes with its NUL, "250 OK12" needs 9.
    mock_start(&m, &t, "250 OK1\r\n250 OK12\r\n", 0);
    TEST_ASSERT_EQUAL_INT(7, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250 OK1", line);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, read_line(&t, line, sizeof(line)));
}

void test_read_line_too_long_for_buffer(void) {
    // A line that just fits: SMTP_LINE_MAX bytes including its CRLF.
    char *script = malloc(2 * SMTP_LINE_MAX + 16);
    TEST_ASSERT_NOT_NULL(script);
    memset(script, 'a', SMTP_LINE_MAX - 2);
    strcpy(script + SMTP_LINE_MAX - 2, "\r\n");

    mock_t m;
    transport_t t;
    char line[2 * SMTP_LINE_MAX];
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(SMTP_LINE_MAX - 2, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_size_t(SMTP_LINE_MAX - 2, strlen(line));

    // One byte more and the buffer fills up before the line ends.
    memset(script, 'a', SMTP_LINE_MAX + 5);
    strcpy(script + SMTP_LINE_MAX + 5, "\r\n");
    mock_start(&m, &t, script, 100);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, read_line(&t, line, sizeof(line)));
    TEST_ASSERT_EQUAL_size_t(SMTP_LINE_MAX, t.len);

    free(script);
}

void test_read_line_errors(void) {
    mock_t m;
    transport_t t;
    char line[64];

    mock_start(&m, &t, "220 OK\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_line(NULL, line, sizeof(line)));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_line(&t, NULL, sizeof(line)));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_line(&t, line, 0));
    transport_init(&t, NULL, mock_write, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_line(&t, line, sizeof(line)));

    // The server hangs up before sending anything, or halfway through a line.
    mock_start(&m, &t, "", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, read_line(&t, line, sizeof(line)));
    mock_start(&m, &t, "220 no line ending", 4);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, read_line(&t, line, sizeof(line)));

    // The read callback fails, or claims more bytes than it was given room for.
    mock_start(&m, &t, "220 partial", 0);
    m.fail_at_end = true;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, read_line(&t, line, sizeof(line)));
    transport_init(&t, failing_read, mock_write, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, read_line(&t, line, sizeof(line)));
    transport_init(&t, oversized_read, mock_write, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, read_line(&t, line, sizeof(line)));
}

void test_read_full_reply_single_line(void) {
    mock_t m;
    transport_t t;
    char reply[256];

    mock_start(&m, &t, "220 smtp.example.com ESMTP ready\r\n", 0);
    TEST_ASSERT_EQUAL_INT(220, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("220 smtp.example.com ESMTP ready", reply);

    // A reply may be nothing but the code.
    mock_start(&m, &t, "250\r\n", 0);
    TEST_ASSERT_EQUAL_INT(250, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("250", reply);
}

void test_read_full_reply_multi_line(void) {
    const char *script = "250-smtp.example.com\r\n"
                         "250-PIPELINING\r\n"
                         "250 SIZE 10240000\r\n"
                         "221 Bye\r\n";

    // Delivered all at once and a few bytes at a time.
    for (size_t chunk = 0; chunk <= 8; chunk++) {
        mock_t m;
        transport_t t;
        char reply[256];

        mock_start(&m, &t, script, chunk);
        TEST_ASSERT_EQUAL_INT(250, read_full_reply(&t, reply, sizeof(reply)));
        TEST_ASSERT_EQUAL_STRING("250-smtp.example.com\n250-PIPELINING\n250 SIZE 10240000", reply);

        // The reply stops at its last line; the next reply is untouched.
        TEST_ASSERT_EQUAL_INT(221, read_full_reply(&t, reply, sizeof(reply)));
        TEST_ASSERT_EQUAL_STRING("221 Bye", reply);
    }
}

void test_read_full_reply_malformed(void) {
    mock_t m;
    transport_t t;
    char reply[256];

    // Not a reply at all; the text is kept so it can be reported.
    mock_start(&m, &t, "hello there\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_SYNTAX, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("hello there", reply);

    // A bad line in the middle of a multi-line reply.
    mock_start(&m, &t, "250-one\r\nbogus\r\n250 three\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_SYNTAX, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("250-one\nbogus", reply);

    // The lines of one reply disagree about the code.
    mock_start(&m, &t, "250-one\r\n554 two\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_SYNTAX, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("250-one\n554 two", reply);
}

void test_read_full_reply_too_long(void) {
    mock_t m;
    transport_t t;
    char reply[16];

    // "250-first\n250 end" is 17 bytes plus the NUL.
    const char *script = "250-first\r\n250 end\r\n";
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("250-first", reply);

    char exact[18];
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(250, read_full_reply(&t, exact, sizeof(exact)));
    TEST_ASSERT_EQUAL_STRING("250-first\n250 end", exact);

    char short_by_one[17];
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, read_full_reply(&t, short_by_one, sizeof(short_by_one)));

    // A single line that does not fit.
    char tiny[4];
    mock_start(&m, &t, "250 OK\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, read_full_reply(&t, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);

    // A line longer than the line reader's buffer.
    char *big = malloc(SMTP_LINE_MAX + 16);
    TEST_ASSERT_NOT_NULL(big);
    memcpy(big, "250 ", 4);
    memset(big + 4, 'x', SMTP_LINE_MAX);
    strcpy(big + 4 + SMTP_LINE_MAX, "\r\n");
    char large[4 * SMTP_LINE_MAX];
    mock_start(&m, &t, big, 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, read_full_reply(&t, large, sizeof(large)));
    free(big);
}

void test_read_full_reply_hang_up(void) {
    mock_t m;
    transport_t t;
    char reply[256];

    mock_start(&m, &t, "", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, read_full_reply(&t, reply, sizeof(reply)));

    // The server hangs up after a continuation line.
    mock_start(&m, &t, "250-smtp.example.com\r\n250-PIPE", 3);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, read_full_reply(&t, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("250-smtp.example.com", reply);

    mock_start(&m, &t, "250-smtp.example.com\r\n", 0);
    m.fail_at_end = true;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, read_full_reply(&t, reply, sizeof(reply)));
}

void test_read_full_reply_args(void) {
    mock_t m;
    transport_t t;
    char reply[16];

    mock_start(&m, &t, "250 OK\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_full_reply(NULL, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_full_reply(&t, NULL, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, read_full_reply(&t, reply, 0));
    TEST_ASSERT_EQUAL_INT(0, m.reads);
}

void test_write_all(void) {
    const char *data = "MAIL FROM:<me@boisestate.edu>\r\n";

    // Whole writes and short writes both deliver every byte.
    for (size_t chunk = 0; chunk <= 5; chunk++) {
        mock_t m;
        transport_t t;
        mock_start(&m, &t, "", 0);
        m.write_chunk = chunk;
        TEST_ASSERT_EQUAL_INT(0, write_all(&t, data, strlen(data)));
        TEST_ASSERT_EQUAL_STRING(data, m.sent);
    }

    mock_t m;
    transport_t t;
    mock_start(&m, &t, "", 0);
    m.write_chunk = 1;
    TEST_ASSERT_EQUAL_INT(0, write_all(&t, data, strlen(data)));
    TEST_ASSERT_EQUAL_INT((int)strlen(data), m.writes);

    // Nothing to write means no write call.
    mock_start(&m, &t, "", 0);
    TEST_ASSERT_EQUAL_INT(0, write_all(&t, data, 0));
    TEST_ASSERT_EQUAL_INT(0, m.writes);

    // A write that fails after a short write.
    mock_start(&m, &t, "", 0);
    m.write_chunk = 4;
    m.fail_write_at = 2;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, write_all(&t, data, strlen(data)));
    TEST_ASSERT_EQUAL_STRING("MAIL", m.sent);

    transport_init(&t, mock_read, failing_write, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, write_all(&t, data, strlen(data)));
    transport_init(&t, mock_read, zero_write, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, write_all(&t, data, strlen(data)));
    transport_init(&t, mock_read, oversized_write, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, write_all(&t, data, strlen(data)));

    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, write_all(NULL, data, strlen(data)));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, write_all(&t, NULL, 3));
    transport_init(&t, mock_read, NULL, &m);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, write_all(&t, data, strlen(data)));
}

void test_expect_reply(void) {
    mock_t m;
    transport_t t;
    char reply[256];

    mock_start(&m, &t, "220 ready\r\n", 0);
    TEST_ASSERT_EQUAL_INT(0, expect_reply(&t, 220, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("220 ready", reply);

    mock_start(&m, &t, "554-no\r\n554 go away\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_UNEXPECTED, expect_reply(&t, 220, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("554-no\n554 go away", reply);

    mock_start(&m, &t, "220 rea", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, expect_reply(&t, 220, reply, sizeof(reply)));

    mock_start(&m, &t, "junk\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_SYNTAX, expect_reply(&t, 220, reply, sizeof(reply)));

    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, expect_reply(NULL, 220, reply, sizeof(reply)));
}

void test_send_command(void) {
    mock_t m;
    transport_t t;
    char reply[256];

    mock_start(&m, &t, "250 smtp.example.com\r\n", 0);
    TEST_ASSERT_EQUAL_INT(0, send_command(&t, "HELO localhost\r\n", 250, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("HELO localhost\r\n", m.sent);
    TEST_ASSERT_EQUAL_STRING("250 smtp.example.com", reply);

    mock_start(&m, &t, "250 OK\r\n", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_UNEXPECTED, send_command(&t, "DATA\r\n", 354, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("250 OK", reply);

    mock_start(&m, &t, "", 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, send_command(&t, "QUIT\r\n", 221, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("QUIT\r\n", m.sent);

    // When the write fails, no reply is read.
    mock_start(&m, &t, "250 OK\r\n", 0);
    m.fail_write_at = 1;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, send_command(&t, "HELO localhost\r\n", 250, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(0, m.reads);

    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, send_command(&t, NULL, 250, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, send_command(NULL, "QUIT\r\n", 221, reply, sizeof(reply)));
}

static void assert_session_ok(const char *script, size_t read_chunk, size_t write_chunk) {
    mock_t m;
    transport_t t;
    char err[256];
    char transcript[2048];

    mock_start(&m, &t, script, read_chunk);
    m.write_chunk = write_chunk;
    TEST_ASSERT_EQUAL_INT(0, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", err);
    build_transcript(transcript, sizeof(transcript), STEPS);
    TEST_ASSERT_EQUAL_STRING(transcript, m.sent);
    TEST_ASSERT_EQUAL_size_t(m.len, m.pos); // every reply was consumed
}

void test_run_smtp_session_happy(void) {
    char script[2048];
    build_script(script, sizeof(script), -1, NULL);

    // The exact bytes the client sends, in order.
    mock_t m;
    transport_t t;
    char err[256];
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(0, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_STRING("HELO onyx.boisestate.edu\r\n"
                             "MAIL FROM:<me@boisestate.edu>\r\n"
                             "RCPT TO:<you@example.com>\r\n"
                             "DATA\r\n"
                             "From: me@boisestate.edu\r\n"
                             "To: you@example.com\r\n"
                             "Subject: hello\r\n"
                             "\r\n"
                             "This is the message body.\r\n"
                             ".\r\n"
                             "QUIT\r\n", m.sent);

    // The same session with replies and writes split into small pieces.
    for (size_t chunk = 1; chunk <= 7; chunk++) {
        assert_session_ok(script, chunk, 0);
        assert_session_ok(script, 0, chunk);
    }
}

void test_run_smtp_session_multi_line_replies(void) {
    const char *script = "220-smtp.example.com ESMTP ready\r\n"
                         "220 no UCE\r\n"
                         "250-smtp.example.com\r\n"
                         "250-PIPELINING\r\n"
                         "250 SIZE 10240000\r\n"
                         "250 2.1.0 Ok\r\n"
                         "250 2.1.5 Ok\r\n"
                         "354 End data with <CR><LF>.<CR><LF>\r\n"
                         "250-2.0.0 Ok\r\n"
                         "250 2.0.0 queued as 12345\r\n"
                         "221 2.0.0 Bye\r\n";
    assert_session_ok(script, 0, 0);
    assert_session_ok(script, 3, 0);
}

void test_run_smtp_session_dot_stuffing(void) {
    char script[2048];
    build_script(script, sizeof(script), -1, NULL);

    smtp_message_t msg = good_msg;
    msg.subject = NULL;
    msg.body = ".starts with a dot\n.\nlast line";

    mock_t m;
    transport_t t;
    char err[256];
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(0, run_smtp_session(&t, &msg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(m.sent,
                                "DATA\r\n"
                                "From: me@boisestate.edu\r\n"
                                "To: you@example.com\r\n"
                                "Subject: \r\n"
                                "\r\n"
                                "..starts with a dot\r\n"
                                "..\r\n"
                                "last line\r\n"
                                ".\r\n"
                                "QUIT\r\n"));
}

// Replaces the reply at each step with `reply` and checks that the client
// reports it and stops without sending anything more.
static void assert_each_step_rejects(const char *(*reply_for)(int step)) {
    for (int step = 0; step < STEPS; step++) {
        const char *reply = reply_for(step);
        char script[2048];
        char transcript[2048];
        char err[512];
        build_script(script, sizeof(script), step, reply);
        build_transcript(transcript, sizeof(transcript), step);

        mock_t m;
        transport_t t;
        mock_start(&m, &t, script, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, run_smtp_session(&t, &good_msg, err, sizeof(err)), step_names[step]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(transcript, m.sent, step_names[step]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, step_names[step]), err);

        // The message names the code that was required and quotes the reply,
        // whose lines the client reports without their CRLF.
        char quoted[128];
        size_t q = 0;
        for (const char *p = reply; *p && q < sizeof(quoted) - 1; p++) {
            if (*p != '\r') quoted[q++] = *p;
        }
        quoted[q - 1] = '\0'; // drop the final LF
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, quoted), err);
        char want[32];
        snprintf(want, sizeof(want), "expected %.3s", good_replies[step]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, want), err);
    }
}

static const char *permanent_failure(int step) {
    (void)step;
    return "554 5.7.1 Rejected\r\n";
}

static const char *transient_failure(int step) {
    (void)step;
    return "421 4.3.2 Service not available\r\n";
}

// A valid reply, but the one that belongs to some other step.
static const char *code_from_wrong_step(int step) {
    static const char *wrong[STEPS] = {
        "250 not a greeting\r\n",
        "220 not a HELO reply\r\n",
        "354 not a MAIL reply\r\n",
        "251 User not local; will forward\r\n",
        "250 not a DATA reply\r\n",
        "354 not a queued reply\r\n",
        "250 not a QUIT reply\r\n",
    };
    return wrong[step];
}

static const char *multi_line_failure(int step) {
    (void)step;
    return "550-5.1.1 Mailbox unavailable\r\n550 5.1.1 Try again later\r\n";
}

void test_run_smtp_session_wrong_codes(void) {
    assert_each_step_rejects(permanent_failure);
    assert_each_step_rejects(transient_failure);
    assert_each_step_rejects(code_from_wrong_step);
}

void test_run_smtp_session_multi_line_rejection(void) {
    assert_each_step_rejects(multi_line_failure);

    char script[2048];
    char err[512];
    build_script(script, sizeof(script), 3, multi_line_failure(3));
    mock_t m;
    transport_t t;
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("RCPT TO: expected 250 but the server replied: "
                             "550-5.1.1 Mailbox unavailable\n550 5.1.1 Try again later", err);
}

void test_run_smtp_session_hang_up(void) {
    for (int step = 0; step < STEPS; step++) {
        char transcript[2048];
        build_transcript(transcript, sizeof(transcript), step);

        // The server hangs up instead of replying, and halfway through a reply.
        for (int partial = 0; partial < 2; partial++) {
            char script[2048];
            char err[512];
            build_script(script, sizeof(script), step, NULL);
            if (partial) strncat(script, good_replies[step], 5);

            mock_t m;
            transport_t t;
            mock_start(&m, &t, script, 2);
            TEST_ASSERT_EQUAL_INT_MESSAGE(2, run_smtp_session(&t, &good_msg, err, sizeof(err)), step_names[step]);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(transcript, m.sent, step_names[step]);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, step_names[step]), err);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "closed the connection"), err);
        }
    }
}

void test_run_smtp_session_malformed_reply(void) {
    char script[2048];
    char err[512];
    mock_t m;
    transport_t t;

    build_script(script, sizeof(script), 0, "SSH-2.0-OpenSSH_9.6\r\n");
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("greeting: the server sent a malformed reply: SSH-2.0-OpenSSH_9.6", err);
    TEST_ASSERT_EQUAL_STRING("", m.sent);

    build_script(script, sizeof(script), 4, "354-go\r\n250 ahead\r\n");
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "DATA: the server sent a malformed reply"));
}

void test_run_smtp_session_reply_too_long(void) {
    char *script = malloc(SMTP_LINE_MAX + 16);
    TEST_ASSERT_NOT_NULL(script);
    memcpy(script, "220 ", 4);
    memset(script + 4, 'x', SMTP_LINE_MAX);
    strcpy(script + 4 + SMTP_LINE_MAX, "\r\n");

    mock_t m;
    transport_t t;
    char err[512];
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("greeting: the server's reply is too long", err);
    TEST_ASSERT_EQUAL_STRING("", m.sent);
    free(script);
}

void test_run_smtp_session_io_errors(void) {
    char script[2048];
    char err[512];
    mock_t m;
    transport_t t;

    // A read fails partway through the session.
    build_script(script, sizeof(script), 2, NULL);
    mock_start(&m, &t, script, 0);
    m.fail_at_end = true;
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("MAIL FROM: error reading from or writing to the connection", err);

    // A write fails: the RCPT TO command is the third write.
    build_script(script, sizeof(script), -1, NULL);
    mock_start(&m, &t, script, 0);
    m.fail_write_at = 3;
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("RCPT TO: error reading from or writing to the connection", err);
    build_transcript(script, sizeof(script), 2);
    TEST_ASSERT_EQUAL_STRING(script, m.sent);
}

void test_run_smtp_session_invalid_message(void) {
    char script[2048];
    char err[512];
    build_script(script, sizeof(script), -1, NULL);

    smtp_message_t bad[6];
    for (int i = 0; i < 6; i++) bad[i] = good_msg;
    bad[0].helo = NULL;
    bad[1].from = NULL;
    bad[2].to = "you@example.com\r\nRCPT TO:<other@example.com>";
    bad[3].subject = "hi\r\nBcc: other@example.com";
    bad[4].helo = "onyx\nRSET";
    bad[5].from = "me@boisestate.edu>\r\nRSET";

    // Nothing is read or written for a message that cannot be sent safely.
    for (int i = 0; i < 6; i++) {
        mock_t m;
        transport_t t;
        mock_start(&m, &t, script, 0);
        TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &bad[i], err, sizeof(err)));
        TEST_ASSERT_NOT_NULL(strstr(err, "invalid message"));
        TEST_ASSERT_EQUAL_INT(0, m.reads);
        TEST_ASSERT_EQUAL_INT(0, m.writes);
    }

    // A missing subject and body are fine.
    smtp_message_t minimal = good_msg;
    minimal.subject = NULL;
    minimal.body = NULL;
    mock_t m;
    transport_t t;
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(0, run_smtp_session(&t, &minimal, err, sizeof(err)));
}

void test_run_smtp_session_args(void) {
    char script[2048];
    char err[512];
    build_script(script, sizeof(script), -1, NULL);
    mock_t m;
    transport_t t;
    mock_start(&m, &t, script, 0);

    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(NULL, &good_msg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("no transport or message", err);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, NULL, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("no transport or message", err);

    // The error buffer is optional.
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(NULL, &good_msg, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, run_smtp_session(&t, &good_msg, err, 0));

    // A long reply is cut to fit the error buffer, which stays terminated.
    char small[16];
    build_script(script, sizeof(script), 0, "554 this reply is much longer than sixteen bytes\r\n");
    mock_start(&m, &t, script, 0);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("greeting: expec", small);
}

/* ------------------------------------------------------------------------
 * LAYER 3: The socket transport
 * ------------------------------------------------------------------------ */

// Opens a TCP socket on an unused loopback port and writes the port number
// into port. The socket listens only when asked to.
static int loopback_socket(char *port, size_t size, bool listening) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(s >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(s, (struct sockaddr *)&addr, sizeof(addr)));
    if (listening) TEST_ASSERT_EQUAL_INT(0, listen(s, 1));

    socklen_t len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(s, (struct sockaddr *)&addr, &len));
    snprintf(port, size, "%u", (unsigned)ntohs(addr.sin_port));
    return s;
}

void test_socket_read_write(void) {
    char port[16];
    char err[256];
    int listener = loopback_socket(port, sizeof(port), true);

    int client = socket_connect("127.0.0.1", port, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(client >= 0, err);
    int server = accept(listener, NULL, NULL);
    TEST_ASSERT_TRUE(server >= 0);

    char buf[64];
    TEST_ASSERT_EQUAL_INT(6, (int)socket_write(&client, "HELO\r\n", 6));
    TEST_ASSERT_EQUAL_INT(6, (int)socket_read(&server, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("HELO\r\n", buf, 6);

    TEST_ASSERT_EQUAL_INT(8, (int)socket_write(&server, "250 OK\r\n", 8));
    TEST_ASSERT_EQUAL_INT(8, (int)socket_read(&client, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("250 OK\r\n", buf, 8);

    // A hang-up reads as zero bytes.
    close(server);
    TEST_ASSERT_EQUAL_INT(0, (int)socket_read(&client, buf, sizeof(buf)));

    close(client);
    close(listener);

    // Both wrappers pass failures through.
    int bad = -1;
    TEST_ASSERT_EQUAL_INT(-1, (int)socket_read(&bad, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, (int)socket_write(&bad, "x", 1));
}

void test_socket_session(void) {
    char port[16];
    char err[256];
    int listener = loopback_socket(port, sizeof(port), true);

    int client = socket_connect("127.0.0.1", port, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(client >= 0, err);
    int server = accept(listener, NULL, NULL);
    TEST_ASSERT_TRUE(server >= 0);

    // The server queues all of its replies up front; the client reads them
    // one at a time, just as it would from a real server.
    char script[2048];
    build_script(script, sizeof(script), -1, NULL);
    TEST_ASSERT_EQUAL_INT((int)strlen(script), (int)write(server, script, strlen(script)));

    transport_t t;
    transport_init(&t, socket_read, socket_write, &client);
    TEST_ASSERT_EQUAL_INT(0, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    close(client);

    // Everything the client sent, read until it hung up.
    char sent[2048];
    size_t got = 0;
    ssize_t n;
    while ((n = read(server, sent + got, sizeof(sent) - 1 - got)) > 0) {
        got += (size_t)n;
    }
    sent[got] = '\0';
    char transcript[2048];
    build_transcript(transcript, sizeof(transcript), STEPS);
    TEST_ASSERT_EQUAL_STRING(transcript, sent);

    close(server);
    close(listener);
}

void test_socket_session_server_hangs_up(void) {
    char port[16];
    char err[256];
    int listener = loopback_socket(port, sizeof(port), true);

    int client = socket_connect("127.0.0.1", port, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(client >= 0, err);
    int server = accept(listener, NULL, NULL);
    TEST_ASSERT_TRUE(server >= 0);

    // The server greets, then goes away before the reply to HELO.
    size_t greeting_len = strlen(good_replies[0]);
    TEST_ASSERT_EQUAL_INT((int)greeting_len, (int)write(server, good_replies[0], greeting_len));
    close(server);

    transport_t t;
    transport_init(&t, socket_read, socket_write, &client);
    TEST_ASSERT_EQUAL_INT(2, run_smtp_session(&t, &good_msg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "HELO: "), err);

    close(client);
    close(listener);
}

void test_socket_connect_refused(void) {
    // Bound but not listening, so a connection attempt is refused.
    char port[16];
    char err[256];
    int s = loopback_socket(port, sizeof(port), false);

    TEST_ASSERT_EQUAL_INT(-1, socket_connect("127.0.0.1", port, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "cannot connect to 127.0.0.1 port "), err);
    TEST_ASSERT_EQUAL_INT(-1, socket_connect("127.0.0.1", port, NULL, 0));
    close(s);
}

void test_socket_connect_bad_address(void) {
    char err[256];
    TEST_ASSERT_EQUAL_INT(-1, socket_connect("127.0.0.1", "no-such-service", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "cannot resolve 127.0.0.1 port no-such-service"), err);

    TEST_ASSERT_EQUAL_INT(-1, socket_connect(NULL, "25", err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("no server or port given", err);
    TEST_ASSERT_EQUAL_INT(-1, socket_connect("127.0.0.1", NULL, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("no server or port given", err);
}

int main(void) {
    // A write to a socket the peer closed must fail, not kill the tests.
    signal(SIGPIPE, SIG_IGN);

    UNITY_BEGIN();
    RUN_TEST(test_has_crlf);
    RUN_TEST(test_parse_reply_code);
    RUN_TEST(test_is_final_reply_line);
    RUN_TEST(test_build_command);
    RUN_TEST(test_build_envelope_command);
    RUN_TEST(test_dot_stuff);
    RUN_TEST(test_build_data_payload);
    RUN_TEST(test_transport_init);
    RUN_TEST(test_read_line);
    RUN_TEST(test_read_line_several_lines_in_one_read);
    RUN_TEST(test_read_line_a_few_bytes_at_a_time);
    RUN_TEST(test_read_line_too_long_for_caller);
    RUN_TEST(test_read_line_too_long_for_buffer);
    RUN_TEST(test_read_line_errors);
    RUN_TEST(test_read_full_reply_single_line);
    RUN_TEST(test_read_full_reply_multi_line);
    RUN_TEST(test_read_full_reply_malformed);
    RUN_TEST(test_read_full_reply_too_long);
    RUN_TEST(test_read_full_reply_hang_up);
    RUN_TEST(test_read_full_reply_args);
    RUN_TEST(test_write_all);
    RUN_TEST(test_expect_reply);
    RUN_TEST(test_send_command);
    RUN_TEST(test_run_smtp_session_happy);
    RUN_TEST(test_run_smtp_session_multi_line_replies);
    RUN_TEST(test_run_smtp_session_dot_stuffing);
    RUN_TEST(test_run_smtp_session_wrong_codes);
    RUN_TEST(test_run_smtp_session_multi_line_rejection);
    RUN_TEST(test_run_smtp_session_hang_up);
    RUN_TEST(test_run_smtp_session_malformed_reply);
    RUN_TEST(test_run_smtp_session_reply_too_long);
    RUN_TEST(test_run_smtp_session_io_errors);
    RUN_TEST(test_run_smtp_session_invalid_message);
    RUN_TEST(test_run_smtp_session_args);
    RUN_TEST(test_socket_read_write);
    RUN_TEST(test_socket_session);
    RUN_TEST(test_socket_session_server_hangs_up);
    RUN_TEST(test_socket_connect_refused);
    RUN_TEST(test_socket_connect_bad_address);
    return UNITY_END();
}
