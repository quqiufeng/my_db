# Agent — AI 驱动的 Linux 集群控制引擎

> **面向 AI 编程助手的集群控制工具。** 纯 C 语言，一个二进制文件，SCP 部署，零依赖，自组织选举，自动故障恢复。
> 让 AI 像操作一台机器一样操作整个集群——部署、命令执行、文件分发、服务管理，全部通过结构化 JSON 接口完成。

## 设计理念

这个项目的核心定位是 **AI-first（AI优先）**：

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
│   ● 自部署：AI 可以自主完成 scp → ssh → 选举 → 就绪 全流程               │
│   ● 自愈：Leader 挂了 AI 不需要管，集群自动恢复                           │
└──────────────────────────────────────────────────────────────────────────┘
```

**传统工具 vs 本项目：**

| 对比维度 | 传统工具（Ansible/Salt/ssh） | 本项目 |
|---------|------------------------------|--------|
| 用户 | 人类运维工程师 | **AI + 人类** |
| 输出格式 | 人类可读的彩色文本 | **JSON（AI 可直接解析）** |
| 部署方式 | 配 inventory、装 Python 依赖 | **一个二进制 scp 过去就完事** |
| 架构 | 中心化或纯 SSH | **自组织选举，三层控制** |
| 故障处理 | 人工介入 | **自动选举恢复** |

## 架构总览

```
┌──────────────────────────────────────────────────────────────────┐
│                       Master 运维机                              │
│                                                                  │
│   ┌─────────────┐         ① clusterctl -connect 192.168.1.10    │
│   │  clusterctl │──────── TCP 9527 ──────┐                      │
│   │  (CLI/JSON) │                         │ 等待 Leader 报备     │
│   └─────────────┘                         │                      │
│   AI ──shell──► clusterctl                │                      │
│                                          │                      │
│   ④ Leader 报备 → Master 就绪            │                      │
│   ⑤ 下发任务                                │                      │
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
│                       ② 选举                                      │
│   ┌──────────────────┐     ┌──────────────────┐                 │
│   │  Server C        │     │  Server D        │                 │
│   │  ┌────────────┐  │     │  ┌────────────┐  │                 │
│   │  │  Agent     │  │     │  │  Agent     │  │                 │
│   │  │  (Follower)│  │     │  │  (Follower)│  │                 │
│   │  └────────────┘  │     │  └────────────┘  │                 │
│   └──────────────────┘     └──────────────────┘                 │
│                                                                  │
│   ◄── 心跳 + 状态上报 (Follower → Leader)                       │
│   ◄── 对端建连（-ip 参数指定全部对端列表）                        │
│   ◄── 选举投票（随机数 PK）                                       │
│                                                                  │
│   ③ Leader 选出后 → 主动向 Master 报备 "我是 Leader, epoch=N"     │
└──────────────────────────────────────────────────────────────────┘
```

**通信方式一览：**

| 链路 | 协议 | 说明 |
|------|------|------|
| clusterctl → Agent | **TCP 自定义二进制协议** | 先连接，等待 Leader 报备 |
| Leader → Master | TCP 自定义二进制协议 | Leader 选出后主动报备 |
| Leader → Follower | TCP 自定义二进制协议 | 心跳、任务下发、状态聚合 |
| 节点 ↔ 节点 | TCP 自定义二进制协议 | 选举投票 |

整个集群**只用一种协议**、**一个端口**（--port），没有 HTTP、没有 SSH 隧道，简洁统一。

## 核心特性

| 特性 | 说明 |
|------|------|
| **AI 友好** | 所有输出为 JSON，错误码 + 结构化消息，AI 无需解析文本 |
| **纯 C 语言** | C11 标准，静态编译，单个二进制 ≈ 500KB，零运行时依赖 |
| **一键部署** | `deploy -ip 192.168.1.10,192.168.1.11,192.168.1.12`，AI 一条命令拉起整个集群 |
| **零配置发现** | 启动时 `-ip` 参数传入全部对端 IP，Agent 开机即知全部邻居 |
| **Leader 选举** | 随机数 PK 内嵌，集群自组织，无需 etcd/Consul/ZooKeeper |
| **三层控制** | Master ⇄ Leader ⇄ Follower，Master 仅连 Leader，降低连接压力 |
| **Leader 主动报备** | Leader 选出后主动向 Master 报告，Master 等待期间自动阻塞 |
| **状态聚合** | Follower 定时上报状态给 Leader，Master 查询 Leader 即可获取全貌 |
| **高可用** | Leader 故障自动感知 → 重新选举 → 新 Leader 向 Master 报备 → 服务自愈 |
| **统一协议** | 全链路走 TCP 自定义二进制协议，不引入 HTTP 等额外协议栈 |

## AI 使用示例

AI 编程助手只需发一条 shell 命令就能控制整个集群：

```bash
# 部署集群（AI 执行）
./deploy -ip 192.168.1.10,192.168.1.11,192.168.1.12 --user root
# → {"status":"ok","nodes":["192.168.1.10:9527",...,]}

