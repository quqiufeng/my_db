# Agent — AI 驱动的 Linux 集群控制引擎

> **面向 AI 编程助手的集群控制工具。** 纯 C 语言，一个二进制文件，SCP 部署，零依赖，**Raft 自组织选举**，自动故障恢复。
> 让 AI 像操作一台机器一样操作整个集群——部署、命令执行、文件分发、服务管理，全部通过结构化 JSON 接口完成。

> ✅ **当前状态：全部核心功能完成，44 个单元测试通过，`make test` 无警告。**

---

## 设计理念

### AI-first（AI 优先）

这个项目的核心定位是让 **AI 编程助手** 能够像操作一台机器一样操作整个集群：

```
┌──────────────────────────────────────────────────────────────────────────┐
│                                                                          │
│   ┌──────────────┐      ┌──────────────────┐      ┌──────────────────┐  │
│   │  AI 编程助手  │ ──── │   clusterctl     │ ──── │   Agent 集群     │  │
│   │  (Claude,     │ JSON │   (Master 端)    │ TCP  │   (Leader +      │  │
│   │   Cursor,     │ ◀─── │                  │ ◀─── │    Follower)     │  │
│   │   OpenCode)   │      │   stdin/stdout   │      │                  │  │
│   └──────────────┘      └──────────────────┘      └──────────────────┘  │
│                                                                          │
│   ● AI 发一条 shell 命令即可控制整个集群                                  │
│   ● 所有输出都是 JSON，AI 天然可解析                                      │
│   ● 自部署：AI 可以自主完成 scp → SSH → 选举 → 就绪 全流程               │
│   ● 自愈：Leader 挂了 AI 不需要管，集群自动恢复                           │
└──────────────────────────────────────────────────────────────────────────┘
```

**关键设计决策：**

| 对比维度 | 传统工具（Ansible/Salt/SSH） | 本项目 |
|---------|------------------------------|--------|
| 用户 | 人类运维工程师 | **AI + 人类** |
| 输出格式 | 人类可读的彩色文本 | **JSON（AI 可直接解析）** |
| 部署方式 | 配 inventory、装 Python 依赖 | **一个二进制 SCP 过去就完事** |
| 架构 | 中心化或纯 SSH | **Raft 自组织选举，三层控制** |
| 故障处理 | 人工介入 | **自动选举恢复** |

### 为什么用 C 而不是 Go/Python/Rust

| 语言 | 问题 |
|------|------|
| **Python** | 依赖 sysadmin 装 Python3 + pip，SCP 过去不能直接跑。违背"零依赖"目标 |
| **Go** | 静态编译确实好，但 runtime > 1MB，交叉编译需要特定 toolchain，且 goroutine 调试复杂 |
| **Rust** | 编译慢，学习曲线陡。在 AI 编程场景下 AI 写 Rust 的正确率远低于 C |
| **C** | 500KB 静态二进制，SCP 过去 `chmod +x` 就能跑。任何 Linux 都自带 libc。AI 训练数据中 C 占比最高 |

### 为什么不直接用 etcd / ZooKeeper

简单：agent 是一个零依赖的二进制，不需要额外跑一个分布式存储。etcd/ZooKeeper 太重，部署复杂度高，且一旦宕机需要人工恢复。本项目把 Raft 选举内嵌在每个 agent 中，启动即选主。

---

## 架构

### 三层控制

