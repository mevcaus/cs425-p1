#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
