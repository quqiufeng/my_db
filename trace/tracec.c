/*
 * tracec.c — 探针 DSL 编译器
 *
 * 将 .trace 探针描述编译为 perf/SystemTap/eBPF 等后端追踪脚本。
 *
 * 用法:
 *   tracec detect                   # 列出可用后端
 *   tracec compile input.trace      # 编译为 Shell 脚本 (自动选后端)
 *   tracec compile input.trace --target perf
 *   tracec compile input.trace --target stap
 *
 * 构建:
 *   gcc -o tracec tracec.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================================================
 * 辅助函数
 * ============================================================ */

// 检查命令是否存在 (不启动 shell)
static int cmd_avail(const char *cmd) {
    char buf[1024];
    // 搜索 PATH
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    char *save;
    char *dir = strtok_r(copy, ":", &save);
    while (dir) {
        snprintf(buf, sizeof(buf), "%s/%s", dir, cmd);
        if (access(buf, X_OK) == 0) { free(copy); return 1; }
        dir = strtok_r(NULL, ":", &save);
    }
    free(copy);
    return 0;
}

// 读取整个文件到字符串 (调用者 free, 兼容 pipe)
static char *read_file(const char *path, long *out_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return NULL; }

    // 尝试 fseek/ftell (仅普通文件)
    long len = 0;
    if (fseek(fp, 0, SEEK_END) == 0) {
        len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
    }

    // 管道模式: 逐块读取直到 EOF
    size_t cap = len > 0 ? (size_t)len + 1 : 65536;
    char *buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }

    size_t total = 0;
    size_t n;
    while ((n = fread(buf + total, 1, cap - total - 1, fp)) > 0) {
        total += n;
        if (total >= cap - 1) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(fp); return NULL; }
            buf = nb;
        }
    }
    buf[total] = '\0';
    fclose(fp);
    if (out_len) *out_len = (long)total;
    return buf;
}

/* ============================================================
 * JSON 输出 schema
 * ============================================================
 *
 * {
 *   "probe_file": "test.trace",
 *   "target": "perf",
 *   "pid": 31274,
 *   "duration_sec": 60,
 *   "probes": [
 *     {
 *       "name": "lj_gc_step",
 *       "type": "function",
 *       "results": [
 *         {
 *           "function": "lj_gc_step",
 *           "file": "lj_gc.c",
 *           "line": 724,
 *           "call_count": 142000,
 *           "duration_ns": { "avg": 2340, "p50": 2100, "p90": 4500, "p99": 12000 },
 *           "callers": { "lj_gc_step_jit": 85200, "lua_pcall": 42600 },
 *           "backtrace": [ "lj_gc_step", "lj_gc_step_jit", "lj_asm_trace" ]
 *         }
 *       ]
 *     }
 *   ]
 * }
 */

/* ============================================================
 * DSL 语法定义 (简单递归下降解析器)
 * ============================================================
 *
 * probe     := "probe" target "{" { clause } "}"
 * target    := "cpu" | string
 * string    := '"' { char } '"'
 * clause    := "freq" number
 *            | "duration" number
 *            | "collect" "(" field [ ":" agg ] ")"
 *            | "on" ("enter" | "exit")
 *            | "when" field ">" number
 * field     := ident
 * agg       := "hist" | "sum" | "avg" | "max"
 * number    := digit { digit }
 */

/* ============================================================
 * 数据结构
 * ============================================================ */
#define MAX_COLLECTS 16

typedef enum { COLLECT_CALL_COUNT, COLLECT_DURATION, COLLECT_BYTES,
               COLLECT_CALLER, COLLECT_BACKTRACE, COLLECT_ARG, COLLECT_RETURN } CollectType;

typedef enum { AGG_NONE, AGG_HIST, AGG_SUM, AGG_AVG, AGG_MAX } AggType;

typedef struct {
    CollectType type;
    AggType agg;
    int arg_index;
    int backtrace_depth;
} CollectItem;

typedef struct {
    char target[256];        // "cpu" / 函数名
    char library[256];       // 可选 lib 路径
    int freq;
    int duration_sec;
    CollectItem collects[MAX_COLLECTS];
    int collect_count;
    int has_when_field;
    char when_field[64];
    int when_op;            // 0=>
    long long when_value;
    int phase;              // 0=enter, 1=exit, -1=default
} Probe;

typedef struct {
    Probe probes[32];
    int probe_count;
} TraceScript;

/* ============================================================
 * DSL 解析器 (递归下降)
 * ============================================================ */
