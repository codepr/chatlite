/* MIT License
 *
 * Copyright (c) 2023 Andrea Baldan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*
 * Static flags to manage the terminal mode
 */
static bool rawmode_is_set = false;
static bool rawmode_atexit_is_registered = false;

/*
 * Creates a socket connection to the specified host:port
 */
int socket_connection(const char *host, int port) {

    struct sockaddr_in serveraddr;
    struct hostent *server;

    // socket: create the socket
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0)
        goto err;

    // gethostbyname: get the server's DNS entry
    server = gethostbyname(host);
    if (server == NULL)
        goto err;

    // build the server's address
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr,
          server->h_length);
    serveraddr.sin_port = htons(port);

    // connect: create a connection with the server
    if (connect(sfd, (const struct sockaddr *)&serveraddr, sizeof(serveraddr)) <
        0)
        goto err;

    return sfd;

err:

    perror("socket(2) opening socket failed");
    return -1;
}

/*
 * ===============================================
 *               TERMINAL MANAGING
 * ===============================================
 *
 * To provide a basic UX we're gonna disable the 'cooked mode'
 * of the terminal and play with the VT100 escape codes to provide
 * a degree of control on the input and output behaviour of the
 * client interface.
 */

// Save original terminal status here.
static struct termios orig_termios;

void tty_raw_mode_disable_atexit(void);

int tty_raw_mode_disable(int fd) {
    if (!rawmode_is_set)
        return 0;

    int err = tcsetattr(fd, TCSAFLUSH, &orig_termios);
    if (err < 0)
        return err;
    rawmode_is_set = false;
    return 0;
}

