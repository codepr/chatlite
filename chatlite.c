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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ADDR "127.0.0.1"
#define PORT 6699
#define BACKLOG 128
#define MAX_EVENTS 64
#define MAX_CLIENTS 1024
#define NICK_MAXLEN 32

// Return codes
#define CL_OK 0
#define CL_ERR -1

// Debug logging
#define CL_LOG(fmt, ...)                                                       \
    do {                                                                       \
        char timestamp_str[64] = {0};                                          \
        time_t t = time(NULL);                                                 \
        struct tm *tmp = localtime(&t);                                        \
        strftime(timestamp_str, sizeof(timestamp_str), "%T", tmp);             \
        stderr_printf("[%s] " fmt, timestamp_str, __VA_ARGS__);                \
    } while (0)

void stderr_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/*
 * Let's use a global event based IO syscall
 */
struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];

/*
 * Simple client state, currently contains only the file descriptor and
 * the nickname set in the chat
 */
typedef struct {
    int fd;
    char nick[NICK_MAXLEN];
} Client;

/*
 * A basic server state
 *  - fd the file descriptor it listens on
 *  - clients an array of file descriptors representing client connections
 */
typedef struct {
    int fd;
    Client *clients[MAX_CLIENTS];
} Server;

/*
 * Basic utility functions, e.g. memory management, allocator functions
 */

void *cl_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        perror("Out of memory");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

char *trim_string(char *str) {
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str))
        str++;

    if (*str == 0) // All spaces?
        return str;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    // Write new null terminator character
    end[1] = '\0';

    return str;
}

void generate_random_token(char token[16]) {
    int urandom = open("/dev/urandom", O_RDONLY);
    if (urandom < 0) {
        perror("urandom");
        // Panic for now
        exit(EXIT_FAILURE);
    }
    char random_data[16];
    ssize_t result = read(urandom, random_data, sizeof(random_data));
    if (result < 0) {
        perror("reading urandom");
        // Panic for now`
        exit(EXIT_FAILURE);
    }
    token[15] = 0;
    for (int i = 0; i < 8; i++)
        sprintf(&token[2 * i], "%02X", random_data[i]);
}

/*
 * =====================================================
 *                 NETWORKING HELPERS
 * =====================================================
 *
 * This server will adopt a simple text protocol TCP-based; to manage
 * the communication, we're defining a couple of useful helpers to
 * create a connection (TCP stream) and to read and write data from a
 * connected stream.
 *
 * - listen;
 * - accept;
 * - set_nonblocking; useful to leverage EPOLL (or SELECT/POLL, KQUEUE
 *   on BSD-like systems) and perform non-blocking reads/writes
 * - read
 * - write
 */

/* Set non-blocking socket */
static int set_nonblocking(int fd) {
    int flags, result;
    flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1)
        goto err;

    result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (result == -1)
        goto err;

    return CL_OK;

err:

    fprintf(stderr, "set_nonblocking: %s\n", strerror(errno));
    return CL_ERR;
}

static int cl_listen(Server *server, const char *host, int port, int backlog) {

    int listen_fd = -1;
    const struct addrinfo hints = {.ai_family = AF_UNSPEC,
                                   .ai_socktype = SOCK_STREAM,
                                   .ai_flags = AI_PASSIVE};
    struct addrinfo *result, *rp;
    char port_str[6];

    snprintf(port_str, 6, "%i", port);

    if (getaddrinfo(host, port_str, &hints, &result) != 0)
        goto err;

    /* Create a listening socket */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd < 0)
            continue;

        /* set SO_REUSEADDR so the socket will be reusable after process kill */
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1},
                       sizeof(int)) < 0)
            goto err;

        /* Bind it to the addr:port opened on the network interface */
        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break; // Succesful bind
        close(listen_fd);
    }

    freeaddrinfo(result);
    if (rp == NULL)
        goto err;

    /*
     * Let's make the socket non-blocking (strongly advised to use the
     * eventloop)
     */
    (void)set_nonblocking(listen_fd);

    /* Finally let's make it listen */
    if (listen(listen_fd, backlog) != 0)
        goto err;

    server->fd = listen_fd;

    return CL_OK;
err:
    return CL_ERR;
}

static int cl_accept(Server *server) {
    int fd;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    /* Let's accept on listening socket */
    fd = accept(server->fd, (struct sockaddr *)&addr, &addrlen);
    if (fd <= 0)
        goto exit;

    (void)set_nonblocking(fd);
    return fd;
exit:
    if (errno != EWOULDBLOCK && errno != EAGAIN)
        perror("accept");
    return CL_ERR;
}

