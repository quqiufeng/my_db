#include "crypto.h"
#include <string.h>
#include <stdlib.h>

/* 全局密钥 */
static uint8_t global_key[XXTEA_KEY_LEN] = {0};
static int    global_key_set = 0;

void xxtea_set_key(const uint8_t *key) {
    memcpy(global_key, key, XXTEA_KEY_LEN);
    global_key_set = 1;
}

void xxtea_get_key(uint8_t *key) {
    memcpy(key, global_key, XXTEA_KEY_LEN);
}

/* MX macro from XXTEA spec */
#define MX(z, y, sum, k) \
    ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(sum >> 2) & 3] ^ z)))


/* MX macro - standard XXTEA */
#define XXTEA_MX(z, y, sum, key, p, e) \
    ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (key[((sum >> 2) & 3) ^ e] ^ z)))

void xxtea_encrypt(uint8_t *data, size_t len) {
    if (!global_key_set || len < 8 || (len & 3)) return;
    uint32_t *v = (uint32_t *)data;
    const uint32_t *k = (const uint32_t *)global_key;
    size_t n = len / 4;
    uint32_t y = v[0], z = v[n-1], sum = 0, delta = 0x9E3779B9;
    size_t r = 6 + 52 / n;
    while (r-- > 0) {
        sum += delta;
        uint32_t e = (sum >> 2) & 3;
        for (size_t p = 0; p < n-1; p++) {
            y = v[p+1];
            v[p] += XXTEA_MX(z, y, sum, k, p, e);
            z = v[p];
        }
        y = v[0];
        v[n-1] += XXTEA_MX(z, y, sum, k, n-1, e);
        z = v[n-1];
    }
}

int xxtea_decrypt(uint8_t *data, size_t len) {
    if (!global_key_set || len < 8 || (len & 3)) return -1;
    uint32_t *v = (uint32_t *)data;
    const uint32_t *k = (const uint32_t *)global_key;
    size_t n = len / 4;
    uint32_t y = v[0], z = v[n-1], delta = 0x9E3779B9;
    size_t r = 6 + 52 / n;
    uint32_t sum = delta * r;
    while (r-- > 0) {
        uint32_t e = (sum >> 2) & 3;
        for (size_t p = n-1; p > 0; p--) {
            z = v[p-1];
            v[p] -= XXTEA_MX(z, y, sum, k, p, e);
            y = v[p];
        }
        z = v[n-1];
        v[0] -= XXTEA_MX(z, y, sum, k, 0, e);
        y = v[0];
        sum -= delta;
    }
    return 0;
}
size_t xxtea_encoded_len(size_t raw_len) {
    size_t aligned = (raw_len + 3) & ~3;
    if (aligned < 8) aligned = 8;
    return aligned;
}

void key_from_string(const char *str, uint8_t *key) {
    uint32_t h[4] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A};
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        h[0] ^= *p++;
        h[1] += h[0];
        h[2] ^= h[1];
        h[3] += h[2];
        h[0] ^= h[3];
        /* 简单的扩散 */
        uint32_t t = h[0] ^ (h[0] << 11);
        h[1] += t; h[2] ^= t >> 8;
    }
    memcpy(key, h, XXTEA_KEY_LEN);
    global_key_set = 1;
}
