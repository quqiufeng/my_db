#ifndef TASK_H
#define TASK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 任务类型
 * =================================================================== */
typedef enum {
    TASK_EXEC    = 0,   /* 执行 shell 命令 */
    TASK_SERVICE,       /* 管理 systemd 服务 */
    TASK_MONITOR,       /* 采集系统指标 */
    TASK_SCRIPT,        /* 上传并执行脚本 */
} task_type_t;

/* ===================================================================
 * 任务结构
 * =================================================================== */
typedef struct {
    char     id[64];            /* 任务 ID */
    task_type_t type;           /* 任务类型 */
    char     cmd[1024];         /* 命令内容 */
    int      timeout_ms;        /* 超时（毫秒） */
    long     create_time;       /* 创建时间 */
    char     target_node[64];   /* 目标节点，"" 表示所有 */
} task_t;

/* ===================================================================
 * 任务结果
 * =================================================================== */
typedef struct {
    char     task_id[64];       /* 任务 ID */
    char     node_id[64];       /* 执行节点 ID */
    int      exit_code;         /* 退出码 */
    char     stdout_buf[4096];  /* 标准输出 */
    char     stderr_buf[1024];  /* 错误输出 */
    int      duration_ms;       /* 执行耗时 */
    int      timed_out;         /* 是否超时 */
} task_result_t;

/* ===================================================================
 * 任务函数
 * =================================================================== */

/**
 * task_execute - 执行一个任务（fork + exec）
 * @task:   任务描述
 * @result: 输出，执行结果
 * 返回 0 成功（已填充 result），-1 执行失败
 */
int task_execute(const task_t *task, task_result_t *result);

/**
 * task_result_to_json - 将任务结果序列化为 JSON
 */
char *task_result_to_json(const task_result_t *result, char *buf, size_t size);

/**
 * task_generate_id - 生成唯一任务 ID
 */
void task_generate_id(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* TASK_H */
