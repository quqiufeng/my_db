#include "json.h"
#include "task.h"
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

/* task_execute - 通过 popen 执行命令 */
int task_execute(const task_t *task, task_result_t *result) {
    memset(result, 0, sizeof(*result));
    snprintf(result->task_id, sizeof(result->task_id), "%s", task->id);

    FILE *fp = popen(task->cmd, "r");
    if (!fp) {
        result->exit_code = -1;
        snprintf(result->stdout_buf, sizeof(result->stdout_buf), "popen failed");
        return -1;
    }

    size_t pos = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp) != NULL && pos < sizeof(result->stdout_buf) - 4096) {
        size_t llen = strlen(line);
        if (pos + llen < sizeof(result->stdout_buf)) {
            memcpy(result->stdout_buf + pos, line, llen);
            pos += llen;
        }
    }
    result->exit_code = pclose(fp);
    result->duration_ms = (int)(now_ms() - task->create_time);
    return 0;
}

/* task_result_to_json - 序列化为 JSON */
char *task_result_to_json(const task_result_t *result, char *buf, size_t size) {
    char escaped_stdout[sizeof(result->stdout_buf) * 2];
    json_escape(result->stdout_buf, escaped_stdout, sizeof(escaped_stdout));

    snprintf(buf, size,
        "{\"task_id\":\"%s\",\"node\":\"%s\",\"exit\":%d,\"stdout\":\"%s\",\"duration_ms\":%d}",
        result->task_id, result->node_id, result->exit_code,
        escaped_stdout, result->duration_ms);
    return buf;
}

/* task_generate_id - 生成唯一 ID */
void task_generate_id(char *buf, size_t size) {
    snprintf(buf, size, "t_%ld_%d", now_ms(), rand() % 10000);
}
