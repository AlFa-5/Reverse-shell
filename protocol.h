#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define HEADER_LEN      4
#define MAX_MSG_SIZE    65536
#define HEARTBEAT_INT   30
#define RECONNECT_DELAY 5
#define DEFAULT_PORT    4444
#define MAX_CLIENTS     128
#define BUF_SIZE        65536

/* Paquet : [4 bytes payload length LE][payload] */
/* Retourne la taille totale du paquet ou -1 si plen invalide */
static inline int pack_msg(const uint8_t *payload, int plen, uint8_t *out) {
    if (plen < 0 || plen > MAX_MSG_SIZE) return -1;
    out[0] = plen & 0xFF;
    out[1] = (plen >> 8) & 0xFF;
    out[2] = (plen >> 16) & 0xFF;
    out[3] = (plen >> 24) & 0xFF;
    if (plen > 0 && payload)
        memcpy(out + HEADER_LEN, payload, plen);
    return HEADER_LEN + plen;
}

/* Extrait un message du buffer. 
   Retourne le nombre d'octets consommés, 0 si pas assez de données, -1 si erreur */
static inline int try_unpack(const uint8_t *buf, int blen,
                             uint8_t *msg_out, int *msg_len) {
    if (blen < HEADER_LEN) return 0;
    int plen = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    if (plen < 0 || plen > MAX_MSG_SIZE) return -1;
    if (blen < HEADER_LEN + plen) return 0;
    if (plen > 0)
        memcpy(msg_out, buf + HEADER_LEN, plen);
    *msg_len = plen;
    return HEADER_LEN + plen;
}

#endif