/* ===================================================================
 * deploy - Master-side SCP+SSH deployment tool
 *
 * 用法:
 *   deploy --ip "10.0.0.1,10.0.0.2,10.0.0.3" [options]
 *
 * 选项:
 *   --ip <list>       目标节点 IP 列表（逗号分隔，可含端口号）
 *   --user <name>     SSH 用户名（默认 root）
 *   --key <path>      SSH 私钥路径（可选）
 *   --password <pw>   SSH 密码（可选，不推荐）
 *   --binary <path>   agent 二进制文件路径（默认 ./agent）
 *   --port <n>        agent 监听端口（默认 9527）
 *   --datadir <dir>   数据目录（默认 /tmp/agent）
 *   --foreground      前台运行（不 daemonize）
 *   --help            显示帮助
 *
 * 输出: JSON
 *   {"status":"ok","nodes":[{"ip":"...","idx":0,"pid":1234,"error":""},...]}
 * 退出码:
 *   0 = 全部成功
 *   1 = 参数错误
 *   2 = 部署部分失败
 *
 * 环境变量:
 *   DEPLOY_USER, DEPLOY_KEY, DEPLOY_PASSWORD 可用于代替 CLI 参数
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

/* ===================================================================
 * 配置
 * =================================================================== */
typedef struct {
    char     ip_list[4096];        /* -ip 参数 */
    char     ssh_user[256];        /* SSH 用户名 */
    char     ssh_key[1024];        /* SSH 私钥路径 */
    char     ssh_password[1024];   /* SSH 密码 */
    char     binary_path[1024];    /* agent 二进制路径 */
    char     data_dir[256];        /* 数据目录 */
    int      agent_port;           /* agent 监听端口 */
    int      foreground;           /* 前台运行标志 */
} deploy_config_t;

/* ===================================================================
 * JSON 输出帮助
 * =================================================================== */
static void json_print_result(int total, int success, int failed,
                              char (*results)[8192], int results_count) {
    printf("{\"status\":\"%s\",\"total\":%d,\"success\":%d,\"failed\":%d,\"nodes\":[",
           failed == 0 ? "ok" : "partial",
           total, success, failed);
    for (int i = 0; i < results_count; i++) {
        if (i > 0) printf(",");
        printf("%s", results[i]);
    }
    printf("]}\n");
}

/* ===================================================================
 * 打印帮助
 * =================================================================== */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --ip <list> [options]\n"
        "\n"
        "Options:\n"
        "  --ip <list>       Target node IP list (comma-separated, optional :port)\n"
        "  --user <name>     SSH username (default: root)\n"
        "  --key <path>      SSH private key path\n"
        "  --password <pw>   SSH password (not recommended)\n"
        "  --binary <path>   Agent binary path (default: ./agent)\n"
        "  --port <n>        Agent listen port (default: 9527)\n"
        "  --datadir <dir>   Data directory (default: /tmp/agent)\n"
        "  --foreground      Run in foreground\n"
        "  --help            Show this help\n"
        "\n"
        "Environment:\n"
        "  DEPLOY_USER, DEPLOY_KEY, DEPLOY_PASSWORD\n",
        prog);
}

/* ===================================================================
 * 执行 shell 命令并等待完成
 * 返回 0 成功，-1 失败
 * =================================================================== */
static int run_cmd(const char *cmd, char *output_buf, size_t output_size) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        if (output_buf) snprintf(output_buf, output_size, "popen failed: %s", strerror(errno));
        return -1;
    }
    if (output_buf) {
        size_t n = fread(output_buf, 1, output_size - 1, fp);
        output_buf[n] = '\0';
        /* 去掉末尾换行 */
        while (n > 0 && (output_buf[n-1] == '\n' || output_buf[n-1] == '\r'))
            output_buf[--n] = '\0';
    }
    int status = pclose(fp);
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    return -1;
}

/* ===================================================================
 * deploy_node - 部署到单个节点
 * =================================================================== */
