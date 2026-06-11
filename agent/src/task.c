#include "task.h"
#include "agent.h"
#include "json.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int task_execute(const task_t *task, task_result_t *result) {
    memset(result, 0, sizeof(*result));
    snprintf(result->task_id, sizeof(result->task_id), "%s", task->id);

    FILE *fp = popen(task->cmd, "r");
    if (!fp) {
        result->exit_code = -1;
        snprintf(result->stdout_buf, sizeof(result->stdout_buf), "popen failed");
        return -1;
    }

    int ch;
    size_t pos = 0;
    while ((ch = fgetc(fp)) != EOF && pos < sizeof(result->stdout_buf) - 1) {
        result->stdout_buf[pos++] = (char)ch;
    }
    result->stdout_buf[pos] = 0;
    result->exit_code = pclose(fp);
    result->duration_ms = (int)(now_ms() - task->create_time);
    return 0;
}

char *task_result_to_json(const task_result_t *result, char *buf, size_t size) {
    char escaped_stdout[sizeof(result->stdout_buf) * 2];
    json_escape(result->stdout_buf, escaped_stdout, sizeof(escaped_stdout));
    snprintf(buf, size,
        "{\"task_id\":\"%s\",\"node\":\"%s\",\"exit\":%d,\"stdout\":\"%s\",\"duration_ms\":%d}",
        result->task_id, result->node_id, result->exit_code,
        escaped_stdout, result->duration_ms);
    return buf;
}

void task_generate_id(char *buf, size_t size) {
    snprintf(buf, size, "t_%ld_%d", now_ms(), rand() % 10000);
}