static const char *g_input;
static int g_line;

static int next_char(void) {
    return (unsigned char)*g_input;
}

static void skip_spaces(void) {
    while (*g_input) {
        if (*g_input == ' ' || *g_input == '\t' || *g_input == '\r') { g_input++; }
        else if (*g_input == '\n') { g_input++; g_line++; }
        else if (*g_input == '/' && g_input[1] == '/') {
            g_input += 2;
            while (*g_input && *g_input != '\n') g_input++;
        }
        else if (*g_input == '/' && g_input[1] == '*') {
            g_input += 2;
            while (*g_input && !(*g_input == '*' && g_input[1] == '/')) {
                if (*g_input == '\n') g_line++;
                g_input++;
            }
            if (*g_input) g_input += 2;
        }
        else break;
    }
}

static int expect_char(char c) {
    skip_spaces();
    if (next_char() != c) return -1;
    g_input++;
    return 0;
}

static int parse_number(long long *val) {
    skip_spaces();
    if (!isdigit(next_char())) return -1;
    *val = 0;
    while (isdigit(next_char())) {
        *val = *val * 10 + (next_char() - '0');
        g_input++;
    }
    return 0;
}

static int parse_ident(char *buf, int maxlen) {
    skip_spaces();
    int i = 0;
    while (isalnum(next_char()) || next_char() == '_') {
        if (i < maxlen - 1) buf[i++] = next_char();
        g_input++;
    }
    buf[i] = '\0';
    return i > 0 ? 0 : -1;
}

static int parse_string(char *buf, int maxlen) {
    skip_spaces();
    if (expect_char('"') != 0) return -1;
    int i = 0;
    while (next_char() != '"' && *g_input) {
        if (i < maxlen - 1) buf[i++] = next_char();
        g_input++;
    }
    buf[i] = '\0';
    if (expect_char('"') != 0) return -1;
    return 0;
}

static int parse_field(char *buf) {
    return parse_ident(buf, 64);
}

static int parse_agg(AggType *agg) {
    char name[32];
    if (expect_char(':') != 0) { *agg = AGG_NONE; return 0; }
    if (parse_ident(name, sizeof(name)) != 0) return -1;
    if (strcmp(name, "hist") == 0) *agg = AGG_HIST;
    else if (strcmp(name, "sum") == 0) *agg = AGG_SUM;
    else if (strcmp(name, "avg") == 0) *agg = AGG_AVG;
    else if (strcmp(name, "max") == 0) *agg = AGG_MAX;
    else return -1;
    return 0;
}

static int parse_collect(Probe *p) {
    if (expect_char('(') != 0) return fprintf(stderr, "line %d: expected '('\n", g_line), -1;

    char field[64];
    if (parse_field(field) != 0) return fprintf(stderr, "line %d: expected collect field\n", g_line), -1;

    CollectItem c = {0};
    c.agg = AGG_NONE;

    if (strcmp(field, "call_count") == 0) c.type = COLLECT_CALL_COUNT;
    else if (strcmp(field, "duration_ns") == 0) { c.type = COLLECT_DURATION; parse_agg(&c.agg); }
    else if (strcmp(field, "bytes") == 0) { c.type = COLLECT_BYTES; parse_agg(&c.agg); }
    else if (strcmp(field, "caller") == 0) c.type = COLLECT_CALLER;
    else if (strcmp(field, "backtrace") == 0) {
        c.type = COLLECT_BACKTRACE;
        c.backtrace_depth = 0;
        // 可选: backtrace: depth N
        skip_spaces();
        if (next_char() == ':') {
            g_input++;
            char kw[16];
            parse_ident(kw, sizeof(kw));
            if (strcmp(kw, "depth") == 0) {
                long long v = 0;
                parse_number(&v);
                c.backtrace_depth = (int)v;
            }
        }
    }
    else if (strncmp(field, "arg", 3) == 0 && isdigit(field[3])) {
        c.type = COLLECT_ARG;
        c.arg_index = atoi(field + 3);
    }
    else if (strcmp(field, "return") == 0) c.type = COLLECT_RETURN;
    else return fprintf(stderr, "line %d: unknown collect field '%s'\n", g_line, field), -1;

    if (expect_char(')') != 0) return -1;
    if (p->collect_count < MAX_COLLECTS) p->collects[p->collect_count++] = c;
    return 0;
}

