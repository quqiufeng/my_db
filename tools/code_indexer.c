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
        strcmp(kind, "namespace") == 0
    );
}

static int find_end_line(const char* filepath, int start_line) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return start_line;
    
    int current_line = 0;
    int brace_depth = 0;
    int in_string = 0;
    int string_char = 0;
    int found_open = 0;
    char line[4096];
    
    // Phase 1: read lines from start_line, looking for '{'
    while (fgets(line, sizeof(line), fp)) {
        current_line++;
        if (current_line < start_line) continue;
        
        // Check if this is a declaration (ends with ; before any {)
        char* semicolon = strchr(line, ';');
        char* brace = strchr(line, '{');
        
        if (!found_open && semicolon && (!brace || semicolon < brace)) {
            // It's a declaration, skip
            fclose(fp);
            return 0;  // 0 means no body (declaration only)
        }
        
        // Process the line character by character
        char* p = line;
        while (*p) {
            if (!in_string) {
                if (*p == '"' || *p == '\'') {
                    in_string = 1; string_char = *p;
                } else if (*p == '/' && *(p+1) == '/') {
                    break;  // C++ style comment
                } else if (*p == '/' && *(p+1) == '*') {
                    in_string = 2;  // Multi-line comment start
                } else if (*p == '{') {
                    brace_depth++;
                    found_open = 1;
                } else if (*p == '}') {
                    if (brace_depth > 0) {
                        brace_depth--;
                        if (brace_depth == 0 && found_open) {
                            fclose(fp);
                            return current_line;
                        }
                    }
                }
            } else if (in_string == 2) {
                if (*p == '*' && *(p+1) == '/') { in_string = 0; p++; }
            } else {
                if (*p == string_char && *(p-1) != '\\') in_string = 0;
            }
            p++;
        }
        
        // Safety limits
        if (!found_open && current_line > start_line + 10) {
            // No opening brace found within 10 lines, likely a declaration
            fclose(fp);
            return 0;
        }
        if (found_open && current_line > start_line + 500) {
            fclose(fp);
            return current_line;
        }
    }
    
    fclose(fp);
    return current_line;
}

