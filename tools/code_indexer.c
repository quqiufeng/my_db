#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include "cache.h"

#define MAX_PATH_LEN 2048

// 源文件信息
typedef struct {
    char* path;
} source_file_t;

// Worker 分配
typedef struct {
    const char* repo_path;
    char** files;
    int file_count;
    int capacity;
} worker_t;

// 文件扩展名检测
static int is_source_file(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    return (
        strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 ||
        strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
        strcmp(ext, ".cxx") == 0 || strcmp(ext, ".hpp") == 0 ||
        strcmp(ext, ".py") == 0 || strcmp(ext, ".js") == 0 ||
        strcmp(ext, ".ts") == 0 || strcmp(ext, ".go") == 0 ||
        strcmp(ext, ".rs") == 0 || strcmp(ext, ".java") == 0 ||
        strcmp(ext, ".rb") == 0 || strcmp(ext, ".php") == 0 ||
        strcmp(ext, ".sh") == 0 || strcmp(ext, ".lua") == 0 ||
        strcmp(ext, ".swift") == 0 || strcmp(ext, ".scala") == 0 ||
        strcmp(ext, ".r") == 0 || strcmp(ext, ".R") == 0
    );
}

// Noise 目录
static int is_noise_dir(const char* name) {
    static const char* noise[] = {
        "thirdparty", "3rdparty", "third_party", "third-party",
        "external", "deps", "dependencies", "vendor",
        "build", "cmake-build", "CMakeFiles", "out", "bin", "obj",
        "test", "tests", "testing", "gtest", "googletest",
        "examples", "demo", "demos", "sample", "samples",
        "docs", "doc", "documentation", "website",
        "scripts", "tools", "utils", "benchmark", "benchmarks",
        ".git", "node_modules", "__pycache__", ".pytest_cache",
        NULL
    };
    for (int i = 0; noise[i]; i++) {
        if (strcmp(name, noise[i]) == 0) return 1;
    }
    return 0;
}

// 递归扫描目录
static void scan_directory(const char* base_path, const char* current_path,
                           source_file_t** files, int* count, int* capacity) {
    DIR* dir = opendir(current_path);
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        if (is_noise_dir(entry->d_name)) continue;
        
        char fullpath[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", current_path, entry->d_name);
        
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            scan_directory(base_path, fullpath, files, count, capacity);
        } else if (S_ISREG(st.st_mode) && is_source_file(entry->d_name)) {
            if (*count >= *capacity) {
                int new_cap = *capacity * 2;
                source_file_t* new_files = realloc(*files, new_cap * sizeof(source_file_t));
                if (!new_files) continue;
                *files = new_files;
                *capacity = new_cap;
            }
            (*files)[*count].path = strdup(fullpath);
            (*count)++;
        }
    }
    closedir(dir);
}

// JSON 字段提取
static int json_extract_str(const char* json, const char* key, char* out, int max_len) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) {
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);
        p = strstr(json, pattern);
        if (!p) return 0;
        p += strlen(pattern);
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '"') {
            p++;
            int i = 0;
            while (*p && *p != '"' && i < max_len - 1) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                        case 'n': out[i++] = '\n'; break;
                        case 't': out[i++] = '\t'; break;
                        case 'r': out[i++] = '\r'; break;
                        case '\\': out[i++] = '\\'; break;
                        case '"': out[i++] = '"'; break;
                        default: out[i++] = *p; break;
                    }
                } else {
                    out[i++] = *p;
                }
                p++;
            }
            out[i] = '\0';
            return i;
        }
        int i = 0;
        while (*p && (isdigit((unsigned char)*p) || *p == '-') && i < max_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return i;
    }
    p += strlen(pattern);
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

static int json_extract_int(const char* json, const char* key) {
    char buf[32];
    if (json_extract_str(json, key, buf, sizeof(buf)) > 0) {
        return atoi(buf);
    }
    return 0;
}