```
┌──────────────────────────────────────────────────────────────────┐
│                       Master 运维机                              │
│                                                                  │
│   ┌─────────────┐         ① clusterctl --connect 192.168.1.10   │
│   │  clusterctl │──────── TCP 9527 ──────┐                      │
│   │  (CLI/JSON) │                         │ 等待 Leader 报备     │
│   └─────────────┘                         │                      │
│   AI ──shell──► clusterctl                │                      │
│                                          │                      │
│   ④ Leader 报备 → Master 就绪            │                      │
│   ⑤ 下发任务                             │                      │
└──────────────────────────────────────────┼──────────────────────┘
                                           │
                                           ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Linux 服务器集群                               │
│                                                                  │
│   ┌──────────────────┐     ┌──────────────────┐                 │
│   │  Server A        │     │  Server B        │                 │
│   │  ┌────────────┐  │     │  ┌────────────┐  │                 │
│   │  │  Agent     ├──┼─────┼──┤  Agent     │  │                 │
│   │  │  (Leader)  │  │  TCP│  │  (Follower)│  │                 │
│   │  └────────────┘  │     │  └────────────┘  │                 │
│   └──────────────────┘     └──────────────────┘                 │
│                       ② Raft 选举                                │
│   ┌──────────────────┐     ┌──────────────────┐                 │
│   │  Server C        │     │  Server D        │                 │
│   │  ┌────────────┐  │     │  ┌────────────┐  │                 │
│   │  │  Agent     │  │     │  │  Agent     │  │                 │
│   │  │  (Follower)│  │     │  │  (Follower)│  │                 │
│   │  └────────────┘  │     │  └────────────┘  │                 │
│   └──────────────────┘     └──────────────────┘                 │
│                                                                  │
│   ───► 心跳 (Leader → Follower)                                  │
│   ───► Raft 选举投票 (Candidate → Peer)                          │
│   ───► 状态上报 (Follower → Leader) [待实现]                      │
│                                                                  │
│   ③ Leader 选出后 → 主动向 Master 报备 "我是 Leader, epoch=N"     │
└──────────────────────────────────────────────────────────────────┘
```

### 通信方式

| 链路 | 协议 | 方向 | 说明 |
|------|------|------|------|
| clusterctl → Agent | TCP 自定义二进制 | Master → 任一节点 | 先连接，等待 Leader 报备 |
| Agent ↔ Agent | TCP 自定义二进制 | 全连接 | 选举投票、心跳、任务分发 |
| Leader → Master | TCP 自定义二进制 | Leader → clusterctl | Leader 选出后主动报备 |

整个集群**只用一种协议**、**一个端口**（`--port`），没有 HTTP、没有 SSH 隧道。

---

## TCP 通信协议

### 帧格式

```
┌──────────────────────────────────────────────────┐
│  Magic(2B) │ Type(1B) │ Flags(1B) │ Len(4B)     │  ← 8B 固定头
├──────────────────────────────────────────────────┤
│  Payload (变长，由 Len 指定)                       │
├──────────────────────────────────────────────────┤
│  Checksum(4B)  CRC32                             │
└──────────────────────────────────────────────────┘
```

- **Magic**: `0xAG 0xNT`（"AGENT" 缩写）
- **Type**: 消息类型（见消息表）
- **Flags**: 控制位（第1位=请求/响应，第2位=需要ACK）
- **Len**: Payload 长度（大端序，不含头、不含校验）
- **Payload**: JSON 数据（UTF-8）
- **Checksum**: CRC32（从 Type 到 Payload 末尾）

### 原子发送设计

早期的实现用了三次独立的 `write()`（分别写头、载荷、CRC），导致一个关键 bug：

```
发送端: write(header) ✓ → write(payload) ✗ EAGAIN
                              ↑ 头已发送但载荷没发出去
接收端: read(header) ✓ → read(payload) ✗ EAGAIN
                              ↑ 收到不完整的消息 → 协议错误 → 关连接
```

**修复：单缓冲区原子发送**

```c
// 把整个帧拼到一个 buffer 里，一次 write() 写完
uint8_t buf[256];
pack_header((proto_header_t *)buf, type, flags, payload_len);
memcpy(buf + HEADER_LEN, payload, payload_len);
// CRC...
int ret = write(fd, buf, HEADER_LEN + payload_len + 4);
if (ret != total) return -1;  // 要么全发要么全失败
```

这个设计保证**不会出现部分发送**——内核 TCP 栈要么把整个 buffer 发出去，要么 EAGAIN 一个字节都不发。

### 收端循环读取

接收端使用循环读取，防止 `read()` 返回部分数据：