# 连接集群（等待 Leader 报备后返回）
./clusterctl --connect 192.168.1.10:9527 status
# → {"status":"waiting_election"}  ...（Leader 还在选）
# → {"leader":"192.168.1.10:9527","epoch":3,"nodes":[
#      {"id":"node1","role":"leader","load":0.45,"mem_pct":32},
#      {"id":"node2","role":"follower","load":0.12,"mem_pct":28}
#    ]}
# 注：clusterctl 会一直等待直到 Leader 报备或超时

# 在全部机器上执行命令
./clusterctl --connect 192.168.1.10:9527 exec "uptime"
# → {"task_id":"abc123","results":[
#      {"node":"192.168.1.10","exit":0,"stdout":"..."},
#      {"node":"192.168.1.11","exit":0,"stdout":"..."},
#      ...
#    ]}

# 分发配置文件（通过 SCP，集群 ctl 自动获取节点列表）
./clusterctl --connect 192.168.1.10:9527 push /etc/nginx.conf /etc/nginx/

# 重启服务（通过 TCP 协议）
./clusterctl --connect 192.168.1.10:9527 exec "systemctl restart nginx"
```

## 工作流程

### 1. 部署

```bash
# 方式一：批量部署（推荐）
./deploy -ip 192.168.1.10,192.168.1.11,192.168.1.12 --user root

# 方式二：手动部署
scp agent root@192.168.1.10:/tmp/agent && ssh root@192.168.1.10 "/tmp/agent -ip 192.168.1.10,192.168.1.11,192.168.1.12 --port 9527"
scp agent root@192.168.1.11:/tmp/agent && ssh root@192.168.1.11 "/tmp/agent -ip 192.168.1.10,192.168.1.11,192.168.1.12 --port 9527"
scp agent root@192.168.1.12:/tmp/agent && ssh root@192.168.1.12 "/tmp/agent -ip 192.168.1.10,192.168.1.11,192.168.1.12 --port 9527"
```

> 每一台 agent 启动时都通过 `-ip` 知道全部对端是谁，直接开始建连 + 选举。
> agent 启动后自动 daemonize（fork + setsid），SSH 断开后继续运行。

### 2. 集群自组织 + Master 等待报备

```
┌────────────────────────────────────────────────────┐
│  Master（clusterctl）           Agent 集群          │
├────────────────────────────────────────────────────┤
│  ① clusterctl -connect 192.168.1.10:9527           │
│     │                                              │
│     │── TCP 连接 ──────────────→ 任一 Agent        │
│     │                                              │
│     │  ② Agent 们开始选举                           │
│     │     ├── 互相建连                              │
│     │     ├── 随机数 PK                             │
│     │     └── Leader 胜出                           │
│     │                                              │
│     │  ③ Leader ── MSG_MASTER_LEADER ──→ Master    │
│     │     {"leader":"ip:port","epoch":3}           │
│     │                                              │
│     │  ④ Master 收到报备，标记就绪                   │
│     │     "等待中..." → "Leader 已就绪"             │
│     │                                              │
│     │  ⑤ 正常任务下发                               │
│     │     Master ──→ Leader ──→ Follower           │
└────────────────────────────────────────────────────┘

Leader 故障场景：
  ┌── ⑥ Follower 检测到 Leader 超时
  ├── ⑦ 重新随机数 PK 选举
  ├── ⑧ 新 Leader → MSG_MASTER_LEADER → Master（再次报备）
  └── ⑨ Master 更新 Leader 信息，继续下发任务
```

### 3. Master 通信方式

```
clusterctl ──TCP 9527──→ 任一 Agent（先连接，再等报备）
                   Leader 选出后 ── 主动报备 ──→ clusterctl
                   之后所有命令 ──→ Leader
