#!/bin/bash
#
# tests/validate_tools.sh — 用真实项目校验 code search 工具链
# ============================================================
# 对已索引项目跑一组不变量断言，专门捕捉「只有真数据才触发」的问题：
#   - 向量：.bin 存在、.hnsw 新于 .bin、search top1 分数 > 0.3（防零向量/坏索引）
#   - context：symbol 命中、callees 非空且**不全自指**（防 call_graph 正向表 bug）
#   - dataflow：合法 JSON、变量数 > 阈值、无 void 等噪声键
#
# 用法:
#   tests/validate_tools.sh                 # 跑内置的已知项目（存在才跑）
#   tests/validate_tools.sh <name> <cache_dir> <namespace> <symbol> <query>
#
# 依赖: tools/cache_query, tools/vector_search, python3
#
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CQ="$ROOT/tools/cache_query"
VS="$ROOT/tools/vector_search"

PASS=0
FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

validate() {
    local name="$1" cache="$2" ns="$3" sym="$4" query="$5"
    echo "== $name  ($ns) =="
    if [[ ! -d "$cache" ]]; then echo "  [SKIP] cache not found: $cache"; return; fi

    # 1) 向量 + HNSW 新鲜度
    local bin; bin=$(ls "$cache"/vectors/*.jina.bin 2>/dev/null | head -1)
    if [[ -n "$bin" ]]; then ok ".bin present"; else bad ".bin missing"; fi
    if [[ -n "$bin" && -f "${bin}.hnsw" && "${bin}.hnsw" -nt "$bin" ]]; then
        ok "hnsw newer than .bin"
    else
        bad "hnsw missing or older than .bin"
    fi

    # 2) search 冒烟 + top1 分数（防零向量）
    local top
    top=$("$VS" "$cache" "$query" 3 --json 2>/dev/null \
        | python3 -c "import sys,json;d=json.load(sys.stdin);r=d.get('results',[]);print('%.3f'%r[0]['score'] if r else 'NONE')" 2>/dev/null)
    if [[ "$top" != "NONE" && -n "$top" ]] && python3 -c "import sys;sys.exit(0 if float('$top')>0.3 else 1)"; then
        ok "search top1=$top > 0.3"
    else
        bad "search top1=$top (零向量/坏索引?)"
    fi

    # 3) context 不变量（#8：callees 不全自指）
    local ctx
    ctx=$("$CQ" "$sym" --repo "$ns" --type context --depth 1 2>/dev/null | grep '^{' | head -1)
    if printf '%s' "$ctx" | python3 -c "
import sys,json
sym='$sym'
try: d=json.load(sys.stdin)
except Exception: sys.exit(2)
c=d.get('context',{}); s=c.get('symbol')
if not s: sys.exit(3)
if s.get('name')!=sym: sys.exit(6)
cal=[x.get('name') for x in c.get('callees',[])]
if not cal: sys.exit(4)
if all(n==sym for n in cal): sys.exit(5)
"; then
        ok "context: symbol 命中 + callees 有效(非全自指)"
    else
        bad "context 不变量失败 (exit=$?)"
    fi

    # 4) dataflow 合法 + 无噪声键
    local df="$cache/dataflow.json"
    if [[ -f "$df" ]] && python3 -c "
import json
d=json.load(open('$df'))
assert len(d)>1000, 'too few vars'
assert 'void' not in d and 'return' not in d
" 2>/dev/null; then
        ok "dataflow 合法 (>1000 变量, 无 void/return)"
    else
        bad "dataflow 非法或噪声键"
    fi
}

if [[ $# -ge 5 ]]; then
    validate "$1" "$2" "$3" "$4" "$5"
else
    # 内置已知项目（存在才跑）
    validate "openresty"  "/opt/code_caches/openresty_cache" "/code/openresty" "ngx_http_lua_run_thread" "content handler run lua"
    validate "linux_723"  "/opt/code_caches/linux_723_cache" "/code/linux_723"  "vfs_read"                "virtual file system read"
fi

echo
echo "PASS=$PASS  FAIL=$FAIL"
[[ $FAIL -eq 0 ]]