```c
// 循环读取帧头，直到全部读完或 EAGAIN
size_t remain = PROTO_HEADER_LEN;
uint8_t *rp = (uint8_t *)&hdr;
while (remain > 0) {
    ret = read(fd, rp, remain);
    if (ret < 0) { if (errno == EAGAIN) return -1; return -1; }
    if (ret == 0) return -1;  // EOF = 对方关闭连接
    rp += ret;
    remain -= ret;
}
```

### epoll 模式

从边缘触发（`EPOLLET`）改为**水平触发**：

- 水平触发：只要 fd 有数据，每次 `epoll_wait` 都会返回
- **优点**：不需要在 `handle_read` 里循环读到 EAGAIN，每次只读一个消息即可
- **容错**：如果某次读得不完整（EAGAIN），下次 `epoll_wait` 会继续返回同一个 fd

### 单向连接模型（Anti-dual-connection）

**问题：** 原始实现中每对节点之间会建立两条 TCP 连接（各自主动连接对方），导致：

1. peer 结构只跟踪一条 fd，另一条变"孤儿连接"
2. 孤儿连接收到数据或断开时，触发异常事件
3. 异常事件导致 `peer_mark_offline`，关闭主连接
4. 主连接关闭后对端也断开 → 雪崩式断连

**修复：每个 pair 只建一条连接**

```
规则：port 小的节点主动连 port 大的节点

node0(9527) ── 出站 ──► node1(9528)     # 9527 < 9528 → 主动连
node0(9527) ── 出站 ──► node2(9529)     # 9527 < 9529 → 主动连
node1(9528) ── 出站 ──► node2(9529)     # 9528 < 9529 → 主动连
node2(9529)    无出站连接                # 最大 port，等别人连
```

- `peer_connect_all` / `peer_reconnect` 都遵循这个规则
- 入站 `accept` 按 `IP + PEER_DISCONNECTED` 状态匹配 peer
- `handle_peer_connect` 检测到 peer 已通过入站连接连通时，关闭新出站连接
- 重连间隔从 5s 缩短到 1s，配合单向模型使断连后快速恢复

---

## 选举算法：Raft 风格

### 角色与术语

| 角色 | 说明 |
|------|------|
| **Follower** | 初始状态，等待 Leader 心跳或选举超时 |
| **Candidate** | 选举超时后自升，发起投票 |
| **Leader** | 赢得多数票，负责心跳、任务分发 |

| 术语 | 说明 |
|------|------|
| **Term（任期）** | 每次选举递增，旧任期消息被忽略 |
| **选举超时** | 随机 1.5~3.0s，防同时触发 |
| **多数派** | `ceil(N/2) + 1`，3 节点需要 2 票 |
| **心跳** | Leader 每秒广播一次，Follower 收到后重置选举定时器 |

### 选举流程

```
1. 触发条件
   a. 首次启动，没有已知 Leader
   b. Follower 选举超时（Leader 心跳超时 3 次 × 9s）

2. 选举
   a. Follower → Candidate：term++，投自己一票
   b. 广播 MSG_ELECTION_VOTE_REQ 给所有已连接 peer
   c. 每个 Peer 在同一 term 最多投一票（先到先得）
   d. Candidate 收集投票，达到多数即当选 Leader
   e. 若 vote_deadline(3s) 内未达到多数，增加 term 重新选举

3. 当选
   a. Leader 广播 MSG_COORD 通知所有人
   b. Follower 收到后更新 leader_addr，进入稳定状态
   c. Leader 开始定期发送 MSG_HEARTBEAT（每秒 1 次）
   d. Follower 收到心跳后重置选举定时器

4. 故障恢复
   a. Leader 宕机 → Follower 心跳超时 → 重新选举
   b. 新 Leader 以更高 term 当选，广播 COORD
   c. 旧 Leader 恢复后收到更高 term 的消息 → 自动降级为 Follower
```

### 示例（3 节点，--cluster-size 3，多数派 = 2）