```

- `clusterctl` 启动时连接 `-connect` 指定的任一 Agent IP
- 如果该 Agent 还没选完 Leader，连接保持，等待 `MSG_MASTER_LEADER`
- Leader 选出后主动发送 `MSG_MASTER_LEADER` 给所有已连接的 Master
- Master 收到后才开始接受用户/AI 的输入
- 如果 Leader 挂了，新 Leader 选出来后会再次报备

### 4. 消息流程

```
clusterctl 启动：
  → TCP connect(192.168.1.10:9527)
  → 发送 MSG_MASTER_HELLO
  → 阻塞等待 MSG_MASTER_LEADER
  → {"leader":"192.168.1.10:9527","epoch":3}
  → 就绪，输出 JSON，等待用户/AI 输入

AI 输入命令：
  → clusterctl 发送 MSG_MASTER_CMD
  → Leader 接收，分发给 Follower
  → 聚合结果，返回 MSG_MASTER_RESULT
  → clusterctl 输出 JSON
```

## 模块设计

```
agent/
├── README.md             # 本文档
│
├── src/
│   ├── main.c            # 入口：参数解析 → daemonize → 启动
│   ├── server.c          # TCP 监听 + 连接管理（epoll）
│   ├── peer.c            # 对端管理：连接、重连、保活
│   ├── election.c        # 随机数 PK 选举算法
│   ├── heartbeat.c       # 心跳检测 + 超时判定
│   ├── status.c          # 状态采集 + 上报
│   ├── task.c            # 任务下发 + 执行 + 结果回传
│   ├── protocol.c        # 二进制帧编解码
│   ├── master.c          # clusterctl 主控逻辑（Master 端）
│   │                    #   -connect 等待 Leader 报备
│   │                    #   -exec / status 命令（TCP）
│   ├── deploy.c          # SCP + SSH 批量部署（Master 端）
│   └── json.c            # 轻量 JSON 构造/解析器
│
├── include/
│   ├── agent.h           # 公共类型 + 接口声明
│   ├── protocol.h        # 通信协议定义（二进制消息格式）
│   ├── election.h        # 选举相关结构体
│   ├── task.h            # 任务结构体定义
│   └── peer.h            # 节点信息结构体
│
├── tests/
│   ├── test_election.c   # 选举算法单元测试
│   ├── test_protocol.c   # 协议编解码测试
│   ├── test_heartbeat.c  # 心跳逻辑测试
│   └── test_task.c       # 任务执行测试
│
├── tools/
│   ├── deploy            # 批量部署工具（编译后）
│   └── clusterctl        # 集群控制命令行（编译后）
│
├── Makefile              # 构建脚本
├── .gitignore
└── LICENSE
```

## 通信协议

### 传输层

全链路统一的 TCP 二进制协议：

```
┌──────────────────────────────────────────────────┐
│  Magic(2B) │ Type(1B) │ Flags(1B) │ Len(4B)     │  ← 8B 固定头
├──────────────────────────────────────────────────┤
│  Payload (变长，由 Len 指定)                       │
├──────────────────────────────────────────────────┤
│  Checksum(4B)  CRC32                             │
└──────────────────────────────────────────────────┘
```

- Magic: `0xAG` `0xNT`（"AGENT" 缩写）
- Type: 消息类型（见下表）
- Flags: 控制位（第1位=请求/响应，第2位=需要ACK，等等）
- Len: Payload 长度（不含头、不含校验）
- Payload: JSON 数据（UTF-8）
- Checksum: CRC32（从 Type 到 Payload 末尾）


### 会话上下文（Session Context）

A 启动后自动 daemonize（fork + setsid），脱离 SSH 会话独立运行。

每个 TCP 连接对应一个会话上下文，Agent 在上下文中维护自己的身份和状态，Master 可随时查询。

```c
typedef struct {
    /* ── namespace：集群视角 ── */
    char     leader_addr[64];      /* 当前 Leader 地址 "192.168.1.10:9527" */
    int      epoch;                /* 当前纪元号，每次选举递增 */
    int      cluster_size;         /* 集群节点总数 */

    /* ── self：自身视角 ── */
    char     node_id[64];          /* 节点 ID "node1" */
    char     role[16];             /* "leader" / "follower" */
    char     self_addr[64];        /* 本机监听地址 "192.168.1.10:9527" */

    /* ── status：实时状态（定期更新） ── */
    double   load_1;               /* 1 分钟负载 */
    double   mem_pct;              /* 内存使用率 %% */
    double   cpu_pct;              /* CPU 使用率 %% */
    double   disk_pct;             /* 磁盘使用率 %% */
    long     uptime_sec;           /* 运行时长（秒） */
    int      client_count;         /* 当前连接数 */
} session_ctx_t;
```

Master 发送 `MSG_MASTER_QUERY` 后，Agent 将当前会话上下文序列化为 JSON 返回：

```json
// {"type":"master_status"} 的响应内容
{"type":"master_status",
 "namespace":{"leader":"192.168.1.10:9527","epoch":3,"cluster_size":5},
 "self":{"id":"node2","role":"follower","addr":"192.168.1.11:9527"},
 "status":{"load_1":0.35,"mem_pct":42,"cpu_pct":12,"disk_pct":55,"uptime_sec":277200,"clients":3}}
