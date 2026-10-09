#!/bin/bash
#
# tools/deploy.sh — 本地编译并 scp 二进制到远程（配合 coding.md 的固定部署流程）
#
# 用法:
#   DEPLOY_HOST=<host> [DEPLOY_PORT=22] [DEPLOY_PW=<pw>] [DEPLOY_DEST=/opt/my_db/] \
#       ./tools/deploy.sh tools/vector_search tools/cache_query ai_code_search.sh
#
# 说明:
#   - 本地 make 目标后 scp；不传 DEPLOY_PW 则用 ssh 密钥。
#   - 远程 /opt/my_db 不是 git clone，故只传二进制（源码历史走 GitHub）。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${DEPLOY_HOST:?请设置 DEPLOY_HOST}"
DEPLOY_PORT="${DEPLOY_PORT:-22}"
DEPLOY_DEST="${DEPLOY_DEST:-/opt/my_db/}"

[[ $# -ge 1 ]] || { echo "用法: DEPLOY_HOST=<host> $0 <目标1> [目标2 ...]"; exit 1; }

# 二进制目标先 make
for t in "$@"; do
    if [[ "$t" != *.sh ]]; then
        echo "==> make $t"
        make -C "$ROOT" "$t"
    fi
done

# scp
for t in "$@"; do
    local_src="$ROOT/$t"
    base="$(basename "$t")"
    dest="${DEPLOY_DEST%/}/$base"
    if [[ "$t" == tools/* ]]; then
        dest="${DEPLOY_DEST%/}/tools/$base"
    fi
    echo "==> scp $local_src -> ${DEPLOY_HOST}:${dest}"
    if [[ -n "${DEPLOY_PW:-}" ]]; then
        sshpass -p "$DEPLOY_PW" scp -P "$DEPLOY_PORT" "$local_src" "root@${DEPLOY_HOST}:${dest}"
    else
        scp -P "$DEPLOY_PORT" "$local_src" "root@${DEPLOY_HOST}:${dest}"
    fi
done
echo "==> done"