```text
 节点A (term=0,F) ── 超时 ──► Candidate(term=1)
    ├── 投自己 (1/3)
    ├── MSG_ELECTION_VOTE_REQ → 节点B → 同意 (2/3) ✓
    ├── MSG_ELECTION_VOTE_REQ → 节点C → 已投节点B (2/3)
    └── 达到多数 → Leader!
         ├── MSG_COORD → 节点B → "LEADER ELECTED: A" ✓
         ├── MSG_COORD → 节点C → "LEADER ELECTED: A" ✓
         └── MSG_HEARTBEAT (每秒) → 重置 Follower 定时器

 节点B (term=0,F) ── 收到 A 的请求
    └── vote_granted=1 (term=1)

 节点C (term=0,F) ── 先收到 B 的请求（若 B 也同时竞选）
    └── vote_granted=1 (term=1, voted_for=B)
    └── A 的请求到达 → voted_for=-2 ≠ -1 → 拒绝
```

---

## 消息类型

| 消息类型 | 值 | 方向 | 用途 |
|---------|-----|------|------|
| `MSG_HEARTBEAT` | 0x01 | **Leader → Follower** | 心跳保活 |
| `MSG_HEARTBEAT_ACK` | 0x02 | Follower → Leader | 心跳确认 |
| `MSG_ELECTION_VOTE_REQ` | 0x03 | Candidate → Peer | 请求投票 |
| `MSG_ELECTION_VOTE_RESP` | 0x04 | Peer → Candidate | 投票响应 |
| `MSG_COORD` | 0x05 | 胜出者 → 所有人 | 宣布新 Leader |
| `MSG_OK` | 0x06 | 节点 → 节点 | 确认接受 |
| `MSG_TASK_DISPATCH` | 0x10 | Leader → Follower | ✅ 命令执行，返回真实 stdout/exit |
| `MSG_TASK_RESULT` | 0x11 | Follower → Leader | ✅ 异步收集，pending_task 追踪 |
| `MSG_STATUS_REPORT` | 0x12 | Follower → Leader | ✅ 每 5 秒上报 CPU/内存/磁盘 |
| `MSG_MASTER_HELLO` | 0x20 | Master → Agent | Master 连接声明 |
| `MSG_MASTER_LEADER` | 0x21 | Leader → Master | Leader 主动报备 ✅ |
| `MSG_MASTER_CMD` | 0x22 | Master → Leader | ✅ 支持 exec/query action |
| `MSG_MASTER_RESULT` | 0x23 | Leader → Master | ✅ 聚合所有节点结果 |
| `MSG_MASTER_QUERY` | 0x24 | Master → 任一 Agent | 查询身份 + 状态 |
| `MSG_MASTER_STATUS` | 0x25 | Agent → Master | 返回身份 + 状态 |

---

## 模块结构

```
agent/
├── README.md
├── Makefile
│
├── src/
│   ├── main.c           # 入口：参数解析 → daemonize → 启动
│   ├── server.c         # epoll 事件循环 + TCP 管理 + 消息分发
│   ├── peer.c           # 对端管理：单向连接、重连、保活
│   ├── election.c       # Raft 选举 FSM
│   ├── protocol.c       # 二进制帧编解码（单缓冲区原子发送）
│   ├── crypto.c         # XXTEA 加密（128-bit key）
│   ├── plugin.c         # LuaJIT 插件系统（热更新、cjson）
│   ├── task.c           # 任务执行 + 结果序列化
│   ├── json.c           # 轻量 JSON 工具
│   ├── clusterctl.c     # Master 端 CLI
│   └── deploy.c         # SCP + SSH 批量部署
│
├── include/
│   ├── agent.h          # 公共类型 + 全局状态
│   ├── protocol.h       # 帧头定义 + 消息类型枚举
│   ├── election.h       # 选举状态机接口
│   ├── peer.h           # 对端管理接口
│   ├── task.h           # 任务结构体（预留）
│   └── json.h           # JSON 工具接口
```

### 核心模块职责

| 模块 | 职责 |
|------|------|
| **main.c** | 解析 `--port` `--idx` `-ip` `--cluster-size` 等参数；
  初始化 election 定时器；进入 epoll 事件循环 |