static int parse_target(Probe *p) {
    skip_spaces();
    if (strncmp(g_input, "cpu", 3) == 0 && !isalnum(g_input[3])) {
        strcpy(p->target, "cpu");
        g_input += 3;
        return 0;
    }
    if (strncmp(g_input, "offcpu", 6) == 0 && !isalnum(g_input[6])) {
        strcpy(p->target, "offcpu");
        g_input += 6;
        return 0;
    }
    return parse_string(p->target, sizeof(p->target));
}

static int parse_probe(Probe *p) {
    memset(p, 0, sizeof(Probe));
    p->freq = 99;
    p->duration_sec = 60;
    p->phase = -1;
    if (parse_target(p) != 0) return -1;

    // 可选的 when 子句, 在 { 之前
    {
        char kw[64];
        skip_spaces();
        if (strncmp(g_input, "when", 4) == 0 && !isalnum(g_input[4])) {
            g_input += 4;
            parse_field(p->when_field);
            skip_spaces();
            if (next_char() == '>') { p->when_op = 0; g_input++; }
            parse_number(&p->when_value);
            p->has_when_field = 1;
        }
    }

    if (expect_char('{') != 0) return -1;

    while (next_char() && next_char() != '}') {
        char kw[64];
        if (parse_ident(kw, sizeof(kw)) != 0) break;

        if (strcmp(kw, "freq") == 0) {
            { long long v = 99; parse_number(&v); p->freq = (int)v; }
        } else if (strcmp(kw, "duration") == 0) {
            long long v = 0;
            parse_number(&v);
            p->duration_sec = (int)v;
        } else if (strcmp(kw, "collect") == 0) {
            parse_collect(p);
        } else if (strcmp(kw, "on") == 0) {
            char phase[16];
            parse_ident(phase, sizeof(phase));
            if (strcmp(phase, "enter") == 0) p->phase = 0;
            else if (strcmp(phase, "exit") == 0) p->phase = 1;
        }
    }
    if (expect_char('}') != 0) return -1;
    return 0;
}

static int parse_trace(TraceScript *s, const char *input) {
    memset(s, 0, sizeof(TraceScript));
    g_input = input;
    g_line = 1;

    while (*g_input) {
        skip_spaces();
        if (!*g_input) break;

        char kw[64];
        if (parse_ident(kw, sizeof(kw)) != 0) {
            fprintf(stderr, "line %d: expected 'probe'\n", g_line);
            return -1;
        }
        if (strcmp(kw, "probe") != 0) {
            fprintf(stderr, "line %d: expected 'probe', got '%s'\n", g_line, kw);
            return -1;
        }
        if (s->probe_count >= 32) {
            fprintf(stderr, "too many probes (max 32)\n");
            return -1;
        }
        if (parse_probe(&s->probes[s->probe_count]) != 0) return -1;
        s->probe_count++;
    }
    return 0;
}

/* ============================================================
 * perf 后端生成
 * ============================================================ */
static void gen_perf_probe(FILE *out, const Probe *p) {
    if (strcmp(p->target, "cpu") == 0) {
        fprintf(out, "perf record -F %d -g -p \"$PID\" --sleep %d -o perf.data 2>&1\n", p->freq, p->duration_sec);
        fprintf(out, "echo ''\n");
        fprintf(out, "echo '=== CPU TOP 20 ==='\n");
        fprintf(out, "perf report -i perf.data -n --stdio 2>/dev/null | head -30\n");
    } else if (strcmp(p->target, "offcpu") == 0) {
        int dur = p->duration_sec > 0 ? p->duration_sec : 60;
        fprintf(out, "# off-CPU: 追踪进程被调度出去的原因\n");
        fprintf(out, "perf record -e sched:sched_switch -g -p \"$PID\" --sleep %d -o perf.data 2>&1\n\n", dur);
        fprintf(out, "# 按 off-CPU 原因分类统计\n");
        fprintf(out, "echo '=== off-CPU 原因分布 ==='\n");
        fprintf(out, "perf script -i perf.data -F trace:event,trace:prev_state,comm,pid 2>/dev/null | \\\n");
        fprintf(out, "  awk '{a[$3]++} END{for(k in a) printf \"%%5d  %%s\\n\", a[k], k}' | sort -rn | head -10\n\n");
        fprintf(out, "# 生成 off-CPU 火焰图数据\n");
        fprintf(out, "perf script -i perf.data -F trace:event,trace:prev_state,ip,sym 2>/dev/null | \\\n");
        fprintf(out, "  awk -v state=\"${STATE:-1}\" '$2==state' | \\\n");
        fprintf(out, "  stackcollapse-perf.pl 2>/dev/null > offcpu.folded 2>&1\n");
        fprintf(out, "if [ -s offcpu.folded ]; then\n");
        fprintf(out, "  flamegraph.pl --color=java offcpu.folded > offcpu.svg 2>&1\n");
        fprintf(out, "  echo \"flame graph: offcpu.svg\"\n");
        fprintf(out, "fi\n");
        fprintf(out, "echo ''\n");
        fprintf(out, "echo '注: prev_state=1 等I/O, 2 等锁/磁盘, 0 被抢占'\n");
    } else {
        // 函数探针: 动态添加 uprobe
        fprintf(out, "perf probe -x /proc/\"$PID\"/exe \"%s\" 2>&1\n", p->target);
        fprintf(out, "perf record -e probe:%s -g -p \"$PID\" --sleep %d -o perf.data 2>&1\n", p->target, p->duration_sec);
        fprintf(out, "perf report -i perf.data -n --stdio 2>/dev/null | head -30\n");
        fprintf(out, "perf probe --del \"%s\" 2>/dev/null\n", p->target);
    }
}