int tty_raw_mode_enable(int fd) {

    struct termios raw;

    // Enable raw mode.
    if (!isatty(fd))
        goto fatal;
    if (!rawmode_atexit_is_registered) {
        atexit(tty_raw_mode_disable_atexit);
        rawmode_atexit_is_registered = true;
    }
    if (tcgetattr(fd, &orig_termios) == -1)
        goto fatal;

    // modify the original mode
    raw = orig_termios;
    // input modes: no break, no CR to NL, no parity check, no strip char,
    // no start/stop output control.
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // output modes - do nothing. We want post processing enabled so that
    // \n will be automatically translated to \r\n.

    // control modes - set 8 bit chars
    raw.c_cflag |= (CS8);
    // local modes - choing off, canonical off, no extended functions,
    // but take signal chars (^Z,^C) enabled.
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    // control chars - set return condition: min number of bytes and timer.
    // We want read to return every single byte, without timeout.
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0; // 1 byte, no timer

    // put terminal in raw mode after flushing
    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
        goto fatal;
    rawmode_is_set = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

// At exit we'll try to fix the terminal to the initial conditions
void tty_raw_mode_disable_atexit(void) { tty_raw_mode_disable(STDIN_FILENO); }

void pty_clear_current_line(void) { write(STDOUT_FILENO, "\e[2K", 4); }

void pty_cursor_at_line_start(void) {
    write(STDOUT_FILENO, "\r\x1b[1m>\x1b[0m ", 11);
}

#define BUFSIZE 1024
#define IB_OK 0
#define IB_ERR -1
#define IB_NEWLINE 2

struct buffer {
    char buf[BUFSIZE];
    int len;
};

int buffer_append(struct buffer *ib, char c) {
    if (ib->len >= BUFSIZE)
        return IB_ERR;

    ib->buf[ib->len++] = c;
    return IB_OK;
}

void buffer_hide(const struct buffer *ib);
void buffer_show(const struct buffer *ib);

/*
 * Process every new keystroke arriving from the keyboard. As a side effect
 * the input buffer state is modified in order to reflect the current line
 * the user is typing, so that reading the input buffer 'buf' for 'len'
 * bytes will contain it.
 */
int buffer_feed_char(struct buffer *ib, int c) {
    switch (c) {
    case '\n':
        break; // Ignored. We handle \r instead.
    case '\r':
        return IB_NEWLINE;
    case 127: // Backspace.
        if (ib->len > 0) {
            ib->len--;
            buffer_hide(ib);
            buffer_show(ib);
        }
        break;
    default:
        if (buffer_append(ib, c) == 0)
            write(STDOUT_FILENO, ib->buf + ib->len - 1, 1);
        break;
    }
    return IB_OK;
}

int buffer_append_string(struct buffer *b, const char *src, size_t len) {
    assert(b->len + len <= BUFSIZE);
    memcpy(b->buf, src, len);
    b->len += len;
    return 0;
}

// Hide the line the user is typing
void buffer_hide(const struct buffer *ib) {
    (void)ib; // Not used var, but is conceptually part of the API.
    pty_clear_current_line();
    pty_cursor_at_line_start();
}

// Show again the current line
void buffer_show(const struct buffer *ib) {
    write(STDOUT_FILENO, ib->buf, ib->len);
    fflush(stdout);
}

// Reset the buffer to be empty
void buffer_clear(struct buffer *ib) {
    memset(ib->buf, 0x00, ib->len);
    ib->len = 0;
    buffer_hide(ib);
}

void buffer_dump(struct buffer *b) {
    buffer_show(b);
    buffer_clear(b);
}

size_t buffer_fmt(const struct buffer *ib, char *dst) {
    char ts_str[64] = {0};
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    size_t ts_len = strftime(ts_str, sizeof(ts_str), "%T", tmp);
    // 15 is the number of additional format chars we're adding + 1 for the nul
    // character
    size_t maxlen = strlen(ib->buf) + ts_len + 16;
    return snprintf(dst, maxlen, "\x1b[1m[%s you]:\x1b[m %s", ts_str, ib->buf);
}

/*
 * Very simple structure wrapping messages coming from the server
 */

struct message {
    char nick[32];
    char content[256];
};

// Parse the buffer content coming from the server, populating a
// struct message.
// The protocol couldn't be simpler, just looking for a \r\n (assuming
// every buffer contains already a complete message) to extract the
// nick then the message:
//
// <nick>\r\n<message>\r\n
int message_parse(const char *buf, struct message *msg, size_t len) {
    memset(msg->nick, 0x00, sizeof(msg->nick));
    memset(msg->content, 0x00, sizeof(msg->content));
    int i = 0;
    while (len--) {
        char c = buf[i];
        if (c == '\r')
            break;
        msg->nick[i++] = c;
    }
    msg->nick[i + 1] = '\0';

    len -= 2;
    i += 2;
    memcpy(msg->content, buf + i, len);

    return 0;
}

// Format a message to be correctly printed in the terminal
// interface
size_t message_fmt(const struct message *m, char *buf) {
    char ts_str[64] = {0};
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    size_t ts_len = strftime(ts_str, sizeof(ts_str), "%T", tmp);
    // 13 is the number of additional format chars we're adding + 1 for the nul
    // character
    size_t maxlen = strlen(m->nick) + strlen(m->content) + ts_len + 14;
    return snprintf(buf, maxlen, "\x1b[1m[%s %s]:\x1b[m %s\n", ts_str, m->nick,
                    m->content);
}

int pty_get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;

    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

// Reset position to the bottom left
int pty_reset_cursor_position(struct buffer *b) {
    int r, c;
    int err = pty_get_window_size(&r, &c);
    if (err < 0)
        return err;

    char esc[16];
    int len = snprintf(esc, sizeof(esc), "\x1b[%d;0H >> ", r);
    return buffer_append_string(b, esc, len);
}

int pty_draw_status_bar(struct buffer *b) {
    int err, r, c;
    err = pty_get_window_size(&r, &c);
    if (err < 0)
        return err;
    // This switches to inverted colors.
    // NOTE:
    // The m command (Select Graphic Rendition) causes the text printed
    // after it to be printed with various possible attributes including
    // bold (1), underscore (4), blink (5), and inverted colors (7). An
    // argument of 0 clears all attributes (the default one). See
    // http://vt100.net/docs/vt100-ug/chapter3.html#SGR for more info.
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "\x1b[0;0H\x1b[7m");
    // 7 is just the dumb half of the header `chatlite client`
    int mid = (c / 2) - 7;
    int i = 0, offset = 0;
    while (i < mid) {
        buf[n + i] = ' ';
        i++;
    }
    offset = n + mid;
    memcpy(buf + offset, "\x1b[1mchatlite client", 19);
    i = 0;
    // 19 is just strlen("\x1b[1mchatlite client")
    offset += 19;
    while (i < mid) {
        buf[offset + i] = ' ';
        i++;
    }
    offset += mid;
    memcpy(buf + offset, "\x1b[m\r\n", 5);
    int len = strlen(buf);
    return buffer_append_string(b, buf, len);
}