```

### 消息类型

| 消息类型 | 值 | 方向 | 用途 |
|---------|-----|------|------|
| `MSG_HEARTBEAT` | 0x01 | Follower → Leader | 心跳 + 状态数据 |
| `MSG_HEARTBEAT_ACK` | 0x02 | Leader → Follower | 心跳确认 |
| `MSG_VOTE` | 0x03 | 节点 → 节点 | 投票（node_id + 随机数） |
| `MSG_COORD` | 0x04 | 胜出者 → 所有人 | 宣布新 Leader |
| `MSG_OK` | 0x05 | 节点 → 节点 | 确认接受 |
| `MSG_TASK_DISPATCH` | 0x10 | Leader → Follower | 下发任务 |
| `MSG_TASK_RESULT` | 0x11 | Follower → Leader | 任务结果 |
| `MSG_STATUS_REPORT` | 0x12 | Follower → Leader | 状态上报 |
| `MSG_MASTER_HELLO` | 0x20 | Master → Agent | Master 连接声明 |
| `MSG_MASTER_LEADER` | 0x21 | **Leader → Master** | **Leader 主动报备** |
| `MSG_MASTER_CMD` | 0x22 | Master → Leader | Master 下发指令 |
| `MSG_MASTER_QUERY` | 0x24 | Master → 任一 Agent | 查询对方身份 + 状态 |
| `MSG_MASTER_STATUS` | 0x25 | Agent → Master | 返回身份 + 状态 |
| `MSG_MASTER_RESULT` | 0x23 | Leader → Master | 任务执行结果 |

### Payload 格式（JSON）

```json
// 心跳
{"type":"heartbeat","id":"node2","ts":1712345678,"load":0.35,"mem_pct":42}

// 投票
{"type":"vote","id":"node1","val":7,"epoch":1}

// 宣布 Leader
{"type":"coord","leader":"192.168.1.10:9527","epoch":1}

// Master 连接声明

// Master 查询对方身份 + 状态
{"type":"master_query"}

// Agent 返回当前身份 + 状态 ← 每个 TCP 会话自带
{"type":"master_status","id":"node1","role":"follower","epoch":3,
 "leader":"192.168.1.10:9527",
 "load":0.35,"mem_pct":42,"cpu_pct":12,"disk_pct":55,
 "uptime":"3d 2h","clients":5}
{"type":"master_hello","version":"1.0"}

// Leader 主动报备 ← 关键消息
{"type":"master_leader","leader":"192.168.1.10:9527","epoch":3,
 "nodes":[{"id":"node1","addr":"192.168.1.10:9527"},
          {"id":"node2","addr":"192.168.1.11:9527"}]}

// Master 下发命令
{"type":"master_cmd","action":"exec","cmd":"uptime","target":"all","timeout":30}

// Master 查询结果
{"type":"master_result","task_id":"t_001","status":"completed",
 "results":[
   {"node":"192.168.1.10","exit":0,"stdout":"..."},
   {"node":"192.168.1.11","exit":0,"stdout":"..."}
 ]}
```

## 选举算法：随机数 PK

采用最简方案，每轮产生随机数（0-10），最大的胜出：

```text
1. 选举触发条件：
   a. 首次启动，所有节点互相建连完毕
   b. Leader 心跳超时（连续 N 次未收到）

2. 每轮选举：
   a. 每个节点生成一个 0-10 的随机数
   b. 向所有对端发送 MSG_VOTE {id, val, epoch}
   c. 等待收集所有对端的投票（超时机制兜底）

3. 判定：
   a. 收集完毕后，找出随机数最大的节点
   b. 如果唯一最大 → 该节点广播 MSG_COORD，当选 Leader
   c. 如果有并列最大（平局）→ 平局节点进入下一轮，重新生成随机数
   d. 下一轮中，非平局节点不再参与