/* ============================================================
 * SystemTap 后端生成
 * ============================================================ */
static void gen_stap_probe(FILE *out, const Probe *p) {
    if (strcmp(p->target, "cpu") == 0) {
        fprintf(out, "probe profile-%d.hz\n", p->freq);
        fprintf(out, "{\n");
        fprintf(out, "  printf(\"%%s\\n\", usymname(ubacktrace()));\n");
        fprintf(out, "}\n");
        return;
    }
    if (strcmp(p->target, "offcpu") == 0) {
        fprintf(out, "probe scheduler.ctxswitch\n");
        fprintf(out, "{\n");
        fprintf(out, "  printf(\"pid:%%d -> %%d state:%%d\\n\", prev_pid, next_pid, task_state(prev_task));\n");
        fprintf(out, "}\n");
        return;
    }

    // 函数探针
    fprintf(out, "probe process.function(\"%s\").call\n", p->target);
    fprintf(out, "{\n");
    for (int j = 0; j < p->collect_count; j++) {
        switch (p->collects[j].type) {
            case COLLECT_CALLER:
                fprintf(out, "  printf(\"caller: %%s\\n\", usymname(ubacktrace()[1]));\n");
                break;
            case COLLECT_ARG:
                fprintf(out, "  printf(\"arg%d: %%d\\n\", $arg%d);\n", p->collects[j].arg_index, p->collects[j].arg_index);
                break;
            case COLLECT_DURATION:
                fprintf(out, "  t = gettimeofday_ns();\n");
                break;
            default:
                break;
        }
    }
    fprintf(out, "}\n");

    // return probe for duration
    int need_return = 0;
    for (int j = 0; j < p->collect_count; j++) {
        if (p->collects[j].type == COLLECT_DURATION || p->collects[j].type == COLLECT_RETURN)
            need_return = 1;
    }
    if (need_return) {
        fprintf(out, "probe process.function(\"%s\").return\n", p->target);
        fprintf(out, "{\n");
        for (int j = 0; j < p->collect_count; j++) {
            if (p->collects[j].type == COLLECT_DURATION)
                fprintf(out, "  printf(\"duration_ns: %%d\\n\", gettimeofday_ns() - t);\n");
            if (p->collects[j].type == COLLECT_RETURN)
                fprintf(out, "  printf(\"return: %%x\\n\", $return);\n");
        }
        fprintf(out, "}\n");
    }
}

/* ============================================================
 * eBPF (bpftrace) 后端生成
 * ============================================================ */
