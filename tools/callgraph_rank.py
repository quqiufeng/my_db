#!/usr/bin/env python3
"""tools/callgraph_rank.py — 从 call_graph.json 计算真实 fan-in / fan-out

call_graph.json 是反向图: { "<callee>": { "calls": [ {"function":"<caller>", ...}, ... ] } }

- fan-in(X)  = X 被多少个**不同**函数调用（= X 作为 key 时 calls 里不同 caller 数）
- fan-out(X) = X 调用了多少个**不同**函数（= X 作为 caller 出现过的不同 callee 数）

真正的"核心/公共接口"看 fan-in；"分发器/大函数"看 fan-out。
（注意：不要把 fan-out 当 fan-in——旧分析里踩过这个坑。）

用法:
  tools/callgraph_rank.py <cache_dir> [--top N]
"""
import argparse
import collections
import json
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cache_dir", help="e.g. /opt/code_caches/linux_723_cache")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    path = os.path.join(args.cache_dir, "call_graph.json")
    with open(path) as f:
        cg = json.load(f)

    fanin = collections.Counter()
    fanout = collections.defaultdict(set)
    for callee, info in cg.items():
        callers = set()
        for e in info.get("calls", []):
            caller = e.get("function")
            if caller:
                callers.add(caller)
                fanout[caller].add(callee)
        fanin[callee] = len(callers)

    print(f"# {path}: {len(cg)} 个 callee")
    print("## TOP FAN-IN （被最多不同函数调用 → 核心/公共接口）")
    for fn, n in fanin.most_common(args.top):
        print(f"  {n:5d}  {fn}")
    print("## TOP FAN-OUT （调用最多不同函数 → 分发器/大函数，预期被 ioctl 类主导）")
    for fn, s in sorted(fanout.items(), key=lambda kv: -len(kv[1]))[: args.top]:
        print(f"  {len(s):5d}  {fn}")


if __name__ == "__main__":
    main()
