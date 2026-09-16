#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "harness/unity.h"
#include "../src/lab.h"

void setUp(void) {}
void tearDown(void) {}

void test_get_greeting(void) {
  char *greeting = get_greeting("Alice");
  TEST_ASSERT_NOT_NULL(greeting);
  TEST_ASSERT_EQUAL_STRING("Hello, Alice!", greeting);
  free(greeting); // Free the allocated memory for the greeting

  greeting = get_greeting(NULL);
  TEST_ASSERT_NULL(greeting);

  greeting = get_greeting("");
  TEST_ASSERT_NOT_NULL(greeting);
  TEST_ASSERT_EQUAL_STRING("Hello, !", greeting);
  free(greeting);
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_greeting);
    RUN_TEST(test_has_crlf);
    RUN_TEST(test_parse_reply_code);
    RUN_TEST(test_is_final_reply_line);
    RUN_TEST(test_build_command);
    RUN_TEST(test_build_envelope_command);
    RUN_TEST(test_dot_stuff);
    RUN_TEST(test_build_data_payload);
    return UNITY_END();
}
