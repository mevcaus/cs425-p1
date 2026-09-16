# cs425-p1 - Simple Mail Client

- Name: Mevludin Causevic
- Email: mevludincausevic@boisestate.edu
- Class: CS425

## Known Bugs or Issues

None known. Some deliberate limits:

- Only plain SMTP is spoken: `HELO`, no ESMTP extensions, no TLS and no
  authentication, as the assignment requires.
- A reply line longer than `SMTP_LINE_MAX` (1024 bytes, twice the RFC 5321
  limit) or a whole reply longer than 4096 bytes is reported as an error
  instead of being truncated.
- Only the codes the assignment lists are accepted (220, 250, 250, 250, 354,
  250, 221), so a `251 User not local; will forward` reply to `RCPT TO` is
  treated as a failure.

## Experience

I learned how to implement an application layer protocol (SMTP) from scratch
by writing an SMTP client in C. Handling the CRLF line endings, dot-stuffing
correctly, and parsing continuations were some of the key lessons learned.

## Usage

```bash
make all
echo "This is the message body." | \
  ./build/release/myapp -f me@boisestate.edu -t you@example.com \
    -s "hello" -H onyx.boisestate.edu -p 2525 <server>
```

The exit status is 0 when the server queues the message, 1 when the command
line is wrong, and 2 when the connection or the SMTP session fails. Every
failure prints the step that failed and the reply the server actually sent,
for example:

```text
myapp: RCPT TO: expected 250 but the server replied: 550 5.1.1 <nobody@example.com>: Recipient address rejected
```

## Design

You cannot unit test against a live mail server, so `src/lab.h` splits the
client into three layers, and only the bottom one touches a socket.

1. **Pure protocol helpers.** Functions that take strings and return strings
   or codes, with no I/O: `parse_reply_code`, `is_final_reply_line`,
   `build_command`, `build_envelope_command`, `dot_stuff`,
   `build_data_payload` and `has_crlf`. This is where the details that are
   easy to get wrong live: every line goes out with CRLF (a body piped in by
   `echo` has bare LFs, which are converted), every body line that starts
   with a period gets a second one (RFC 5321 section 4.5.2), the message
   always ends with `CRLF.CRLF`, and any value containing a CR or LF is
   refused so it cannot inject a command or a header.

2. **The session, over a swappable transport.** `transport_t` holds a
   `read` and a `write` callback (with the same meaning as `recv` and
   `send`), a context pointer, and the line reader's buffer. `read_line`
   refills that buffer only when it does not already hold a complete line,
   so a reply that arrives a few bytes at a time and several replies that
   arrive in one read are handled the same way. `read_full_reply` follows
   `250-` continuation lines to the final `250 ` line. `write_all` retries
   short writes. `expect_reply` and `send_command` check one step's code,
   and `run_smtp_session` runs the whole sequence, stopping at the first
   failure without sending anything more. Nothing in this layer calls
   `recv` or `send`.

3. **The socket transport.** `socket_connect` resolves the server with
   `getaddrinfo` and tries each address, while `socket_read` and
   `socket_write` are one-line wrappers over `recv` and `send` that match
   the two callbacks.

The payoff is layer 2. `main.c` plugs a socket into it, while the unit tests
plug in a scripted in-memory server: a string of replies, handed out whole or
a few bytes at a time, plus a buffer that records everything the client sent.
That makes every error path easy to drive with no network at all. A wrong
status code is the good script with one reply changed, and a server that
hangs up is the script cut short. The tests check the exact bytes the client
sent and that it stopped at the right step. Layer 3 is tested over loopback
sockets the tests open themselves, so `make check` needs no outside network
either.

## Testing

```bash
make clean && make all
make check       # Unity tests
make report      # coverage, 100% of lines and branches in src/lab.c
make leak        # ASan on the no-argument path
make leak-test   # ASan on the whole test suite
```

The only coverage exclusions are the failure branches of `malloc`,
`snprintf` and `socket`, which cannot be made to fail from a test.