static int deploy_node(deploy_config_t *cfg, const char *ip, int port,
                       int idx, char *result_json, size_t result_size) {
    char cmd[8192];
    char output[4096];
    int  ret;

    /* 构建 SSH 连接参数 */
    char ssh_extra[2048] = "";
    if (cfg->ssh_key[0]) {
        snprintf(ssh_extra, sizeof(ssh_extra), "-i %s", cfg->ssh_key);
    }
    /* 如果有密码，使用 sshpass */
    char sshpass_prefix[2048] = "";
    if (cfg->ssh_password[0]) {
        snprintf(sshpass_prefix, sizeof(sshpass_prefix),
                 "sshpass -p '%s' ", cfg->ssh_password);
    }

    char remote_host[512];
    snprintf(remote_host, sizeof(remote_host), "%s@%s", cfg->ssh_user, ip);

    /* ========= Step 1: 创建远程目录 ========= */
    snprintf(cmd, sizeof(cmd),
             "%sssh %s -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
             "%s \"mkdir -p %s\" 2>/dev/null",
             sshpass_prefix, ssh_extra, remote_host, cfg->data_dir);
    ret = run_cmd(cmd, output, sizeof(output));
    if (ret < 0) {
        snprintf(result_json, result_size,
                 "{\"ip\":\"%s\",\"idx\":%d,\"status\":\"failed\","
                 "\"error\":\"mkdir failed\"}",
                 ip, idx);
        return -1;
    }

    /* ========= Step 2: SCP agent 二进制 ========= */
    snprintf(cmd, sizeof(cmd),
             "scp %s -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
             "%s %s:%s/agent 2>/dev/null",
             ssh_extra, cfg->binary_path, remote_host, cfg->data_dir);
    ret = run_cmd(cmd, output, sizeof(output));
    if (ret < 0) {
        snprintf(result_json, result_size,
                 "{\"ip\":\"%s\",\"idx\":%d,\"status\":\"failed\","
                 "\"error\":\"scp failed\"}",
                 ip, idx);
        return -1;
    }

    /* ========= Step 3: SSH 启动 agent ========= */
    char extra_args[256] = "";
    if (cfg->foreground)
        snprintf(extra_args, sizeof(extra_args), " --foreground");

    snprintf(cmd, sizeof(cmd),
             "%sssh %s -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
             "%s \"chmod +x %s/agent && "
             "nohup %s/agent --id node%d --idx %d --port %d "
             "--datadir %s%s > /dev/null 2>&1 & "
             "echo \\$!\" 2>/dev/null",
             sshpass_prefix, ssh_extra, remote_host,
             cfg->data_dir, cfg->data_dir,
             idx, idx, port, cfg->data_dir, extra_args);
    ret = run_cmd(cmd, output, sizeof(output));

    if (ret < 0 || output[0] == '\0') {
        snprintf(result_json, result_size,
                 "{\"ip\":\"%s\",\"idx\":%d,\"status\":\"failed\","
                 "\"error\":\"start failed\"}",
                 ip, idx);
        return -1;
    }

    /* 成功 */
    snprintf(result_json, result_size,
             "{\"ip\":\"%s\",\"idx\":%d,\"status\":\"ok\",\"pid\":%s}",
             ip, idx, output);
    return 0;
}

/* ===================================================================
 * main
 * =================================================================== */
int main(int argc, char **argv) {
    deploy_config_t config;
    memset(&config, 0, sizeof(config));

    /* 默认值 */
    snprintf(config.ssh_user, sizeof(config.ssh_user), "%s",
             getenv("DEPLOY_USER") ? getenv("DEPLOY_USER") : "root");
    if (getenv("DEPLOY_KEY"))
        snprintf(config.ssh_key, sizeof(config.ssh_key), "%s", getenv("DEPLOY_KEY"));
    if (getenv("DEPLOY_PASSWORD"))
        snprintf(config.ssh_password, sizeof(config.ssh_password), "%s", getenv("DEPLOY_PASSWORD"));
    snprintf(config.binary_path, sizeof(config.binary_path), "./agent");
    snprintf(config.data_dir, sizeof(config.data_dir), "/tmp/agent");
    config.agent_port = 9527;
    config.foreground = 0;

    /* ========= 解析参数 ========= */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            snprintf(config.ip_list, sizeof(config.ip_list), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            snprintf(config.ssh_user, sizeof(config.ssh_user), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            snprintf(config.ssh_key, sizeof(config.ssh_key), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            snprintf(config.ssh_password, sizeof(config.ssh_password), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--binary") == 0 && i + 1 < argc) {
            snprintf(config.binary_path, sizeof(config.binary_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            config.agent_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--datadir") == 0 && i + 1 < argc) {
            snprintf(config.data_dir, sizeof(config.data_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--foreground") == 0) {
            config.foreground = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.ip_list[0] == '\0') {
        fprintf(stderr, "ERROR: --ip is required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* ========= 解析 IP 列表 ========= */
    char ip_copy[4096];
    char *ips[256];
    int   ports[256];
    int   ip_count = 0;

    snprintf(ip_copy, sizeof(ip_copy), "%s", config.ip_list);
    char *saveptr, *token = strtok_r(ip_copy, ",", &saveptr);
    while (token && ip_count < 256) {
        /* 去除前后空格 */
        while (*token == ' ') token++;
        char *colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            ports[ip_count] = atoi(colon + 1);
        } else {
            ports[ip_count] = config.agent_port;
        }
        ips[ip_count] = strdup(token);
        ip_count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (ip_count == 0) {
        fprintf(stderr, "ERROR: no valid IPs in list\n");
        return 1;
    }

    /* ========= 部署到所有节点 ========= */
    char results[256][8192];
    int  success = 0, failed = 0;

    for (int i = 0; i < ip_count; i++) {
        fprintf(stderr, "deploying to %s (idx=%d)...\n", ips[i], i);
        if (deploy_node(&config, ips[i], ports[i], i,
                        results[i], sizeof(results[i])) == 0) {
            success++;
        } else {
            failed++;
        }
        free(ips[i]);
    }

    /* ========= 输出 JSON 结果 ========= */
    json_print_result(ip_count, success, failed, results, ip_count);

    return failed == 0 ? 0 : 2;
}