4. Leader 报备：
   a. 新 Leader 向所有已连接的 Master 发送 MSG_MASTER_LEADER
   b. Master 更新 Leader 信息，标记就绪
```

**示例（3 节点）：**

```text
节点A ─── 随机数 7 ───┐
节点B ─── 随机数 3 ───┤── 最大是 7 → A 当选 Leader
节点C ─── 随机数 5 ───┘
                       └──→ A 向 Master 报备 "我是 Leader"

平局场景：
节点A ─── 随机数 7 ───┐
节点B ─── 随机数 7 ───┤── 平局！A、B 进入下一轮
节点C ─── 随机数 2 ───┘

第二轮：
节点A ─── 随机数 4 ───┐
节点B ─── 随机数 9 ───┤── 最大是 9 → B 当选 Leader
（节点C 不参与）  ────┘
                       └──→ B 向 Master 报备 "我是 Leader"
```

## 容错机制

### 选举失败场景

| 问题 | 场景 | 处理方式 |
|------|------|----------|
| **无限平局** | 两节点连续多轮随机数相同 | 最大轮次限制（默认 5 轮），超过则轮空一轮再重试 |
| **节点掉线** | 投票过程中有节点宕机 | 投票超时（默认 3s），超时后以已收投票为准，未投票节点视为弃权；
  如果 Leader 已经选出但掉线节点恢复，它自动接受当前 Leader |
| **分区脑裂** | 网络断开形成两个子集群，各自选出 Leader | 引入**纪元号 + 多数派原则**：Leader 必须获得超过半数的投票才能当选。
  子集群节点数不足半数时选举失败，等待网络恢复；
  网络恢复后 Epoch 大的 Leader 胜出，小的自动降级 |
| **旧消息干扰** | 上一轮选举的延迟消息到达新选举中 | 每条消息携带 Epoch 号，接收方只处理等于当前 Epoch 的消息 |
| **同时选举** | 多个节点同时发现 Leader 超时，各自发起选举 | 随机数 PK 天然解决——不管谁发起的，最终按随机数定胜负；
  加上选举超时随机化（1-3s），降低同时发起概率 |

### 运行时容错

| 问题 | 场景 | 处理方式 |
|------|------|----------|
| **Leader 刚当选就崩溃** | 发送 MSG_COORD 后、MSG_MASTER_LEADER 前宕机 | Follower 心跳超时感知 → 重新选举；
  新 Leader 产生后向 Master 报备，Master 无缝切换 |
| **Master 断连** | clusterctl 进程退出或被控机重启 | Agent 端清理 Master 会话，继续正常运行；
  Master 重连后重新等待 Leader 报备即可 |
| **Follower 连续超时** | 某节点负载过高，心跳延迟 | Leader 累计 N 次未收到该节点心跳，标记为 OFFLINE，
  但仍保留连接；上报 Master 时标记该节点状态为 "unreachable" |
| **Follower 死而复生** | 宕机后恢复，重启 agent | 新 agent 启动，连接对端；通过 `-ip` 参数重新加入集群；
  当前 Leader 检测到新节点，更新成员列表 |
| **Leader 抖动** | 网络短暂中断，Leader 被误判宕机后又恢复 | 连续 N 次心跳超时才触发选举（N≥3），避免单次丢包误判；
  如果 Leader 实际上未宕机，新选举期间原 Leader 仍在运行，
  两边以 Epoch 号大的为准 |
| **时钟不同步** | 各节点时间不一致 | 选举和时间戳不依赖系统时钟，
  epoch 是本地自增计数器，心跳间隔用相对时间（monotonic clock） |
| **全部节点同时宕机** | 机房断电等灾难性故障 | 所有 agent 停止，重新部署后从零开始选举；
  如果任务已持久化到磁盘，新 Leader 可恢复未完成任务 |
| **重复 Master 连接** | 多个 clusterctl 同时连集群 | 每个连接独立管理，Leader 可以同时服务多个 Master；
  每个 Master 都收到 Leader 报备，各自可独立下发任务 |

### Master 端容错

| 行为 | 说明 |
|------|------|
| 连接超时 | clusterctl 连接 `-connect` 地址超时（默认 10s），输出 `{"error":"connect timeout"}`，退出码 2 |
| 等待报备超时 | 连接成功但 Leader 长时间未选出（默认 30s），输出 `{"status":"timeout","msg":"no leader elected"}`，退出码 2 |
| Leader 切换 | 如果当前 Leader 断开，clusterctl 自动重新等待新 Leader 的 MSG_MASTER_LEADER，
  中间若用户输入命令，返回 `{"error":"leader lost, waiting for re-election"}` |
| 部分节点失败 | `exec` 等任务如果部分节点超时/失败，不影响其他节点结果，
  返回结果中每个节点独立携带 `exit`/`error` 字段 |
### 编译

```bash
cd agent
make                # 编译 agent 二进制
make tools          # 编译 deploy / clusterctl
make test           # 运行单元测试
```

### 本地模拟 3 节点集群

```bash
# 终端 1
./agent --port 9527 --id node1 -ip 127.0.0.1:9527,127.0.0.1:9528,127.0.0.1:9529

