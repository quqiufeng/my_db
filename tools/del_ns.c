// del_ns.c — 删除整个 namespace（走 cache_del_namespace 批量路径，O(n) 而非 O(n*k)）。
// 用法: del_ns <cache_dir> <ns> [--max-mb N]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cache.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <cache_dir> <ns> [--max-mb N]\n", argv[0]); return 2; }
    const char *dir = argv[1], *ns = argv[2];
    size_t max_mb = 16384;
    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "--max-mb") == 0 && i + 1 < argc) max_mb = (size_t)atol(argv[++i]);

    cache_t *c = cache_open(dir, max_mb * 1024 * 1024);
    if (!c) { fprintf(stderr, "cache_open failed: %s\n", dir); return 1; }
    size_t before = cache_count(c);
    fprintf(stderr, "before: entries=%zu\n", before);
    int rc = cache_del_namespace(c, ns);
    if (rc != 0) { fprintf(stderr, "cache_del_namespace failed rc=%d\n", rc); cache_close(c); return 1; }
    cache_sync(c);
    size_t after = cache_count(c);
    fprintf(stderr, "after:  entries=%zu  (removed %zu)  ns=%s\n", after, before - after, ns);
    cache_close(c);
    return 0;
}