static void gen_ebpf_probe(FILE *out, const Probe *p) {
    if (strcmp(p->target, "cpu") == 0) {
        fprintf(out, "profile:hz:%d\n", p->freq);
        fprintf(out, "{\n");
        fprintf(out, "  @[ustack] = count();\n");
        fprintf(out, "}\n");
        return;
    }
    if (strcmp(p->target, "offcpu") == 0) {
        fprintf(out, "tracepoint:sched:sched_switch\n");
        fprintf(out, "{\n");
        fprintf(out, "  @reason[args->prev_state] = count();\n");
        fprintf(out, "  @[kstack, args->prev_state] = count();\n");
        fprintf(out, "}\n");
        return;
    }
    // 函数探针
    fprintf(out, "uprobe:/proc/\"$PID\"/exe:\"%s\"\n", p->target);
    fprintf(out, "{\n");
    for (int j = 0; j < p->collect_count; j++) {
        switch (p->collects[j].type) {
            case COLLECT_CALL_COUNT:
                fprintf(out, "  @count[\"%s\"] = count();\n", p->target);
                break;
            case COLLECT_CALLER:
                fprintf(out, "  @caller[\"%s\"] = ustack(2);\n", p->target);
                break;
            case COLLECT_BACKTRACE:
                fprintf(out, "  @stack[\"%s\"] = ustack();\n", p->target);
                break;
            case COLLECT_DURATION:
                fprintf(out, "  @ts[\"%s\", tid] = nsecs;\n", p->target);
                break;
            case COLLECT_ARG:
                fprintf(out, "  @arg%d[\"%s\"] = arg%d;\n", p->collects[j].arg_index, p->target, p->collects[j].arg_index);
                break;
            case COLLECT_BYTES:
                fprintf(out, "  @bytes[\"%s\"] += arg0;\n", p->target);
                break;
            default:
                break;
        }
    }
    fprintf(out, "}\n");
    // 出口探针
    int need_return = 0;
    for (int j = 0; j < p->collect_count; j++)
        if (p->collects[j].type == COLLECT_DURATION || p->collects[j].type == COLLECT_RETURN) need_return = 1;
    if (need_return) {
        fprintf(out, "uretprobe:/proc/\"$PID\"/exe:\"%s\"\n", p->target);
        fprintf(out, "{\n");
        fprintf(out, "  $dur = nsecs - @ts[\"%s\", tid];\n", p->target);
        fprintf(out, "  @dur_hist[\"%s\"] = hist($dur);\n", p->target);
        fprintf(out, "  delete(@ts[\"%s\", tid]);\n", p->target);
        fprintf(out, "}\n");
    }
}

/* ============================================================
 * DTrace 后端生成 (macOS/FreeBSD)
 * ============================================================ */
static void gen_dtrace_probe(FILE *out, const Probe *p) {
    if (strcmp(p->target, "cpu") == 0) {
        fprintf(out, "profile-99hz\n");
        fprintf(out,"/ { @[ustack()] = count(); }\n");
        return;
    }
    if (strcmp(p->target, "offcpu") == 0) {
        fprintf(out, "sched:::\n");
        fprintf(out,"/ { @[ustack()] = count(); }\n");
        return;
    }
    fprintf(out, "pid$1::%s:entry\n", p->target);
    fprintf(out,"/ { @[ustack()] = count(); }\n");

    int need_return = 0;
    for (int j = 0; j < p->collect_count; j++)
        if (p->collects[j].type == COLLECT_DURATION || p->collects[j].type == COLLECT_RETURN) need_return = 1;
    if (need_return) {
        fprintf(out, "pid$1::%s:return\n", p->target);
        fprintf(out, "/ { @[ustack()] = count(); }\n");
    }
}

/* ============================================================
 * GDB Python 后端生成 (兜底)
 * ============================================================ */
static void gen_gdb_probe(FILE *out, const Probe *p) {
    // 生成 bash 包装脚本: 将 Python 写入临时文件后调用 gdb
    fprintf(out, "GDB_PID=\"${PID:-$1}\"\n");
    fprintf(out, "if [ -z \"$GDB_PID\" ]; then echo \"usage: PID=<pid> bash $0\"; exit 1; fi\n");
    fprintf(out, "DUR=%d\n", p->duration_sec > 0 ? p->duration_sec : 60);
    fprintf(out, "PY=$(mktemp /tmp/tracec_gdb.XXXXXX.py)\n");
    fprintf(out, "cat > \"$PY\" << 'GDBPYEOF'\n");
    fprintf(out, "import gdb, time, sys\n");
    fprintf(out, "from collections import Counter\n");
    fprintf(out, "frames = []\n");
    fprintf(out, "pid = gdb.selected_inferior().pid\n");
    fprintf(out, "for i in range(%d):\n", p->duration_sec > 0 ? p->duration_sec : 60);
    fprintf(out, "  try:\n");
    fprintf(out, "    frame = gdb.selected_thread().frame()\n");
    fprintf(out, "    stack = []\n");
    fprintf(out, "    while frame:\n");
    fprintf(out, "      sal = frame.find_sal()\n");
    fprintf(out, "      if sal.symtab:\n");
    fprintf(out, "        frames.append((frame.name(), sal.symtab.filename, sal.line))\n");
    fprintf(out, "      frame = frame.older()\n");
    fprintf(out, "  except:\n");
    fprintf(out, "    pass\n");
    fprintf(out, "  time.sleep(1)\n");
    fprintf(out, "hot = Counter(frames).most_common(20)\n");
    fprintf(out, "import json\n");
    fprintf(out, "results = []\n");
    fprintf(out, "for (func, file, line), count in hot:\n");
    fprintf(out, "  results.append({\"function\": func, \"file\": file, \"line\": line, \"samples\": count})\n");
    fprintf(out, "print(json.dumps({\"source\": \"gdb\", \"results\": results}, indent=2))\n");
    fprintf(out, "GDBPYEOF\n");
    fprintf(out, "gdb -batch -x \"$PY\" -p \"$GDB_PID\" 2>/dev/null\n");
    fprintf(out, "rm -f \"$PY\"\n");
}