static int extract_content_fp(FILE* fp, int start_line, int end_line, 
                                char* out, int max_len) {
    if (!fp) return 0;
    
    int current_line = 0;
    int pos = 0;
    char line[4096];
    
    // Reset to beginning if we need to read from earlier lines
    rewind(fp);
    
    while (fgets(line, sizeof(line), fp) && current_line < end_line) {
        current_line++;
        if (current_line < start_line) continue;
        int len = strlen(line);
        if (pos + len < max_len - 1) {
            memcpy(out + pos, line, len);
            pos += len;
        } else break;
    }
    
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

static void write_chunk(FILE* fp, FILE* text_fp, FILE* meta_fp,
                        const char* name, const char* file,
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

    // Write content as plain text (one line per chunk, for vector generation)
    if (text_fp) {
        // Replace newlines with spaces for single-line format
        for (const char* p = content; *p; p++) {
            fputc((*p == '\n' || *p == '\r') ? ' ' : *p, text_fp);
        }
        fputc('\n', text_fp);
    }

    // Write metadata (for search)
    if (meta_fp) {
        fprintf(meta_fp, "{\"name\":\"%s\",\"file\":\"%s\",\"kind\":\"%s\","
                "\"line_start\":%d,\"line_end\":%d,\"language\":\"%s\","
                "\"signature\":\"%s\",\"content\":\"%s\",\"docstring\":\"%s\"}\n",
                esc_name, esc_file, esc_kind, line_start, line_end, lang, esc_sig, esc_content, esc_doc);
    }
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
        "ctags-universal --output-format=json --fields=+nKzSe "
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

    char text_file[256];
    snprintf(text_file, sizeof(text_file), "/tmp/code_indexer_text_%d.txt", worker_id);
    FILE* text_fp = fopen(text_file, "w");

    char meta_file[256];
    snprintf(meta_file, sizeof(meta_file), "/tmp/code_indexer_meta_%d.jsonl", worker_id);
    FILE* meta_fp = fopen(meta_file, "w");

    char line[131072];
    int chunk_count = 0;
    char fullpath[2048];
    char content[32768];

    // File cache: for small files (<2MB), load entire content into memory
    // For large files, use FILE* handle cache
    FILE* current_fp = NULL;
    char current_file[512] = {0};
    char* file_buffer = NULL;
    size_t file_buffer_size = 0;
    char** file_lines = NULL;
    int file_line_count = 0;
    
    while (fgets(line, sizeof(line), ctags_fp)) {
        char name[256] = {0};
        char filepath[512] = {0};
        char kind[64] = {0};
        char signature[2048] = {0};
        
        char type_check[32] = {0};
        json_extract_str(line, "_type", type_check, sizeof(type_check));
        if (strcmp(type_check, "tag") != 0) continue;
        
        json_extract_str(line, "name", name, sizeof(name));
        
        // For scoped symbols (class/struct members), prefix with scope
        char scope[256] = {0};
        json_extract_str(line, "scope", scope, sizeof(scope));
        if (scope[0]) {
            char scoped_name[512];
            snprintf(scoped_name, sizeof(scoped_name), "%s::%s", scope, name);
            strncpy(name, scoped_name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
        
        json_extract_str(line, "path", filepath, sizeof(filepath));
        json_extract_str(line, "kind", kind, sizeof(kind));
        json_extract_str(line, "signature", signature, sizeof(signature));
        int line_num = json_extract_int(line, "line");
        int end_line = json_extract_int(line, "end");
        
        if (!name[0] || !filepath[0] || !line_num) continue;
        
        if (filepath[0] == '/') {
            strncpy(fullpath, filepath, sizeof(fullpath) - 1);
            fullpath[sizeof(fullpath) - 1] = '\0';
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", worker->repo_path, filepath);
        }
        const char* lang = detect_language(filepath);
        
        content[0] = '\0';
        
        if (needs_range(kind)) {
            if (end_line <= line_num) {
                // ctags didn't provide end, fall back to our own parser
                if (current_fp) { fclose(current_fp); current_fp = NULL; current_file[0] = '\0'; }
                if (file_buffer) { free(file_buffer); file_buffer = NULL; }
                if (file_lines) { free(file_lines); file_lines = NULL; }
                end_line = find_end_line(fullpath, line_num);
                if (end_line == 0) end_line = line_num;
            }
            if (end_line > line_num) {
                // Check if we need to load a new file
                if (strcmp(filepath, current_file) != 0) {
                    // Clean up old cache
                    if (current_fp) { fclose(current_fp); current_fp = NULL; }
                    if (file_buffer) { free(file_buffer); file_buffer = NULL; }
                    if (file_lines) { free(file_lines); file_lines = NULL; }
                    file_line_count = 0;
                    
                    strncpy(current_file, filepath, sizeof(current_file) - 1);
                    current_file[sizeof(current_file) - 1] = '\0';
                    
                    // Try to load small file into memory
                    struct stat st;
                    if (stat(fullpath, &st) == 0 && st.st_size > 0 && st.st_size < 2*1024*1024) {
                        FILE* fp = fopen(fullpath, "r");
                        if (fp) {
                            file_buffer = malloc(st.st_size + 1);
                            if (file_buffer) {
                                size_t n = fread(file_buffer, 1, st.st_size, fp);
                                file_buffer[n] = '\0';
                                file_buffer_size = n;
                                
                                // Count lines and build line index
                                int count = 1; // at least 1 line
                                for (size_t i = 0; i < n; i++) {
                                    if (file_buffer[i] == '\n') count++;
                                }
                                file_lines = malloc(count * sizeof(char*));
                                if (file_lines) {
                                    file_lines[0] = file_buffer;
                                    int idx = 1;
                                    for (size_t i = 0; i < n && idx < count; i++) {
                                        if (file_buffer[i] == '\n') {
                                            file_buffer[i] = '\0';
                                            file_lines[idx] = file_buffer + i + 1;
                                            idx++;
                                        }
                                    }
                                    file_line_count = count;
                                }
                            }
                            fclose(fp);
                        }
                    } else {
                        // Large file: use FILE* handle
                        current_fp = fopen(fullpath, "r");
                    }
                }
                
                // Extract content from memory buffer or FILE*
                if (file_lines && line_num <= file_line_count) {
                    int pos = 0;
                    for (int l = line_num; l <= end_line && l <= file_line_count; l++) {
                        int len = strlen(file_lines[l-1]);
                        if (pos + len < sizeof(content) - 2) {
                            memcpy(content + pos, file_lines[l-1], len);
                            pos += len;
                            content[pos++] = '\n';
                        }
                    }
                    if (pos > 0) content[pos] = '\0';
                } else if (current_fp) {
                    extract_content_fp(current_fp, line_num, end_line, content, sizeof(content));
                }
            }
        } else {
            end_line = line_num;
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
            strcmp(kind, "local") == 0 || strcmp(kind, "parameter") == 0 ||
            strcmp(kind, "macro") == 0) {
            continue;
        }
        
        // Sliding window chunking for long functions
        int content_len = strlen(content);
        if (content_len > 2000) {
            int window_size = 1500;
            int overlap = 500;
            int offset = 0;
            int part = 0;
            
            while (offset < content_len) {
                char chunk_content[4096];
                int remaining = content_len - offset;
                int take = (remaining > window_size) ? window_size : remaining;
                
                // Build chunk: signature + " // Part N" + content slice
                if (signature[0]) {
                    snprintf(chunk_content, sizeof(chunk_content), "%s // Part %d\n", 
                             signature, part + 1);
                } else {
                    snprintf(chunk_content, sizeof(chunk_content), "%s // Part %d\n", 
                             name, part + 1);
                }
                
                int prefix_len = strlen(chunk_content);
                int max_content = sizeof(chunk_content) - prefix_len - 1;
                if (take > max_content) take = max_content;
                
                memcpy(chunk_content + prefix_len, content + offset, take);
                chunk_content[prefix_len + take] = '\0';
                
                // Find last newline to avoid cutting mid-line
                if (offset + take < content_len) {
                    char* last_nl = strrchr(chunk_content, '\n');
                    if (last_nl) {
                        *(last_nl + 1) = '\0';
                        take = (last_nl - chunk_content) - prefix_len + 1;
                    }
                }
                
                write_chunk(chunk_fp, text_fp, meta_fp, name, filepath, kind, 
                           line_num, end_line, lang, signature, chunk_content, "");
                chunk_count++;
                
                offset += (take > overlap) ? (take - overlap) : take;
                part++;
                
                if (part >= 5) break; // Max 5 parts per function
            }
        } else {
            write_chunk(chunk_fp, text_fp, meta_fp, name, filepath, kind, line_num, end_line,
                        lang, signature, content, "");
            chunk_count++;
        }
    }

    if (current_fp) fclose(current_fp);
    if (file_buffer) free(file_buffer);
    if (file_lines) free(file_lines);
    fclose(ctags_fp);
    fclose(chunk_fp);
    if (text_fp) fclose(text_fp);
    if (meta_fp) fclose(meta_fp);
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

    // Create cache directory if it doesn't exist
    if (stat(cache_dir, &st) != 0) {
        if (mkdir(cache_dir, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create cache directory %s\n", cache_dir);
            return 1;
        }
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
    
    printf("Phase 4: Merging output files...\n");

    // Merge text files
    char text_out[512];
    snprintf(text_out, sizeof(text_out), "%s/chunks_text.txt", cache_dir);
    FILE* text_out_fp = fopen(text_out, "w");

    // Merge meta files
    char meta_out[512];
    snprintf(meta_out, sizeof(meta_out), "%s/chunks_meta.jsonl", cache_dir);
    FILE* meta_out_fp = fopen(meta_out, "w");

    int total_chunks = 0;

    for (int i = 0; i < num_workers; i++) {
        char text_file[256], meta_file[256];
        snprintf(text_file, sizeof(text_file), "/tmp/code_indexer_text_%d.txt", i);
        snprintf(meta_file, sizeof(meta_file), "/tmp/code_indexer_meta_%d.jsonl", i);

        // Copy text file
        FILE* fp = fopen(text_file, "r");
        if (fp) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                fwrite(buf, 1, n, text_out_fp);
            }
            fclose(fp);
            unlink(text_file);
        }

        // Copy meta file
        fp = fopen(meta_file, "r");
        if (fp) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                fwrite(buf, 1, n, meta_out_fp);
            }
            fclose(fp);
            unlink(meta_file);
        }

        // Count chunks from chunk file
        char chunk_file[256];
        snprintf(chunk_file, sizeof(chunk_file), "/tmp/code_indexer_chunks_%d.jsonl", i);
        fp = fopen(chunk_file, "r");
        if (fp) {
            char line[131072];
            while (fgets(line, sizeof(line), fp)) {
                total_chunks++;
            }
            fclose(fp);
            // Keep chunk file for now (may be useful for debugging)
        }
    }

    if (text_out_fp) fclose(text_out_fp);
    if (meta_out_fp) fclose(meta_out_fp);
    
    printf("\n========================================\n");
    printf("Indexing complete!\n");
    printf("  Files: %d\n", file_count);
    printf("  Chunks: %d\n", total_chunks);
    printf("  Workers: %d\n", num_workers);
    printf("  Time: %.1fs\n", elapsed);
    printf("  Throughput: %.0f files/s\n", file_count / elapsed);
    printf("  Output: %s/chunks_text.txt, %s/chunks_meta.jsonl\n", cache_dir, cache_dir);
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