int pty_refresh(struct buffer *b) {
    buffer_clear(b);
    int err = pty_draw_status_bar(b);
    if (err < 0)
        return err;
    buffer_dump(b);
    err = pty_reset_cursor_position(b);
    if (err < 0)
        return err;
    buffer_dump(b);
    /* buffer_show(b); */
    return 0;
}

int pty_print_message(const struct message *m) {
    char output[BUFSIZE] = {0};
    pty_clear_current_line();
    write(STDOUT_FILENO, "\r", 1);
    int n = message_fmt(m, output);
    return write(STDOUT_FILENO, output, n);
}

int pty_print_buffer(const struct buffer *b) {
    char output[BUFSIZE] = {0};
    pty_clear_current_line();
    write(STDOUT_FILENO, "\r", 1);
    int n = buffer_fmt(b, output);
    return write(STDOUT_FILENO, output, n);
}

void pty_clear_screen(void) { write(STDOUT_FILENO, "\x1b[2J", 4); }

int main(void) {
    int err = tty_raw_mode_enable(STDIN_FILENO);
    if (err < 0)
        exit(EXIT_FAILURE);

    struct buffer ib;
    buffer_clear(&ib);

    pty_clear_screen();

    pty_refresh(&ib);

    int s = socket_connection("localhost", 6699);
    if (s < 0)
        exit(EXIT_FAILURE);

    fd_set readfds;

    while (1) {

        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int maxfd = s > STDIN_FILENO ? s : STDIN_FILENO;

        int num_events = select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if (num_events == -1) {
            perror("select() error");
            exit(EXIT_FAILURE);
        }

        char buf[BUFSIZE];
        memset(buf, 0x00, sizeof(buf));

        if (FD_ISSET(s, &readfds)) {
            // Data from the server
            ssize_t count = read(s, buf, sizeof(buf));
            if (count <= 0) {
                printf("Connection lost\n");
                exit(1);
            }
            struct message m;
            message_parse(buf, &m, count);
            pty_print_message(&m);
            pty_refresh(&ib);
        } else if (FD_ISSET(STDIN_FILENO, &readfds)) {
            // Data from the user typing on the terminal
            ssize_t count = read(STDIN_FILENO, buf, sizeof(buf));

            for (int j = 0; j < count; j++) {
                // Let's disable the up arrow
                if (buf[j] == '\x1b' && count - j >= 3) {
                    if (buf[j + 1] == '[' && buf[j + 2] == 'A') {
                        j += 2;
                        continue;
                    }
                }

                int res = buffer_feed_char(&ib, buf[j]);
                switch (res) {
                case IB_NEWLINE:
                    buffer_append(&ib, '\n');
                    buffer_hide(&ib);
                    pty_print_buffer(&ib);
                    // TODO print diagnostic on the term
                    (void)write(s, ib.buf, ib.len);
                    buffer_clear(&ib);
                    pty_refresh(&ib);
                    break;
                case IB_OK:
                    break;
                }
            }
        }
    }
    return 0;
}