/* ============================================================
 * perf report 解析器 (parse 命令)
 * ============================================================
 * 输入: perf report --stdio 输出
 * 输出: JSON (到 stdout)
 */
static int parse_perf_report(const char *input_file) {
    FILE *fp = fopen(input_file, "r");
    if (!fp) { perror(input_file); return 1; }

    printf("{\n  \"source\": \"%s\",\n", input_file);
    printf("  \"parser\": \"perf report\",\n");
    printf("  \"results\": [\n");

    char line[4096];
    int first = 1;
    while (fgets(line, sizeof(line), fp)) {
        float percent = 0;
        char func[256] = {0};
        char file[256] = {0};
        int line_no = 0;

        // perf report 格式:
        //   N.NN%  command  shared  [.] function_name  file.c:line
        if (sscanf(line, " %f%%", &percent) != 1 || percent < 0.01) continue;

        // 提取 [.] 或 [k] 后面的函数名
        char *bracket = strstr(line, "[.]");
        if (!bracket) bracket = strstr(line, "[k]");
        if (!bracket) continue;
        bracket = strchr(bracket, ']');
        if (!bracket) continue;
        bracket++;
        while (*bracket == ' ') bracket++;
        int i = 0;
        while (*bracket && *bracket != ' ' && *bracket != '\n' && i < 255)
            func[i++] = *bracket++;
        func[i] = '\0';
        if (!func[0]) continue;

        // 提取文件名:行号 (行末)
        char *colon = strrchr(line, ':');
        if (colon) {
            int maybe_line = atoi(colon + 1);
            if (maybe_line > 0) {
                line_no = maybe_line;
                // 取文件名
                char *sp = colon;
                while (sp > line && *sp != ' ') sp--;
                if (*sp == ' ') sp++;
                sscanf(sp, "%255[^:]:%d", file, &line_no);
            }
        }

        if (!first) printf(",\n");
        first = 0;
        printf("    {\n");
        printf("      \"function\": \"%s\",\n", func);
        printf("      \"cpu_percent\": %.1f,\n", percent);
        if (file[0]) printf("      \"file\": \"%s\",\n", file);
        if (line_no > 0) printf("      \"line\": %d,\n", line_no);
        printf("      \"source\": \"perf\"\n");
        printf("    }");
    } // end while
    fclose(fp);
    printf("\n  ]\n}\n");
    return 0;
}

/* ============================================================
 * extract 命令: 从目标进程提取运行时映射表
 * ============================================================
 *
 * 运行时感知的核心: 读取目标进程堆内的运行时结构体,
 * 提取 "机器码地址 → 源码位置" 的映射表。
 *
 * 每个运行时需要独立实现:
 *   tracec extract luajit  <pid>   ← LuaJIT jit_State
 *   tracec extract v8      <pid>   ← V8 Isolate->code_cache
 *   tracec extract cpython <pid>   ← CPython PyFrameObject
 *   tracec extract php     <pid>   ← PHP zend_executor_globals
 *
 * 当前支持: luajit
 */

