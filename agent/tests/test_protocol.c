#include <stdio.h>
#include <string.h>
#include "../include/protocol.h"

static int tests_pass = 0, tests_fail = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); tests_fail++; } \
    else { tests_pass++; } \
} while(0)

int main(void) {
    /* pack/unpack header */
    proto_header_t hdr;
    pack_header(&hdr, 0x10, 0x01, 1234);

    TEST("magic0", hdr.magic[0] == PROTO_MAGIC_0);
    TEST("magic1", hdr.magic[1] == PROTO_MAGIC_1);
    TEST("type", hdr.type == 0x10);
    TEST("flags", hdr.flags == 0x01);

    uint8_t type, flags;
    uint32_t len;
    int ret = unpack_header(&hdr, &type, &flags, &len);
    TEST("unpack ok", ret == 0);
    TEST("unpack type", type == 0x10);
    TEST("unpack flags", flags == 0x01);
    TEST("unpack len", len == 1234);

    /* bad magic */
    hdr.magic[0] = 0x00;
    ret = unpack_header(&hdr, &type, &flags, &len);
    TEST("bad magic", ret < 0);

    /* session_ctx JSON */
    session_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.leader_addr, sizeof(ctx.leader_addr), "192.168.1.1:9527");
    ctx.epoch = 5;
    ctx.cluster_size = 3;
    snprintf(ctx.node_id, sizeof(ctx.node_id), "node1");
    snprintf(ctx.role, sizeof(ctx.role), "leader");
    snprintf(ctx.self_addr, sizeof(ctx.self_addr), "192.168.1.1:9527");
    ctx.load_1 = 0.5;
    ctx.mem_pct = 42.0;
    ctx.cpu_pct = 12.5;
    ctx.disk_pct = 55.0;
    ctx.uptime_sec = 100000;
    ctx.client_count = 3;

    char json[2048];
    session_ctx_to_json(&ctx, json, sizeof(json));
    TEST("json output", strlen(json) > 0);
    TEST("json has leader", strstr(json, "192.168.1.1:9527") != NULL);
    TEST("json has epoch", strstr(json, "\"epoch\":5") != NULL);
    TEST("json has load", strstr(json, "0.50") != NULL);

    printf("Results: %d passed, %d failed\n", tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