/**
 * Simple broadcast function, for now we just assume all non connected FDs
 * are set to 0 as per initialization of the server struct in the main
 * function.
 */
void broadcast_message(Server *server, const char *buf, int fd,
                       int server_info) {
    int nwrite = 0;
    Client *sender = server->clients[fd];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = server->clients[i];
        if (c == NULL || i == fd)
            continue;
        CL_LOG("Broadcasting to %s\n", c->nick);
        char msg[256];
        int msglen = 0;
        if (!server_info)
            msglen = snprintf(msg, sizeof(msg), "%s\r\n%s", sender->nick, buf);
        else
            msglen = snprintf(msg, sizeof(msg), "Server\r\n%s", buf);
        nwrite = write(c->fd, msg, msglen);
        if (nwrite < 0)
            perror("write(3)");
    }
}

int main(void) {

    CL_LOG("Server init on %s:%d\n\n", ADDR, PORT);

    char token[16] = {0};
    char buf[256] = {0};
    generate_random_token(token);

    CL_LOG("Token: %s\n", token);

    Server server = {.fd = 0, .clients = {NULL}};

    // Make the server listen unblocking
    if (cl_listen(&server, ADDR, PORT, BACKLOG) == -1) {
        fprintf(stderr, "Error listening on %s:%i\n", ADDR, PORT);
        return CL_ERR;
    }

    int nfds = 0;
    int epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create1");
        return CL_ERR;
    }

    // Register the server listening socket into the epoll loop
    ev.events = EPOLLIN;
    ev.data.fd = server.fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, server.fd, &ev) == -1) {
        perror("epoll_ctl: server fd");
        return CL_ERR;
    }

    // Start the event loop
    for (;;) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            return CL_ERR;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server.fd) {
                int client_fd = cl_accept(&server);
                if (client_fd == -1) {
                    perror("accept");
                    return CL_ERR;
                }
                (void)set_nonblocking(client_fd);

                // Let's make a client here
                Client *c = cl_malloc(sizeof(Client));
                c->fd = client_fd;
                snprintf(c->nick, sizeof(c->nick), "anon:%d", client_fd);
                server.clients[client_fd] = c;

                CL_LOG("New user %s connected\n", c->nick);

                // Let's send a welcome message
                int buflen = snprintf(
                    buf, sizeof(buf),
                    "Server\r\nWelcome %s! Use /nick to set a nickname\n\n",
                    c->nick);
                int nwrite = write(c->fd, buf, buflen);
                if (nwrite < 0)
                    perror("write welcome message");

                // Let's broadcast the new joiner
                memset(buf, 0x00, sizeof(buf));
                size_t maxlen = strlen(c->nick) + 9;
                snprintf(buf, maxlen, "%s joined\n", c->nick);
                broadcast_message(&server, buf, client_fd, 1);

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                    perror("epoll_ctl: client fd");
                    return CL_ERR;
                }
            } else {
                ssize_t nread = read(events[i].data.fd, buf, sizeof(buf) - 1);
                if (nread < 0) {
                    CL_LOG("Client disconnected fd=%i\n", events[i].data.fd);
                    close(events[i].data.fd);
                    free(server.clients[events[i].data.fd]);
                } else {
                    buf[nread] = 0;
                    if (strncmp(buf, "/quit", 5) == 0) {
                        // Client wants to disconnect here
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd,
                                      NULL) < 0)
                            perror("disconnecting client");
                        Client *c = server.clients[events[i].data.fd];
                        CL_LOG("User %s disconnected\n", c->nick);
                        // Let's broadcast the user leaving
                        memset(buf, 0x00, sizeof(buf));
                        size_t maxlen = strlen(c->nick) + 7;
                        snprintf(buf, maxlen, "%s left\n", c->nick);
                        broadcast_message(&server, buf, c->fd, 1);
                        close(events[i].data.fd);
                        free(server.clients[events[i].data.fd]);
                        server.clients[events[i].data.fd] = NULL;
                    } else if (strncmp(buf, "/nick", 5) == 0) {
                        Client *c = server.clients[events[i].data.fd];
                        char raw_nick[NICK_MAXLEN];
                        strncpy(raw_nick, buf + 5, nread);
                        char *nick = trim_string(raw_nick);
                        CL_LOG("User %s updating nick to %s\n", c->nick, nick);
                        strncpy(c->nick, nick, strlen(raw_nick));
                    } else {
                        Client *c = server.clients[events[i].data.fd];
                        CL_LOG("User: %s len: %li msg: %s", c->nick, nread,
                               buf);
                        broadcast_message(&server, buf, events[i].data.fd, 0);
                    }
                }
            }
        }
    }

    return 0;
}
