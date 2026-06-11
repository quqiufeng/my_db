#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 帧头定义
 *
 * 全链路统一的 TCP 二进制协议：
 *
 *  ┌──────────────────────────────────────────────────┐
 *  │  Magic(2B) │ Type(1B) │ Flags(1B) │ Len(4B)     │  ← 8B 固定头
 *  ├──────────────────────────────────────────────────┤
 *  │  Payload (变长，由 Len 指定)                       │
 *  ├──────────────────────────────────────────────────┤
 *  │  Checksum(4B)  CRC32                             │
 *  └──────────────────────────────────────────────────┘
 *
 * - Magic:  0xAG 0xNT ("AGENT")
 * - Type:   消息类型
 * - Flags:  控制位
 * - Len:    Payload 长度（不含头、不含校验）
 * - Payload: JSON 数据（UTF-8）
 * - CRC32:  从 Type 到 Payload 末尾
 * =================================================================== */

#define PROTO_MAGIC_0       0x41  /* A */
#define PROTO_MAGIC_1       0x47  /* G */
#define PROTO_HEADER_LEN    8
#define PROTO_CRC_LEN       4
#define PROTO_MAX_PAYLOAD   (64 * 1024)   /* 最大 64KB payload */

/* Flags 控制位 */
#define PROTO_FLAG_REQUEST  0x01   /* 请求包 */
#define PROTO_FLAG_RESPONSE 0x02   /* 响应包 */
#define PROTO_FLAG_ACK      0x04   /* 需要 ACK */
#define PROTO_FLAG_ERROR    0x08   /* 错误包 */

/* ===================================================================
 * 消息类型枚举
 * =================================================================== */
typedef enum {
    /* --- 集群内部（0x01-0x0F） --- */
    MSG_HEARTBEAT        = 0x01,   /* Leader → Follower：心跳 */
    MSG_HEARTBEAT_ACK    = 0x02,   /* Follower → Leader：心跳确认 */
    MSG_ELECTION_VOTE_REQ = 0x03,  /* Candidate → Peer：请求投票 */
    MSG_ELECTION_VOTE_RESP = 0x04, /* Peer → Candidate：投票响应 */
    MSG_COORD            = 0x05,   /* 胜出者 → 所有人：宣布新 Leader */
    MSG_OK               = 0x06,   /* 节点 → 节点：确认接受 */

    /* --- 任务通道（0x10-0x1F） --- */
    MSG_TASK_DISPATCH    = 0x10,   /* Leader → Follower：下发任务 */
    MSG_TASK_RESULT      = 0x11,   /* Follower → Leader：任务结果 */
    MSG_STATUS_REPORT    = 0x12,   /* Follower → Leader：状态上报 */

    /* --- Master 通道（0x20-0x2F） --- */
    MSG_MASTER_HELLO     = 0x20,   /* Master → Agent：连接声明 */
    MSG_MASTER_LEADER    = 0x21,   /* Leader → Master：Leader 报备 */
    MSG_MASTER_CMD       = 0x22,   /* Master → Leader：下发指令 */
    MSG_MASTER_RESULT    = 0x23,   /* Leader → Master：指令结果 */
    MSG_MASTER_QUERY     = 0x24,   /* Master → Agent：查询身份+状态 */
    MSG_MASTER_STATUS    = 0x25,   /* Agent → Master：返回身份+状态 */
} msg_type_t;

/* ===================================================================
 * 帧头结构
 * =================================================================== */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];      /* 固定 0xAG 0xNT */
    uint8_t  type;          /* 消息类型 */
    uint8_t  flags;         /* 控制位 */
    uint32_t len;           /* payload 长度（网络序） */
} proto_header_t;

/* ===================================================================
 * 会话上下文
 *
 * 每个 TCP 连接对应一个 session_ctx_t，Agent 在其中维护自己的
 * 身份（namespace + self）和实时状态（status）。
 *
 * Master 发送 MSG_MASTER_QUERY 后，Agent 将整个上下文序列化为 JSON 返回。
 * =================================================================== */
typedef struct {
    /* ── namespace：集群视角 ── */
    char     leader_addr[128];     /* 当前 Leader 地址 "192.168.1.10:9527" */
    int      epoch;               /* 当前纪元号 */
    int      cluster_size;        /* 集群节点总数 */

    /* ── self：自身视角 ── */
    char     node_id[64];         /* 节点 ID "node1" */
    char     role[16];            /* "leader" / "follower" */
    char     self_addr[128];       /* 本机监听地址 "192.168.1.10:9527" */

    /* ── status：实时状态（定期更新） ── */
    double   load_1;              /* 1 分钟负载 */
    double   mem_pct;             /* 内存使用率 % */
    double   cpu_pct;             /* CPU 使用率 % */
    double   disk_pct;            /* 磁盘使用率 % */
    long     uptime_sec;          /* 运行时长（秒） */
    int      client_count;        /* 当前连接数 */
} session_ctx_t;

/* ===================================================================
 * 协议函数声明
 * =================================================================== */

/**
 * pack_header - 构造帧头
 * @hdr:  输出，帧头缓冲区
 * @type: 消息类型
 * @flags: 控制位
 * @len:  payload 长度
 */
void pack_header(proto_header_t *hdr, uint8_t type, uint8_t flags, uint32_t len);

/**
 * unpack_header - 解析帧头
 * @hdr:   输入，帧头缓冲区
 * @type:  输出，消息类型
 * @flags: 输出，控制位
 * @len:   输出，payload 长度
 * 返回 0 成功，-1 magic 错误
 */
int unpack_header(const proto_header_t *hdr, uint8_t *type, uint8_t *flags, uint32_t *len);

/**
 * crc32_bytes - 计算 CRC32 校验
 * @data: 数据起始（从 type 字段开始，不含 magic）
 * @len:  数据长度（type + flags + len + payload）
 * 返回 CRC32 值
 */
uint32_t crc32_bytes(const uint8_t *data, size_t len);

/**
 * send_message - 发送一条完整消息（帧头 + payload + CRC）
 * @fd:       TCP socket
 * @type:     消息类型
 * @flags:    控制位
 * @payload:  payload 数据（JSON 字符串）
 * @payload_len: payload 长度
 * 返回 0 成功，-1 失败
 */
int send_message(int fd, uint8_t type, uint8_t flags,
                 const char *payload, uint32_t payload_len);

/**
 * recv_message - 接收一条完整消息
 * @fd:       TCP socket
 * @type:     输出，消息类型
 * @flags:    输出，控制位
 * @payload:  输出，payload 缓冲区（调用者分配，至少 PROTO_MAX_PAYLOAD）
 * @payload_len: 输出，payload 实际长度
 * 返回 0 成功，-1 失败/断开
 */
int recv_message(int fd, uint8_t *type, uint8_t *flags,
                 char *payload, uint32_t *payload_len);

/**
 * session_ctx_to_json - 将会话上下文序列化为 JSON 字符串
 * @ctx:   会话上下文
 * @json:  输出缓冲区（至少 1024 字节）
 * 返回 json 指针
 */
char *session_ctx_to_json(const session_ctx_t *ctx, char *json, size_t json_size);

/**
 * json_to_session_ctx - 从 JSON 解析会话上下文
 * @json: JSON 字符串
 * @ctx:  输出，会话上下文
 * 返回 0 成功，-1 解析失败
 */
int json_to_session_ctx(const char *json, session_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