static const char* detect_language(const char* filepath) {
    const char* ext = strrchr(filepath, '.');
    if (!ext) return "unknown";
    if (strcmp(ext, ".c") == 0) return "c";
    if (strcmp(ext, ".h") == 0) return "c";
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || 
        strcmp(ext, ".cxx") == 0 || strcmp(ext, ".hpp") == 0) return "cpp";
    if (strcmp(ext, ".py") == 0) return "python";
    if (strcmp(ext, ".js") == 0) return "javascript";
    if (strcmp(ext, ".ts") == 0) return "typescript";
    if (strcmp(ext, ".go") == 0) return "go";
    if (strcmp(ext, ".rs") == 0) return "rust";
    if (strcmp(ext, ".java") == 0) return "java";
    return "unknown";
}

static int needs_range(const char* kind) {
    return kind[0] && (
        strcmp(kind, "function") == 0 || strcmp(kind, "method") == 0 ||
        strcmp(kind, "class") == 0 || strcmp(kind, "struct") == 0 ||
        strcmp(kind, "union") == 0 || strcmp(kind, "enum") == 0 ||
        strcmp(kind, "namespace") == 0 || strcmp(kind, "macro") == 0
    );
}

static int find_end_line(const char* filepath, int start_line) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return start_line;
    
    int current_line = 0;
    int brace_depth = 0;
    int in_string = 0;
    int string_char = 0;
    int found_start = 0;
    char line[4096];
    
    while (fgets(line, sizeof(line), fp)) {
        current_line++;
        if (current_line < start_line) continue;
        
        if (current_line == start_line) {
            char* p = line;
            while (*p) {
                if (!in_string) {
                    if (*p == '"' || *p == '\'') {
                        in_string = 1; string_char = *p;
                    } else if (*p == '{') {
                        brace_depth++; found_start = 1;
                    } else if (*p == '}') {
                        if (brace_depth > 0) brace_depth--;
                    }
                } else {
                    if (*p == string_char && *(p-1) != '\\') in_string = 0;
                }
                p++;
            }
            if (!found_start) { fclose(fp); return start_line; }
            if (brace_depth == 0) { fclose(fp); return current_line; }
            continue;
        }
        
        char* p = line;
        while (*p) {
            if (!in_string) {
                if (*p == '"' || *p == '\'') { in_string = 1; string_char = *p; }
                else if (*p == '/' && *(p+1) == '/') break;
                else if (*p == '/' && *(p+1) == '*') in_string = 2;
                else if (*p == '{') brace_depth++;
                else if (*p == '}') {
                    brace_depth--;
                    if (brace_depth == 0) { fclose(fp); return current_line; }
                }
            } else if (in_string == 2) {
                if (*p == '*' && *(p+1) == '/') { in_string = 0; p++; }
            } else {
                if (*p == string_char && *(p-1) != '\\') in_string = 0;
            }
            p++;
        }
        
        if (current_line > start_line + 500) { fclose(fp); return current_line; }
    }
    
    fclose(fp);
    return current_line;
}

static int extract_content(const char* filepath, int start_line, int end_line, 
                           char* out, int max_len) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return 0;
    
    int current_line = 0;
    int pos = 0;
    char line[4096];
    
    while (fgets(line, sizeof(line), fp) && current_line < end_line) {
        current_line++;
        if (current_line < start_line) continue;
        int len = strlen(line);
        if (pos + len < max_len - 1) {
            memcpy(out + pos, line, len);
            pos += len;
        } else break;
    }
    
    fclose(fp);
    out[pos] = '\0';
    return pos;
}