static int extract_luajit(int pid) {
    char path[512];
    snprintf(path, sizeof(path), "/tmp/tracec_extract_luajit_%d.py", pid);

    FILE *out = fopen(path, "w");
    if (!out) { perror(path); return 1; }

    // 生成 GDB Python 脚本: 安全提取 LuaJIT JIT 映射表
    // 不会崩溃: 所有内存访问都在 try/except 内
    fprintf(out, "import gdb, json, sys\n\n");
    fprintf(out, "mappings = []\n");
    fprintf(out, "MAX_TRACES = 10000\n\n");

    fprintf(out, "# 尝试通过已知符号找到 jit_State\n");
    fprintf(out, "jit_ptr = None\n");
    fprintf(out, "for sym in ['g_jit_state', 'jit_State', 'J', 'global_State']:\n");
    fprintf(out, "  try:\n");
    fprintf(out, "    val = gdb.parse_and_eval(sym)\n");
    fprintf(out, "    if val and str(val) != '0':\n");
    fprintf(out, "      jit_ptr = val\n");
    fprintf(out, "      break\n");
    fprintf(out, "  except:\n");
    fprintf(out, "    pass\n\n");

    fprintf(out, "if not jit_ptr:\n");
    fprintf(out, "  # 最后一个尝试: 扫描符号表找 lua_State\n");
    fprintf(out, "  try:\n");
    fprintf(out, "    jit_ptr = gdb.parse_and_eval(\"*(void**)g_jit\")\n");
    fprintf(out, "  except:\n");
    fprintf(out, "    pass\n\n");

    fprintf(out, "if not jit_ptr:\n");
    fprintf(out, "  print(json.dumps({'warning': 'jit_State not found',\n");
    fprintf(out, "    'note': 'not a LuaJIT process or symbols stripped',\n");
    fprintf(out, "    'mappings': []}))\n");
    fprintf(out, "  sys.exit(0)\n\n");

    fprintf(out, "try:\n");
    fprintf(out, "  trace_arr = jit_ptr['trace']\n");
    fprintf(out, "  for i in range(MAX_TRACES):\n");
    fprintf(out, "    tr = trace_arr[i]\n");
    fprintf(out, "    mcode = int(tr['mcode'])\n");
    fprintf(out, "    if mcode == 0: continue\n");
    fprintf(out, "    size = int(tr['szmcode'])\n");
    fprintf(out, "    tn   = int(tr['traceno'])\n");
    fprintf(out, "    startpc = int(tr['startpc'])\n");
    fprintf(out, "    mappings.append({\n");
    fprintf(out, "      'start': mcode,\n");
    fprintf(out, "      'end': mcode + size,\n");
    fprintf(out, "      'traceno': tn,\n");
    fprintf(out, "      'source': 'luajit_jit'\n");
    fprintf(out, "    })\n");
    fprintf(out, "except Exception as e:\n");
    fprintf(out, "  print(json.dumps({'error': str(e), 'mappings': []}))\n");
    fprintf(out, "  sys.exit(1)\n\n");

    fprintf(out, "print(json.dumps({\n");
    fprintf(out, "  'runtime': 'luajit',\n");
    fprintf(out, "  'pid': %d,\n", pid);
    fprintf(out, "  'trace_count': len(mappings),\n");
    fprintf(out, "  'mappings': mappings\n");
    fprintf(out, "}, indent=2))\n");

    fclose(out);

    printf("extract script: %s\n", path);
    printf("run: gdb -batch -x %s -p %d 2>/dev/null\n", path, pid);
    printf("(the script will exit safely if no LuaJIT runtime is found)\n");
    return 0;
}

/* ============================================================
 * CLI
 * ============================================================ */
static int detect(void) {
    struct { const char *name; const char *desc; int (*avail)(void); } backends[] = {
        {"perf", "Linux 自带",       NULL},
        {"stap", "SystemTap, 功能最强", NULL},
        {"bpftrace", "低开销",       NULL},
        {"dtrace", "macOS/BSD",     NULL},
        {"gdb",   "兜底, 慢但通用",  NULL},
        {NULL, NULL, NULL}
    };
    printf("available backends:\n");
    int has = 0, best_prio = 999;
    const char *best = NULL;
    for (int i = 0; backends[i].name; i++) {
        int ok = cmd_avail(backends[i].name);
        printf("  %-8s %s", backends[i].name, backends[i].desc);
        printf(ok ? " ✅\n" : "\n");
        if (ok) { has = 1; }
    }
    if (!has) printf("  (none) 安装 perf: apt install linux-tools-common\n");
    printf("\nrecommended: ");
    if (cmd_avail("stap")) printf("stap (SystemTap)\n");
    else if (cmd_avail("bpftrace")) printf("ebpf\n");
    else if (cmd_avail("perf")) printf("perf\n");
    else printf("(none)\n");
    return 0;
}

