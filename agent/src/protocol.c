#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "agent.h"
#include <stdlib.h>   /* htonl, ntohl */

/* ===================================================================
 * CRC32 表（简化实现，生产环境可用 zlib 的 crc32）
 * =================================================================== */
static uint32_t crc32_table[256];
static int      crc32_initialized = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}

uint32_t crc32_bytes(const uint8_t *data, size_t len) {
    if (!crc32_initialized) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ===================================================================
 * 帧头编解码
 * =================================================================== */
void pack_header(proto_header_t *hdr, uint8_t type, uint8_t flags, uint32_t len) {
    hdr->magic[0] = PROTO_MAGIC_0;
    hdr->magic[1] = PROTO_MAGIC_1;
    hdr->type     = type;
    hdr->flags    = flags;
    hdr->len      = htonl(len);   /* 网络序 */
}

int unpack_header(const proto_header_t *hdr, uint8_t *type, uint8_t *flags, uint32_t *len) {
    if (hdr->magic[0] != PROTO_MAGIC_0 || hdr->magic[1] != PROTO_MAGIC_1)
        return -1;
    if (type)  *type  = hdr->type;
    if (flags) *flags = hdr->flags;
    if (len)   *len   = ntohl(hdr->len);
    return 0;
}

/* ===================================================================
 * 收发完整消息
 * =================================================================== */


/* simple XOR obfuscation (not real crypto, just wire obfuscation) */
void proto_xor(uint8_t *data, size_t len, uint8_t key) {
    for (size_t i = 0; i < len; i++) data[i] ^= key;
}
int send_message(int fd, uint8_t type, uint8_t flags,
                 const char *payload, uint32_t payload_len) {
    uint8_t  buf[256];
    int      total;
    int      ret;

    if (payload_len > sizeof(buf) - PROTO_HEADER_LEN - 4)
        payload_len = sizeof(buf) - PROTO_HEADER_LEN - 4;

    pack_header((proto_header_t *)buf, type, flags, payload_len);

    if (payload_len > 0)
        memcpy(buf + PROTO_HEADER_LEN, payload, payload_len);

    {
        uint32_t crc = crc32_bytes(buf + 2, PROTO_HEADER_LEN - 2 + payload_len);
        crc = htonl(crc);
        memcpy(buf + PROTO_HEADER_LEN + payload_len, &crc, 4);
    }

    total = PROTO_HEADER_LEN + payload_len + 4;
    ret = (int)write(fd, buf, total);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -1;
    }
    if (ret != total) return -1;
    return 0;
}

int recv_message(int fd, uint8_t *type, uint8_t *flags,
                 char *payload, uint32_t *payload_len) {
    proto_header_t hdr;
    uint8_t        crc_buf[PROTO_CRC_LEN];
    uint32_t       crc_recv, crc_calc;
    int            ret;

    /* 读取帧头（循环直到全部读完） */
    {
        size_t remain = PROTO_HEADER_LEN;
        uint8_t *ptr = (uint8_t *)&hdr;
        while (remain > 0) {
            ret = (int)read(fd, ptr, remain);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
                return -1;
            }
            if (ret == 0) return -1;
            ptr    += ret;
            remain -= ret;
        }
    }

    /* 解析帧头 */
    uint32_t len;
    if (unpack_header(&hdr, type, flags, &len) < 0)
        return -1;

    if (payload_len) *payload_len = len;

    /* 限制接收大小 */
    if (len > PROTO_MAX_PAYLOAD)
        len = PROTO_MAX_PAYLOAD;

    /* 读取 payload（循环直到全部读完） */
    if (len > 0 && payload) {
        size_t remain = len;
        uint8_t *ptr = (uint8_t *)payload;
        while (remain > 0) {
            ret = (int)read(fd, ptr, remain);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
                return -1;
            }
            if (ret == 0) return -1;
            ptr    += ret;
            remain -= ret;
        }
        payload[len] = '\0';
    }

    /* 读取 CRC（循环直到全部读完） */
    {
        size_t remain = PROTO_CRC_LEN;
        uint8_t *ptr = crc_buf;
        while (remain > 0) {
            ret = (int)read(fd, ptr, remain);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
                return -1;
            }
            if (ret == 0) return -1;
            ptr    += ret;
            remain -= ret;
        }
    }

    /* 校验 CRC */
    crc_recv = ((uint32_t)crc_buf[0] << 24) |
               ((uint32_t)crc_buf[1] << 16) |
               ((uint32_t)crc_buf[2] <<  8) |
               ((uint32_t)crc_buf[3] <<  0);

    uint32_t nlen = htonl(len);
    uint8_t  crc_data[8 + PROTO_MAX_PAYLOAD];
    size_t   crc_len = 0;
    crc_data[crc_len++] = hdr.type;
    crc_data[crc_len++] = hdr.flags;
    memcpy(crc_data + crc_len, &nlen, 4); crc_len += 4;
    if (len > 0 && payload) {
        memcpy(crc_data + crc_len, payload, len);
        crc_len += len;
    }
    crc_calc = crc32_bytes(crc_data, crc_len);

    if (crc_recv != crc_calc)
        return -1;  /* 校验失败 */

    return 0;
}

/* ===================================================================
 * 会话上下文 ↔ JSON 序列化
 *
 * 简易实现：用 sprintf 拼 JSON，不用第三方库
 * =================================================================== */
char *session_ctx_to_json(const session_ctx_t *ctx, char *json, size_t json_size) {
    snprintf(json, json_size,
        "{"
        "\"namespace\":{"
          "\"leader\":\"%s\","
          "\"epoch\":%d,"
          "\"cluster_size\":%d"
        "},"
        "\"self\":{"
          "\"id\":\"%s\","
          "\"role\":\"%s\","
          "\"addr\":\"%s\""
        "},"
        "\"status\":{"
          "\"load_1\":%.2f,"
          "\"mem_pct\":%.1f,"
          "\"cpu_pct\":%.1f,"
          "\"disk_pct\":%.1f,"
          "\"uptime_sec\":%ld,"
          "\"clients\":%d"
        "}"
        "}",
        ctx->leader_addr,
        ctx->epoch,
        ctx->cluster_size,
        ctx->node_id,
        ctx->role,
        ctx->self_addr,
        ctx->load_1,
        ctx->mem_pct,
        ctx->cpu_pct,
        ctx->disk_pct,
        ctx->uptime_sec,
        ctx->client_count);
    return json;
}

