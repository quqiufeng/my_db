#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define BUF_SIZE (1024 * 1024)  // 1MB buffer

// Copy file using sendfile (zero-copy when possible)
static int copy_file(const char* src, const char* dst) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) return -1;
    
    struct stat st;
    fstat(in_fd, &st);
    
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }
    
    off_t offset = 0;
    ssize_t sent;
    while ((sent = sendfile(out_fd, in_fd, &offset, st.st_size - offset)) > 0) {
        // Continue until all data is copied
    }
    
    close(in_fd);
    close(out_fd);
    
    return sent < 0 && offset < st.st_size ? -1 : 0;
}

// Fallback: copy using read/write
static int copy_file_fallback(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    
    char* buf = malloc(BUF_SIZE);
    if (!buf) {
        fclose(in);
        fclose(out);
        return -1;
    }
    
    size_t n;
    while ((n = fread(buf, 1, BUF_SIZE, in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    
    free(buf);
    fclose(in);
    fclose(out);
    
    return 0;
}

// Copy a file (try sendfile first, fallback to read/write)
static int copy_file_smart(const char* src, const char* dst) {
    if (copy_file(src, dst) == 0) return 0;
    return copy_file_fallback(src, dst);
}

// Ensure directory exists
static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    char* p = tmp;
    if (*p == '/') p++;
    
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
        p++;
    }
    mkdir(tmp, 0755);
    
    return 0;
}

// Export cache to snapshot
static int do_export(const char* cache_dir, const char* snapshot_dir) {
    printf("Exporting cache: %s -> %s\n", cache_dir, snapshot_dir);
    
    ensure_dir(snapshot_dir);
    
    // Files to copy
    const char* files[] = {
        "cache.bin",
        "index.bin",
        NULL
    };
    
    for (int i = 0; files[i]; i++) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s", cache_dir, files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", snapshot_dir, files[i]);
        
        struct stat st;
        if (stat(src, &st) < 0) {
            printf("  Skip: %s (not found)\n", files[i]);
            continue;
        }
        
        printf("  Copying: %s (%zu bytes)\n", files[i], (size_t)st.st_size);
        if (copy_file_smart(src, dst) < 0) {
            fprintf(stderr, "  Failed to copy: %s\n", files[i]);
            return -1;
        }
    }
    
    printf("Export complete!\n");
    return 0;
}

// Import snapshot to cache
static int do_import(const char* snapshot_dir, const char* cache_dir) {
    printf("Importing snapshot: %s -> %s\n", snapshot_dir, cache_dir);
    
    ensure_dir(cache_dir);
    
    // Files to copy
    const char* files[] = {
        "cache.bin",
        "index.bin",
        NULL
    };
    
    for (int i = 0; files[i]; i++) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s", snapshot_dir, files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", cache_dir, files[i]);
        
        struct stat st;
        if (stat(src, &st) < 0) {
            printf("  Skip: %s (not found)\n", files[i]);
            continue;
        }
        
        printf("  Copying: %s (%zu bytes)\n", files[i], (size_t)st.st_size);
        if (copy_file_smart(src, dst) < 0) {
            fprintf(stderr, "  Failed to copy: %s\n", files[i]);
            return -1;
        }
    }
    
    printf("Import complete!\n");
    return 0;
}

// Verify snapshot
static int do_verify(const char* snapshot_dir) {
    printf("Verifying snapshot: %s\n", snapshot_dir);
    
    cache_t* cache = cache_open(snapshot_dir, 0);
    if (!cache) {
        fprintf(stderr, "Failed to open snapshot\n");
        return -1;
    }
    
    printf("  Entries: %zu\n", cache_count(cache));
    
    // Try a search
    cache_result_t* results = NULL;
    size_t count = 0;
    cache_search_options_t opts = cache_search_options_default();
    opts.max_results = 1;
    cache_search_prefix(cache, "/books/", &opts, &results, &count);
    printf("  Sample search: %zu results\n", count);
    cache_results_free(results);
    
    cache_close(cache);
    printf("Snapshot is valid!\n");
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <export|import|verify> <source> [destination]\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s export ./my_cache ./snapshot      # Export cache to snapshot\n", argv[0]);
        printf("  %s import ./snapshot ./my_cache      # Import snapshot to cache\n", argv[0]);
        printf("  %s verify ./snapshot                 # Verify snapshot integrity\n", argv[0]);
        return 1;
    }
    
    const char* cmd = argv[1];
    const char* src = argv[2];
    const char* dst = argc > 3 ? argv[3] : NULL;
    
    if (strcmp(cmd, "export") == 0) {
        if (!dst) {
            fprintf(stderr, "Export requires destination\n");
            return 1;
        }
        return do_export(src, dst);
    } else if (strcmp(cmd, "import") == 0) {
        if (!dst) {
            fprintf(stderr, "Import requires destination\n");
            return 1;
        }
        return do_import(src, dst);
    } else if (strcmp(cmd, "verify") == 0) {
        return do_verify(src);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
}