static int json_escape_str(const char* src, char* dst, int max_len) {
    int j = 0;
    for (int i = 0; src[i] && j < max_len - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j < max_len - 2) { dst[j++] = '\\'; dst[j++] = c; }
        } else if (c == '\n') {
            if (j < max_len - 3) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (c == '\r') {
            if (j < max_len - 3) { dst[j++] = '\\'; dst[j++] = 'r'; }
        } else if (c == '\t') {
            if (j < max_len - 3) { dst[j++] = '\\'; dst[j++] = 't'; }
        } else if ((unsigned char)c < 0x20) {
            int n = snprintf(dst + j, max_len - j, "\\u%04x", (unsigned char)c);
            if (n > 0) j += n;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j;
}

static void write_chunk(FILE* fp, const char* name, const char* file, 
                        const char* kind, int line_start, int line_end,
                        const char* lang, const char* signature, 
                        const char* content, const char* docstring) {
    char esc_name[512], esc_file[1024], esc_kind[64], esc_sig[4096];
    char esc_content[65536], esc_doc[8192];
    
    json_escape_str(name, esc_name, sizeof(esc_name));
    json_escape_str(file, esc_file, sizeof(esc_file));
    json_escape_str(kind, esc_kind, sizeof(esc_kind));
    json_escape_str(signature, esc_sig, sizeof(esc_sig));
    json_escape_str(content, esc_content, sizeof(esc_content));
    json_escape_str(docstring, esc_doc, sizeof(esc_doc));
    
    fprintf(fp, "{\"name\":\"%s\",\"file\":\"%s\",\"kind\":\"%s\","
            "\"line_start\":%d,\"line_end\":%d,\"language\":\"%s\","
            "\"signature\":\"%s\",\"content\":\"%s\",\"docstring\":\"%s\"}\n",
            esc_name, esc_file, esc_kind, line_start, line_end, lang, esc_sig, esc_content, esc_doc);
}

static void worker_process(worker_t* worker, int worker_id) {
    printf("[Worker %d] Processing %d files...\n", worker_id, worker->file_count);
    
    // Write file list to temp file (avoid command line length limit)
    char file_list[256];
    snprintf(file_list, sizeof(file_list), "/tmp/code_indexer_files_%d.txt", worker_id);
    FILE* list_fp = fopen(file_list, "w");
    if (!list_fp) {
        fprintf(stderr, "[Worker %d] Failed to create file list\n", worker_id);
        return;
    }
    for (int i = 0; i < worker->file_count; i++) {
        fprintf(list_fp, "%s\n", worker->files[i]);
    }
    fclose(list_fp);
    
    char output_file[256];
    snprintf(output_file, sizeof(output_file), "/tmp/code_indexer_worker_%d.json", worker_id);
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "ctags-universal --output-format=json --fields=+nKzS "
        "--extras=+r+f --sort=no -L %s > %s 2>/dev/null",
        file_list, output_file);
    
    int ret = system(cmd);
    unlink(file_list); // Clean up file list
    
    if (ret != 0) {
        fprintf(stderr, "[Worker %d] ctags failed: %d\n", worker_id, ret);
        return;
    }
    
    FILE* ctags_fp = fopen(output_file, "r");
    if (!ctags_fp) {
        fprintf(stderr, "[Worker %d] Failed to open ctags output\n", worker_id);
        return;
    }
    
    char chunk_file[256];
    snprintf(chunk_file, sizeof(chunk_file), "/tmp/code_indexer_chunks_%d.jsonl", worker_id);
    FILE* chunk_fp = fopen(chunk_file, "w");
    if (!chunk_fp) {
        fprintf(stderr, "[Worker %d] Failed to create chunk file\n", worker_id);
        fclose(ctags_fp);
        return;
    }
    
    char line[8192];
    int chunk_count = 0;
    char fullpath[2048];
    char content[32768];
    
    while (fgets(line, sizeof(line), ctags_fp)) {
        char name[256] = {0};
        char filepath[512] = {0};
        char kind[64] = {0};
        char signature[2048] = {0};
        
        char type_check[32] = {0};
        json_extract_str(line, "_type", type_check, sizeof(type_check));
        if (strcmp(type_check, "tag") != 0) continue;
        
        json_extract_str(line, "name", name, sizeof(name));
        json_extract_str(line, "path", filepath, sizeof(filepath));
        json_extract_str(line, "kind", kind, sizeof(kind));
        json_extract_str(line, "signature", signature, sizeof(signature));
        int line_num = json_extract_int(line, "line");
        
        if (!name[0] || !filepath[0] || !line_num) continue;
        
        snprintf(fullpath, sizeof(fullpath), "%s/%s", worker->repo_path, filepath);
        const char* lang = detect_language(filepath);
        
        int end_line = line_num;
        content[0] = '\0';
        
        if (needs_range(kind)) {
            end_line = find_end_line(fullpath, line_num);
            if (end_line > line_num) {
                extract_content(fullpath, line_num, end_line, content, sizeof(content));
            }
        }
        
        if (!content[0] && signature[0]) {
            snprintf(content, sizeof(content), "%s %s;", name, signature);
        }
        
        if (strlen(content) > 16000) {
            content[16000] = '\0';
            char* last_nl = strrchr(content, '\n');
            if (last_nl) *(last_nl + 1) = '\0';
        }
        
        if (strcmp(kind, "member") == 0 || strcmp(kind, "field") == 0 ||
            strcmp(kind, "enumerator") == 0 || strcmp(kind, "variable") == 0 ||
            strcmp(kind, "local") == 0 || strcmp(kind, "parameter") == 0) {
            continue;
        }
        
        write_chunk(chunk_fp, name, filepath, kind, line_num, end_line,
                    lang, signature, content, "");
        chunk_count++;
    }
    
    fclose(ctags_fp);
    fclose(chunk_fp);
    unlink(output_file);
    
    printf("[Worker %d] Done: %d chunks -> %s\n", worker_id, chunk_count, chunk_file);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("High-performance code indexer (C + multiprocess)\n");
        printf("Usage: %s <repo_path> [cache_dir] [num_workers]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s /opt/linux ./ai_code_memory 8\n", argv[0]);
        return 1;
    }
    
    const char* repo_path = argv[1];
    const char* cache_dir = (argc > 2) ? argv[2] : "./ai_code_memory";
    int num_workers = (argc > 3) ? atoi(argv[3]) : sysconf(_SC_NPROCESSORS_ONLN);
    if (num_workers <= 0) num_workers = 4;
    
    printf("Code Indexer v1.0\n");
    printf("==================\n");
    printf("Repo: %s\n", repo_path);
    printf("Cache: %s\n", cache_dir);
    printf("Workers: %d\n\n", num_workers);
    
    struct stat st;
    if (stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s is not a directory\n", repo_path);
        return 1;
    }
    
    printf("Phase 1: Scanning source files...\n");
    source_file_t* files = NULL;
    int file_count = 0;
    int file_capacity = 10000;
    
    files = malloc(file_capacity * sizeof(source_file_t));
    if (!files) {
        fprintf(stderr, "Failed to allocate file list\n");
        return 1;
    }
    
    scan_directory(repo_path, repo_path, &files, &file_count, &file_capacity);
    printf("  Found %d source files\n\n", file_count);
    
    if (file_count == 0) {
        printf("No source files found.\n");
        free(files);
        return 0;
    }
    
    printf("Phase 2: Forking %d workers...\n", num_workers);
    
    worker_t* workers = calloc(num_workers, sizeof(worker_t));
    pid_t* pids = calloc(num_workers, sizeof(pid_t));
    
    for (int i = 0; i < num_workers; i++) {
        workers[i].repo_path = repo_path;
    }
    
    for (int i = 0; i < file_count; i++) {
        int wid = i % num_workers;
        worker_t* w = &workers[wid];
        
        if (w->file_count >= w->capacity) {
            int new_cap = w->capacity == 0 ? 1000 : w->capacity * 2;
            char** new_files = realloc(w->files, new_cap * sizeof(char*));
            if (!new_files) continue;
            w->files = new_files;
            w->capacity = new_cap;
        }
        
        w->files[w->file_count] = strdup(files[i].path);
        w->file_count++;
    }
    
    for (int i = 0; i < num_workers; i++) {
        printf("  Worker %d: %d files\n", i, workers[i].file_count);
    }
    printf("\n");
    
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].file_count == 0) continue;
        
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        } else if (pid == 0) {
            worker_process(&workers[i], i);
            exit(0);
        } else {
            pids[i] = pid;
        }
    }
    
    printf("Phase 3: Waiting for workers...\n");
    int completed = 0;
    for (int i = 0; i < num_workers; i++) {
        if (pids[i] > 0) {
            int status;
            waitpid(pids[i], &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) completed++;
            else fprintf(stderr, "Worker %d failed\n", i);
        }
    }
    printf("  %d/%d workers completed\n\n", completed, num_workers);
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    
    printf("Phase 4: Importing to cache...\n");
    
    cache_t* cache = cache_open(cache_dir, 10ULL * 1024 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
        free(workers); free(files); free(pids);
        return 1;
    }
    
    char ns[512];
    const char* repo_name = strrchr(repo_path, '/');
    if (repo_name) repo_name++;
    else repo_name = repo_path;
    snprintf(ns, sizeof(ns), "/code/local/%s", repo_name);
    
    char meta_key[512], meta_value[1024];
    snprintf(meta_key, sizeof(meta_key), "%s/_meta/info", ns);
    snprintf(meta_value, sizeof(meta_value),
             "{\"type\":\"code_repo\",\"repo_path\":\"%s\",\"languages\":[\"c\"]}",
             repo_path);
    cache_set(cache, meta_key, meta_value, 0);
    
    int total_chunks = 0;
    int total_symbols = 0;
    
    for (int i = 0; i < num_workers; i++) {
        char chunk_file[256];
        snprintf(chunk_file, sizeof(chunk_file), "/tmp/code_indexer_chunks_%d.jsonl", i);
        
        FILE* fp = fopen(chunk_file, "r");
        if (!fp) continue;
        
        char line[65536];
        while (fgets(line, sizeof(line), fp)) {
            char name[256] = {0}, file[512] = {0}, kind[64] = {0};
            char lang[32] = {0}, signature[2048] = {0}, content[32768] = {0};
            
            json_extract_str(line, "name", name, sizeof(name));
            json_extract_str(line, "file", file, sizeof(file));
            json_extract_str(line, "kind", kind, sizeof(kind));
            json_extract_str(line, "language", lang, sizeof(lang));
            json_extract_str(line, "signature", signature, sizeof(signature));
            json_extract_str(line, "content", content, sizeof(content));
            int line_start = json_extract_int(line, "line_start");
            int line_end = json_extract_int(line, "line_end");
            
            if (!name[0]) continue;
            
            char key[1024];
            snprintf(key, sizeof(key), "%s/chunks/%s/%s", ns, file, name);
            
            char value[65536];
            char esc_sig[4096], esc_content[65536];
            json_escape_str(signature, esc_sig, sizeof(esc_sig));
            json_escape_str(content, esc_content, sizeof(esc_content));
            
            snprintf(value, sizeof(value),
                "{\"kind\":\"%s\",\"line_start\":%d,\"line_end\":%d,"
                "\"language\":\"%s\",\"signature\":\"%s\",\"content\":\"%s\"}",
                kind, line_start, line_end, lang, esc_sig, esc_content);
            
            if (strlen(value) < 1024 * 1024) {
                cache_set(cache, key, value, 0);
                total_chunks++;
            }
            
            char sym_key[1024], sym_value[2048];
            snprintf(sym_key, sizeof(sym_key), "%s/symbols/%s", ns, name);
            snprintf(sym_value, sizeof(sym_value),
                "[{\"name\":\"%s\",\"kind\":\"%s\",\"file\":\"%s\",\"line\":%d,\"signature\":\"%s\"}]",
                name, kind, file, line_start, esc_sig);
            cache_set(cache, sym_key, sym_value, 0);
            total_symbols++;
        }
        
        fclose(fp);
        unlink(chunk_file);
    }
    
    cache_sync(cache);
    cache_close(cache);
    
    printf("\n========================================\n");
    printf("Indexing complete!\n");
    printf("  Files: %d\n", file_count);
    printf("  Chunks: %d\n", total_chunks);
    printf("  Symbols: %d\n", total_symbols);
    printf("  Workers: %d\n", num_workers);
    printf("  Time: %.1fs\n", elapsed);
    printf("  Throughput: %.0f files/s\n", file_count / elapsed);
    printf("========================================\n");
    
    for (int i = 0; i < file_count; i++) free(files[i].path);
    free(files);
    for (int i = 0; i < num_workers; i++) {
        for (int j = 0; j < workers[i].file_count; j++) free(workers[i].files[j]);
        free(workers[i].files);
    }
    free(workers);
    free(pids);
    
    return 0;
}
