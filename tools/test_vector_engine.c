#include <stdio.h>
#include <stdlib.h>
#include "vector_engine.h"

int main(int argc, char** argv) {
    const char* analysis_dir = argc > 1 ? argv[1] : "./nginx_cache";
    const char* query = argc > 2 ? argv[2] : "memory pool allocation";
    const char* repo = argc > 3 ? argv[3] : "/code/local/nginx";
    
    printf("Opening vector engine: %s (repo=%s)\n", analysis_dir, repo);
    vector_engine_t* ve = vector_engine_open(analysis_dir, "jina");
    if (!ve) {
        printf("Failed: %s\n", vector_engine_error());
        return 1;
    }
    
    printf("Total vectors: %zu\n", vector_engine_count(ve));
    
    printf("\nSearching: '%s' in repo '%s'\n", query, repo);
    vector_result_t results[10];
    int n = vector_engine_search_ns(ve, query, 10, repo, results);
    printf("Found: %d results\n\n", n);
    
    for (int i = 0; i < n; i++) {
        printf("[%d] %s (score: %.4f)\n", i + 1, results[i].name, results[i].score);
        printf("    file: %s:%d\n", results[i].file, results[i].line_start);
        printf("    kind: %s\n", results[i].kind);
        printf("    content: %.100s...\n", results[i].content);
        printf("\n");
    }
    
    vector_engine_close(ve);
    return 0;
}
