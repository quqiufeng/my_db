#! /bin/bash
# tracec generated | target: perf | probes: 1
PID="${PID:-$1}"
if [ -z "$PID" ]; then echo "usage: PID=<pid> bash $0"; exit 1; fi
set -e

# ---- probe 1: offcpu ----
# off-CPU: 追踪进程被调度出去的原因
perf record -e sched:sched_switch -g -p "$PID" --sleep 10 -o perf.data 2>&1

# 按 off-CPU 原因分类统计
echo '=== off-CPU 原因分布 ==='
perf script -i perf.data -F trace:event,trace:prev_state,comm,pid 2>/dev/null | \
  awk '{a[$3]++} END{for(k in a) printf "%5d  %s\n", a[k], k}' | sort -rn | head -10

# 生成 off-CPU 火焰图数据
perf script -i perf.data -F trace:event,trace:prev_state,ip,sym 2>/dev/null | \
  awk -v state="${STATE:-1}" '$2==state' | \
  stackcollapse-perf.pl 2>/dev/null > offcpu.folded 2>&1
if [ -s offcpu.folded ]; then
  flamegraph.pl --color=java offcpu.folded > offcpu.svg 2>&1
  echo "flame graph: offcpu.svg"
fi
echo ''
echo '注: prev_state=1 等I/O, 2 等锁/磁盘, 0 被抢占'

