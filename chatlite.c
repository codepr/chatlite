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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define ADDR "127.0.0.1"
#define PORT 6699
#define BACKLOG 128
#define MAX_EVENTS 64
#define MAX_CLIENTS 1024
#define NICK_MAX_LENGTH 32

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
    char nick[NICK_MAX_LENGTH];
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

    return 0;

err:

    fprintf(stderr, "set_nonblocking: %s\n", strerror(errno));
    return -1;
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

    return 0;
err:
    return -1;
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
    return -1;
}

/**
 * Simple broadcast function, for now we just assume all non connected FDs
 * are set to 0 as per initialization of the server struct in the main
 * function.
 */
void broadcast_message(Server *server, const char *buf) {
    int nwrite = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = server->clients[i];
        if (c == NULL)
            continue;
        printf("Broadcasting to %s\n", c->nick);
        char msg[256];
        int msglen = snprintf(msg, sizeof(msg), "%s> %s", c->nick, buf);
        nwrite = write(c->fd, msg, msglen);
        if (nwrite < 0)
            perror("write(3)");
    }
}

int main(void) {

    printf("Server init\n\n");

    Server server = {.fd = 0, .clients = {NULL}};

    // Make the server listen unblocking
    if (cl_listen(&server, ADDR, PORT, BACKLOG) == -1) {
        fprintf(stderr, "Error listening on %s:%i\n", ADDR, PORT);
        return -1;
    }

    int nfds = 0;
    int epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create1");
        return -1;
    }

    // Register the server listening socket into the epoll loop
    ev.events = EPOLLIN;
    ev.data.fd = server.fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, server.fd, &ev) == -1) {
        perror("epoll_ctl: server fd");
        return -1;
    }

    // Start the event loop
    for (;;) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            return -1;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server.fd) {
                int client_fd = cl_accept(&server);
                if (client_fd == -1) {
                    perror("accept");
                    return -1;
                }
                set_nonblocking(client_fd);

                // Let's make a client here
                Client *c = cl_malloc(sizeof(Client));
                c->fd = client_fd;
                snprintf(c->nick, sizeof(c->nick), "anon:%d", client_fd);
                server.clients[client_fd] = c;

                // Let's send some welcome message
                char msg[256];
                int msglen = snprintf(
                    msg, sizeof(msg),
                    "Server> Welcome %s! Use /nick to set a nickname\n\n",
                    c->nick);
                int nwrite = write(c->fd, msg, msglen);

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                    perror("epoll_ctl: client fd");
                    return -1;
                }
            } else {
                char buf[256] = {0};
                int nread = read(events[i].data.fd, buf, sizeof(buf) - 1);
                if (nread < 0) {
                    printf("Client disconnected fd=%i\n", events[i].data.fd);
                    close(events[i].data.fd);
                    free(server.clients[events[i].data.fd]);
                } else {
                    buf[nread] = 0;
                    if (strncmp(buf, "/quit", 5) == 0) {
                        // Client wants to disconnect here
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd,
                                      NULL) < 0)
                            perror("disconnecting client");
                        close(events[i].data.fd);
                        free(server.clients[events[i].data.fd]);
                    } else if (strncmp(buf, "/nick", 5) == 0) {
                        Client *c = server.clients[events[i].data.fd];
                        char raw_nick[NICK_MAX_LENGTH];
                        strncpy(raw_nick, buf + 5, nread);
                        char *nick = trim_string(raw_nick);
                        strncpy(c->nick, nick, strlen(raw_nick));
                    } else {
                        printf("(%i bytes) %s", nread, buf);
                        broadcast_message(&server, buf);
                    }
                }
            }
        }
    }

    return 0;
}
