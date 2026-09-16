#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

static const char *usage_text =
    "Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
    "          [-H helo-host] <server>\n"
    "\n"
    "  -f <from>       envelope sender, for example you@example.com\n"
    "  -t <to>         envelope recipient\n"
    "  -s <subject>    subject line (default: empty)\n"
    "  -b <body>       message body (default: read from stdin)\n"
    "  -p <port>       port or service name (default: 25)\n"
    "  -H <helo-host>  host name sent with HELO (default: localhost)\n"
    "  <server>        host name or address of the mail server\n";

// Reads all of stdin into a malloc'd string. Returns NULL on failure.
static char *read_stdin(void) {
    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        if (cap - len < 256) {
            cap *= 2;
            char *bigger = realloc(buf, cap);
            if (!bigger) {
                free(buf);
                return NULL;
            }
            buf = bigger;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, stdin);
        len += n;
        if (n == 0) break;
    }
    if (ferror(stdin)) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        fputs(usage_text, stdout);
        return 0;
    }

    const char *from = NULL;
    const char *to = NULL;
    const char *subject = "";
    const char *body = NULL;
    const char *port = "25";
    const char *helo_host = "localhost";

    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:")) != -1) {
        switch (opt) {
            case 'f': from = optarg; break;
            case 't': to = optarg; break;
            case 's': subject = optarg; break;
            case 'b': body = optarg; break;
            case 'p': port = optarg; break;
            case 'H': helo_host = optarg; break;
            default:
                fputs(usage_text, stderr);
                return 1;
        }
    }

    if (!from || !to) {
        fprintf(stderr, "myapp: -f <from> and -t <to> are required\n");
        fputs(usage_text, stderr);
        return 1;
    }
    if (optind != argc - 1) {
        fprintf(stderr, "myapp: expected exactly one <server>\n");
        fputs(usage_text, stderr);
        return 1;
    }
    const char *server = argv[optind];

    // A CR or LF here would let the caller inject SMTP commands or headers.
    if (has_crlf(from) || has_crlf(to) || has_crlf(subject) || has_crlf(helo_host)) {
        fprintf(stderr, "myapp: addresses, subject and HELO host must not contain CR or LF\n");
        return 1;
    }

    char *stdin_body = NULL;
    if (!body) {
        stdin_body = read_stdin();
        if (!stdin_body) {
            fprintf(stderr, "myapp: failed to read the message body from stdin\n");
            return 1;
        }
        body = stdin_body;
    }

    // Writing to a socket the server has closed must not kill the process.
    signal(SIGPIPE, SIG_IGN);

    char err[512];
    int fd = socket_connect(server, port, err, sizeof(err));
    if (fd < 0) {
        fprintf(stderr, "myapp: %s\n", err);
        free(stdin_body);
        return 2;
    }

    transport_t transport;
    transport_init(&transport, socket_read, socket_write, &fd);

    smtp_message_t msg = {
        .helo = helo_host,
        .from = from,
        .to = to,
        .subject = subject,
        .body = body,
    };
    int result = run_smtp_session(&transport, &msg, err, sizeof(err));
    if (result != 0) {
        fprintf(stderr, "myapp: %s\n", err);
    } else {
        printf("Message queued for delivery to %s\n", to);
    }

    close(fd);
    free(stdin_body);
    return result;
}