static void gen_script(FILE *out, const TraceScript *s, const char *target, int embed_pid) {
    if (strcmp(target, "stap") == 0) {
        fprintf(out, "#!/usr/bin/env stap\n");
    } else {
        fprintf(out, "#! /bin/bash\n");
    }
    fprintf(out, "# tracec generated | target: %s | probes: %d\n", target, s->probe_count);
    if (strcmp(target, "stap") != 0) {
        if (embed_pid > 0) {
            fprintf(out, "PID=%d\n", embed_pid);
        } else {
            fprintf(out, "PID=\"${PID:-$1}\"\n");
            fprintf(out, "if [ -z \"$PID\" ]; then echo \"usage: PID=<pid> bash $0\"; exit 1; fi\n");
        }
        fprintf(out, "set -e\n");
    }
    fprintf(out, "\n");

    for (int i = 0; i < s->probe_count; i++) {
        fprintf(out, "# ---- probe %d: %s ----\n", i + 1, s->probes[i].target);
        if (strcmp(target, "perf") == 0) gen_perf_probe(out, &s->probes[i]);
        else if (strcmp(target, "stap") == 0) gen_stap_probe(out, &s->probes[i]);
        else if (strcmp(target, "ebpf") == 0) gen_ebpf_probe(out, &s->probes[i]);
        else if (strcmp(target, "dtrace") == 0) gen_dtrace_probe(out, &s->probes[i]);
        else if (strcmp(target, "gdb") == 0) gen_gdb_probe(out, &s->probes[i]);
        fprintf(out, "\n");
    }

    if (strcmp(target, "stap") == 0) {
        fprintf(out, "# 运行: stap -x $PID probe.stp\n");
    } else if (strcmp(target, "gdb") == 0) {
        fprintf(out, "# 运行: gdb -batch -x trace_gdb.py -p $PID\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: tracec <detect|compile|parse|extract> [options]\n\n");
        fprintf(stderr, "commands:\n");
        fprintf(stderr, "  detect                  列出可用后端\n");
        fprintf(stderr, "  compile input.trace     编译探针脚本 [--target perf|stap|ebpf|dtrace|gdb]\n");
        fprintf(stderr, "  parse perf.report       解析 perf report 输出为 JSON\n");
        fprintf(stderr, "  extract luajit <pid>    从 LuaJIT 进程提取 JIT 映射表\n");
        fprintf(stderr, "  extract v8 <pid>        从 V8/Node.js 进程提取代码映射表\n");
        return 1;
    }

    if (strcmp(argv[1], "detect") == 0) return detect();

    if (strcmp(argv[1], "extract") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: tracec extract <runtime> <pid>\n");
                        fprintf(stderr, "runtimes: luajit\n"); return 1; }
        const char *runtime = argv[2];
        int pid = atoi(argv[3]);
        if (pid <= 0) { fprintf(stderr, "invalid pid\n"); return 1; }
        if (strcmp(runtime, "luajit") == 0) return extract_luajit(pid);
        fprintf(stderr, "unknown runtime: %s (supported: luajit)\n", runtime);
        return 1;
    }

    if (strcmp(argv[1], "parse") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: tracec parse perf.report\n"); return 1; }
        return parse_perf_report(argv[2]);
    }

    if (strcmp(argv[1], "compile") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: tracec compile input.trace [--target perf|stap|ebpf] [--output file] [--pid N]\n"); return 1; }
        const char *input_file = argv[2];
        const char *target = "auto";
        char output_file[512] = {0};
        int embed_pid = 0;

        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
            else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) snprintf(output_file, sizeof(output_file), "%s", argv[++i]);
            else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) embed_pid = atoi(argv[++i]);
        }

    // 自动选后端
    if (strcmp(target, "auto") == 0) {
        if (cmd_avail("stap")) target = "stap";
        else if (cmd_avail("bpftrace")) target = "ebpf";
        else target = "perf";
    }

    // 读取 + 解析
    long len;
    char *buf = read_file(input_file, &len);
    if (!buf) return 1;
    TraceScript script;
    if (parse_trace(&script, buf) != 0) { free(buf); return 1; }
    free(buf);

        // 确定输出路径
        if (!output_file[0])
            snprintf(output_file, sizeof(output_file), "trace_%s.sh", target);

        FILE *out = fopen(output_file, "w");
        if (!out) { perror(output_file); return 1; }

        if (strcmp(target, "stap") == 0) {
            gen_script(out, &script, "stap", embed_pid);
        } else {
            gen_script(out, &script, target, embed_pid);
        }

        fclose(out);
        printf("generated: %s (target: %s, %d probes)\n", output_file, target, script.probe_count);
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 1;
}
