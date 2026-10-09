#!/usr/bin/env python3
"""tools/ctx.py — 批量 symbol context 查询（cache_query 的 agent 友好封装）

背景：cache_query 输出 JSON，逐条 shell 调用 + 解析既慢又费 context。
本脚本对多个函数批量调用 `cache_query --type context`，只回吐
「符号 + caller/callee 名」，便于 agent 快速建立代码级调用关系。

用法:
  tools/ctx.py <namespace> <func> [func2 ...] [--depth N] [--json] [--max M]

示例:
  tools/ctx.py /code/linux_723 vfs_read do_sys_openat2
  tools/ctx.py /code/openresty ngx_http_lua_run_thread --depth 2
  tools/ctx.py /code/linux_723 call_rcu --json      # 输出原始 JSON（供管道）

依赖: 同目录 cache_query（KV 已导入对应 namespace）。
"""
import argparse
import json
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CQ = os.path.join(SCRIPT_DIR, "cache_query")


def query(ns, fn, depth):
    p = subprocess.run(
        [CQ, fn, "--repo", ns, "--type", "context", "--depth", str(depth)],
        capture_output=True, text=True,
    )
    # cache_query 的 [CACHE] 日志走 stderr；stdout 只有一行 JSON
    line = next((l for l in p.stdout.splitlines() if l.startswith("{")), None)
    return json.loads(line) if line else None


def main():
    ap = argparse.ArgumentParser(description="batch context query via cache_query")
    ap.add_argument("namespace", help="KV namespace, e.g. /code/linux_723")
    ap.add_argument("funcs", nargs="+", help="symbol name(s)")
    ap.add_argument("--depth", type=int, default=1, help="call chain depth (0-5)")
    ap.add_argument("--json", action="store_true", help="emit raw JSON per symbol")
    ap.add_argument("--max", type=int, default=0, help="max callers/callees to print (0=all)")
    args = ap.parse_args()

    if not os.access(CQ, os.X_OK):
        sys.exit(f"cache_query not found/executable: {CQ}")

    for fn in args.funcs:
        d = query(args.namespace, fn, args.depth)
        if not d:
            print(f"{fn}: no result")
            continue
        if args.json:
            print(json.dumps(d, ensure_ascii=False))
            continue

        c = d.get("context", {})
        s = c.get("symbol") or {}
        if not s:
            print(f"{fn}: not found")
            continue

        print(f"■ {s.get('name')}  {s.get('file')}:{s.get('line')}  kind={s.get('kind')}")

        callers = [x.get("name") for x in c.get("callers", [])]
        callees = [x.get("name") for x in c.get("callees", [])]
        nc, nce = len(callers), len(callees)
        if args.max:
            callers = callers[: args.max]
            callees = callees[: args.max]
        if callers:
            print(f"  callers({nc}): {', '.join(callers)}")
        if callees:
            print(f"  callees({nce}): {', '.join(callees)}")


if __name__ == "__main__":
    main()