| **server.c** | epoll 事件循环（水平触发）；TCP listen/accept；
  消息分发（handle_message 开关表）；选举 tick（election_tick）；
  accept 按 IP+状态匹配 peer；handle_peer_connect 处理冗余连接 |
| **peer.c** | 单向连接模型（只连 higher port）；重连间隔 1s；
  peer_mark_offline 关闭 fd 并调度重连 |
| **election.c** | Follower → Candidate → Leader 状态机；
  随机超时生成；多数派判定；任期管理 |
| **crypto.c** | XXTEA 加解密（128-bit key）、密钥派生；
| **plugin.c** | LuaJIT 虚拟机、脚本加载/热重载、API 绑定（on_tick/on_message/set_timer）；
| **task.c** | 任务执行（`popen`）、结果 JSON 序列化、ID 生成；
| **protocol.c** | 帧编解码；单缓冲区原子 write；循环 read；
  CRC32 校验；session_ctx JSON 序列化 |

---

## 核心优化历程

### TCP 断连问题的排查与修复

这是开发过程中最棘手的问题。症状：选举完成后所有连接同时断开。

**排查过程：**

1. 日志显示所有断连都在同一毫秒发生 → 不是随机网络问题，是代码触发
2. 加日志追踪断连来源 → 全部来自 `handle_read`（`recv_message` 返回 -1）
3. 追踪 `recv_message` 失败原因 → `read()` 返回 0（EOF），对端关闭了连接
4. 发现每对节点有两条 TCP 连接 → 一条的关闭触发另一条的断开
5. 深层原因：`send_message` 三阶段写入 → 头写成功但载荷写 EAGAIN → 收端收到不完整帧 → 协议错误 → 关连接

**修复链：**

| 步骤 | 修复 | 效果 |
|------|------|------|
| 1 | `recv_message` 循环读取 | 处理 `read()` 部分返回 |
| 2 | epoll 边缘触发 → 水平触发 | 避免 EAGAIN 导致事件丢失 |
| 3 | EPOLLHUP 处理前先 drain 数据 | 防止 HUP 时仍有未读数据 |
| 4 | `send_message` 单缓冲区原子写入 | 消除部分发送导致的帧损坏 |
| 5 | 单向连接模型（小 port 连大 port） | 消除双连接导致的 fd 混乱 |
| 6 | accept 匹配 IP+DISCONNECTED | 正确绑定入站连接到 peer |
| 7 | `handle_peer_connect` 处理 fd 已被替换 | 防止出站连接完成时 peer 已通过入站连通 |

---

## 快速开始

### 编译

```bash
cd agent
make                    # 编译 agent 二进制
make clusterctl deploy  # 编译 Master 端工具
```

### 本地模拟 3 节点集群

```bash
# 终端 1 — node0（port 9527）
./agent --foreground --bind 127.0.0.1 --port 9527 \
       -ip 127.0.0.1:9528,127.0.0.1:9529 \
       --idx 0 --id node0 --cluster-size 3 --key "my-cluster-key"

# 终端 2 — node1（port 9528）
./agent --foreground --bind 127.0.0.1 --port 9528 \
       -ip 127.0.0.1:9527,127.0.0.1:9529 \
       --idx 1 --id node1 --cluster-size 3 --key "my-cluster-key"

# 终端 3 — node2（port 9529）
./agent --foreground --bind 127.0.0.1 --port 9529 \
       -ip 127.0.0.1:9527,127.0.0.1:9529 \
       --idx 2 --id node2 --cluster-size 3 --key "my-cluster-key"
```

> 启动后自动选举，约 1.5~3s 内选出 Leader。日志中可见：
> ```
> [I WON the election! term=1 votes=2/3]
> [LEADER ELECTED: 127.0.0.1:9528 epoch=1]
> ```
> `--foreground` 让进程在前台运行便于观察。去掉则自动 daemonize。

