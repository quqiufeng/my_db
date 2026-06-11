#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "agent.h"
#include <errno.h>   /* htonl, ntohl */

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
int send_message(int fd, uint8_t type, uint8_t flags,
                 const char *payload, uint32_t payload_len) {
    proto_header_t hdr;
    uint8_t        crc_buf[PROTO_CRC_LEN];
    uint32_t       crc;
    uint32_t       nlen = htonl(payload_len);
    int            ret;

    /* 限制 payload 大小 */
    if (payload_len > PROTO_MAX_PAYLOAD)
        payload_len = PROTO_MAX_PAYLOAD;

    /* 构造帧头 */
    pack_header(&hdr, type, flags, payload_len);

    /* 发送帧头（循环直到全部发完） */
    {
        size_t remain = PROTO_HEADER_LEN;
        const uint8_t *ptr = (const uint8_t *)&hdr;
        while (remain > 0) {
            ret = (int)write(fd, ptr, remain);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
                return -1;
            }
            ptr    += ret;
            remain -= ret;
        }
    }

    /* 发送 payload */
    if (payload_len > 0) {
        {
        size_t remain = payload_len;
        const uint8_t *ptr = (const uint8_t *)payload;
        while (remain > 0) {
            ret = (int)write(fd, ptr, remain);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
                return -1;
            }
            ptr    += ret;
            remain -= ret;
        }
    }
    }

    /* 计算并发送 CRC（从 type 到 payload 末尾） */
    /* 临时变量用于 CRC：type + flags + nlen + payload */
    {
        uint8_t crc_data[8 + PROTO_MAX_PAYLOAD];
        size_t  crc_len = 0;
        crc_data[crc_len++] = hdr.type;
        crc_data[crc_len++] = hdr.flags;
        memcpy(crc_data + crc_len, &nlen, 4); crc_len += 4;
        if (payload_len > 0) {
            memcpy(crc_data + crc_len, payload, payload_len);
            crc_len += payload_len;
        }
        crc = crc32_bytes(crc_data, crc_len);
    }

    crc_buf[0] = (crc >> 24) & 0xFF;
    crc_buf[1] = (crc >> 16) & 0xFF;
    crc_buf[2] = (crc >>  8) & 0xFF;
    crc_buf[3] = (crc >>  0) & 0xFF;

    {
        size_t remain = PROTO_CRC_LEN;
        const uint8_t *ptr = crc_buf;
        while (remain > 0) {
            ret = (int)write(fd, ptr, remain);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
                return -1;
            }
            ptr    += ret;
            remain -= ret;
        }
    }

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

int json_to_session_ctx(const char *json, session_ctx_t *ctx) {
    /* 简易解析：用 sscanf 提取关键字段 */
    session_ctx_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    /* 提取 namespace */
    if (sscanf(json,
        "%*[^{]{%*[^:]:\"%63[^\"]\","
        "%*[^:]:%d,"
        "%*[^:]:%d",
        tmp.leader_addr, &tmp.epoch, &tmp.cluster_size) < 3)
        return -1;

    /* 提取 self */
    const char *p = strstr(json, "\"self\"");
    if (!p) return -1;
    if (sscanf(p,
        "%*[^{]{%*[^:]:\"%63[^\"]\","
        "%*[^:]:\"%15[^\"]\","
        "%*[^:]:\"%63[^\"]\"",
        tmp.node_id, tmp.role, tmp.self_addr) < 3)
        return -1;

    /* 提取 status */
    p = strstr(json, "\"status\"");
    if (!p) return -1;
    if (sscanf(p,
        "%*[^{]{%*[^:]:%lf,"
        "%*[^:]:%lf,"
        "%*[^:]:%lf,"
        "%*[^:]:%lf,"
        "%*[^:]:%ld,"
        "%*[^:]:%d",
        &tmp.load_1, &tmp.mem_pct, &tmp.cpu_pct,
        &tmp.disk_pct, &tmp.uptime_sec, &tmp.client_count) < 6)
        return -1;

    *ctx = tmp;
    return 0;
}