# 终端 2
./agent --port 9528 --id node2 -ip 127.0.0.1:9527,127.0.0.1:9528,127.0.0.1:9529

# 终端 3
./agent --port 9529 --id node3 -ip 127.0.0.1:9527,127.0.0.1:9528,127.0.0.1:9529

# 终端 4 — Master 连接（会自动等待 Leader 报备）
./clusterctl --connect 127.0.0.1:9527 status
./clusterctl --connect 127.0.0.1:9527 exec "uptime"
```

> **注意**：实际部署时 `-ip` 只需传 IP 列表（如 `-ip 192.168.1.10,192.168.1.11,192.168.1.12`），端口统一由 `--port` 指定。
> 本地模拟因多节点同机，需要用 `ip:port` 格式区分。

### 部署到远程集群

```bash
# 批量部署（需要 SSH 密钥）
./deploy -ip 192.168.1.10,192.168.1.11,192.168.1.12 --user root

# 连接并等待自动就绪
./clusterctl --connect 192.168.1.10:9527 status
```

## 支持的任务类型

| 任务类型 | 说明 | 示例 |
|---------|------|------|
| `exec` | 执行 shell 命令 | `{"cmd":"uptime"}` |
| `push` | scp 分发文件到节点 | `{"src":"...","dst":"..."}`（走 SCP，不走 TCP） |
| `pull` | scp 从节点拉取文件 | `{"src":"...","dst":"..."}`（走 SCP，不走 TCP） |
| `service` | 管理 systemd 服务 | `{"action":"restart","name":"nginx"}` |
| `monitor` | 采集系统指标 | `{"metrics":["cpu","mem","disk","net"]}` |
| `script` | 上传并执行脚本 | `{"content":"#!/bin/bash\n...","timeout":60}` |

## AI 集成规范

本工具设计为 AI 编程助手的"集群操作外设"。AI 使用时应遵循：

1. **所有命令标准输出均为 JSON**，stderr 仅输出人类可读的日志
2. **退出码含义**：0=成功，1=参数错误，2=连接失败，3=部分节点失败
3. **超时机制**：每个任务有 `timeout` 参数，避免 AI 发起的任务 hang 住
4. **幂等操作**：部署、服务管理类的操作设计为幂等，AI 可安全重试
5. **错误可恢复**：节点暂时不可达时返回 `{"node":"x","error":"timeout"}`，不阻塞整个任务
6. **等待报备**：`clusterctl` 连接后自动等待 Leader 报备，AI 无须额外轮询

## 技术栈

- **语言**：C11（严格 ANSI C 兼容子集）
- **网络**：epoll（边缘触发，单线程事件驱动）
- **协议**：自定义二进制帧头 + JSON payload，全链路统一
- **序列化**：手写轻量 JSON 构造器（零依赖）
- **构建**：GNU Make + gcc/clang
- **测试**：minunit（纯 C 单元测试框架）

## 后续规划

- [ ] 选举 + 心跳（核心 MVP）
- [ ] Leader 向 Master 报备
- [ ] 状态采集 + 上报
- [ ] 任务分发 + 结果聚合
- [ ] `deploy` 批量部署工具
- [ ] `clusterctl` 命令行（JSON 输出）
- [ ] 任务队列持久化（嵌入式 SQLite）
- [ ] Tail -f 模式：实时流式查看所有节点日志
- [ ] TLS 1.3 加密通信
- [ ] 文件分发进度反馈（用于 AI 估算剩余时间）
- [ ] Webhook / MCP Server 集成（AI 原生接口）

## License

MIT License