### 关键参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 9527 | 监听端口。**小端口连大端口**，每个 pair 只建一条连接 |
| `-ip` | — | 逗号分隔的 peer 地址列表。**所有节点必须都用完整的 IP 列表** |
| `--idx` | 0 | 节点在集群中的索引（0-based）。用于初始化选举定时器 |
| `--cluster-size` | peer_count+1 | 集群总节点数。**必须正确设置**以便计算多数派（N/2+1） |
| `--key` | — | **XXTEA 加密密钥**。所有节点和 clusterctl 必须使用相同密钥 |
| `--plugin-dir` | /etc/agent/plugins | Lua 插件目录，inotify 热更新 |
| `--foreground` | — | 前台运行（调试用）。默认 daemonize |
| `--bind` | 0.0.0.0 | 监听地址 |

### 部署到远程集群

```bash
# 批量部署（需要 SSH 密钥）
./deploy -ip 192.168.1.10,192.168.1.11,192.168.1.12 --user root

# 连接并等待自动就绪
./clusterctl --connect 192.168.1.10:9527 status
```

### 手动部署

```bash
scp agent root@192.168.1.10:/tmp/agent
ssh root@192.168.1.10 "/tmp/agent -ip 192.168.1.10,192.168.1.11,192.168.1.12 \
       --port 9527 --idx 0 --cluster-size 3"
# 其他节点类似
```

---

## 设计要点总结

### 为什么全连接（Full Mesh）而不是发现服务

每个节点启动时通过 `-ip` 知道全部对端。启动即开始建连，不需要 etcd/Consul 做服务发现。

**优点：** 零外部依赖，启动即通。

**代价：** 每节点需要配置完整的 IP 列表。但这对 AI 编程助手不是问题——AI 可以生成全部配置。

### 为什么单向连接而不是随机

每个 pair 只建一条连接，方向由 port 大小决定：

- 确定性强：每个节点都知道自己该连谁、等谁连
- 无竞争：不会出现双方同时连对方的情况
- 简单：`peer->port <= self->port` 就跳过，一行代码

### 为什么选举超时随机化

所有 Follower 同时超时 → 同时成为 Candidate → 同时投票给自己 → 无人获得多数 → 重新选举 → 无限循环。

随机化 1.5~3.0s 保证**几乎不可能**同时超时，第一个超时的 Candidate 有足够时间收集投票。

### 为什么 `--cluster-size` 是必需的

Raft 多数派 = `ceil(N/2) + 1`。要算这个需要知道集群总节点数 N。

如果靠 `peer_count + 1` 来算，每个节点的值不一样：
- node0（2 peers）→ N=3
- node1（1 peer）→ N=2
- node2（0 peers）→ N=1

不同 N 导致不同多数派，系统无法收敛。所以必须显式指定 `--cluster-size`。

---

## 后续规划

### ✅ 已完成
- [x] Raft 选举 + 心跳（核心 MVP）
- [x] Leader 向 Master 报备（`MSG_MASTER_LEADER`）
- [x] TCP 可靠性（单缓冲区发送、循环读取、单向连接）
- [x] `clusterctl` 命令行框架
- [x] `deploy` 批量部署工具（SCP + SSH）

### ✅ 已完成
- [x] Raft 选举 + 心跳
- [x] 命令执行（`clusterctl exec`）
- [x] 异步结果聚合（pending_task 追踪）
- [x] Follower 状态采集 + 定时上报
- [x] Master 指令通道（`MSG_MASTER_CMD`）
- [x] 持久化（任务保存到 JSON 文件）
- [x] 单元测试（`make test` 44 个测试）
- [x] TCP 可靠性（原子发送、单向连接、零断连）
- [x] `deploy` 批量部署工具
- [x] `clusterctl` 命令行框架
- [x] XXTEA 加密（`--key` 参数，128-bit key）
- [x] LuaJIT 热更新插件系统
- [x] cjson 支持（Lua 插件可用 `require("cjson")`）

### ⏳ 待实现
- [ ] Webhook / MCP Server 集成
- [ ] Follower 执行结果异步聚合（`on_task_result` → `pending_task_collect`）

---

## License

MIT License
