/**
 * MOBI/AZW3 C Wrapper - 封装 libmobi 为 C 接口供 Python ctypes 调用
 * 支持章节提取
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "mobi.h"
#include "index.h"

#define API __attribute__((visibility("default")))

// 章节信息结构
struct MobiChapter {
    char* title;
    int level;
    size_t offset;
};

// Opaque handle
struct MobiHandle {
    MOBIData* m;
    MOBIRawml* rawml;
    char* text_cache;
    size_t text_cache_len;
    MobiChapter* chapters;
    int chapter_count;
};

/**
 * 打开 MOBI 文件
 * @param path 文件路径
 * @return handle（NULL 表示失败）
 */
API void* mobi_open(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[MOBI] 无法打开文件: %s\n", path);
        return nullptr;
    }

    MOBIData* m = mobi_init();
    if (!m) {
        fclose(file);
        return nullptr;
    }

    MOBI_RET ret = mobi_load_file(m, file);
    fclose(file);
    if (ret != MOBI_SUCCESS) {
        fprintf(stderr, "[MOBI] 加载文件失败: %s\n", path);
        mobi_free(m);
        return nullptr;
    }

    MOBIRawml* rawml = mobi_init_rawml(m);
    if (!rawml) {
        mobi_free(m);
        return nullptr;
    }

    ret = mobi_parse_rawml(rawml, m);
    if (ret != MOBI_SUCCESS) {
        mobi_free_rawml(rawml);
        mobi_free(m);
        return nullptr;
    }

    MobiHandle* handle = new MobiHandle();
    handle->m = m;
    handle->rawml = rawml;
    handle->text_cache = nullptr;
    handle->text_cache_len = 0;
    handle->chapters = nullptr;
    handle->chapter_count = 0;

    return handle;
}

/**
 * 提取纯文本（去除 HTML 标签）
 * @param handle mobi_open 返回的句柄
 * @param out_text 输出文本指针（需调用 mobi_free_text 释放）
 * @param out_len 输出文本长度
 * @return 0 成功，-1 失败
 */
// 辅助函数：从 HTML 数据中提取纯文本
static void extract_text_from_html(const char* data, size_t size, std::string& result) {
    bool in_tag = false;
    bool in_style = false;
    bool in_script = false;
    
    for (size_t i = 0; i < size; i++) {
        char c = data[i];
        
        // 检查是否进入 style 标签
        if (!in_tag && i + 6 < size && strncasecmp(data + i, "<style", 6) == 0) {
            in_style = true;
        }
        if (in_style && i + 7 < size && strncasecmp(data + i, "</style", 7) == 0) {
            in_style = false;
            in_tag = true;  // 跳过 </style>
            continue;
        }
        
        // 检查是否进入 script 标签
        if (!in_tag && i + 7 < size && strncasecmp(data + i, "<script", 7) == 0) {
            in_script = true;
        }
        if (in_script && i + 8 < size && strncasecmp(data + i, "</script", 8) == 0) {
            in_script = false;
            in_tag = true;  // 跳过 </script>
            continue;
        }
        
        if (in_style || in_script) continue;
        
        if (c == '<') {
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
            result += ' ';
        } else if (!in_tag) {
            result += c;
        }
    }
    result += "\n";
}

API int mobi_extract_text(void* handle, char** out_text, size_t* out_len) {
    MobiHandle* h = static_cast<MobiHandle*>(handle);
    if (!h || !h->rawml) return -1;

    // 如果已有缓存，直接返回
    if (h->text_cache) {
        *out_text = h->text_cache;
        *out_len = h->text_cache_len;
        return 0;
    }

    // 收集所有文本部分
    std::string raw_text;
    
    // 首先尝试从 markup 链表提取（AZW3/KF8 格式）
    MOBIPart* parts = h->rawml->markup;
    if (parts) {
        while (parts) {
            if (parts->data && parts->size > 0) {
                extract_text_from_html((const char*)parts->data, parts->size, raw_text);
            }
            parts = parts->next;
        }
    }
    
    // 如果没有 markup，或者内容很少，再从 flow 提取
    if (raw_text.length() < 1000) {
        parts = h->rawml->flow;
        while (parts) {
            if (parts->data && parts->size > 0) {
                extract_text_from_html((const char*)parts->data, parts->size, raw_text);
            }
            parts = parts->next;
        }
    }
    
    // 删除目录部分
    std::string result = raw_text;
    const char* text = raw_text.c_str();
    
    // 删除目录部分
    // 方法：查找 "第一章" 的所有出现位置
    // 目录中的章节标题间距很短，正文中的间距很长
    const char* first = strstr(text, "第一章");
    if (first) {
        const char* second = strstr(first + 1, "第一章");
        if (second) {
            size_t dist = second - first;
            if (dist > 10000) {
                // 第一个在目录中，第二个在正文中
                result = second;
            }
        }
    }

    // 分配并缓存结果
    h->text_cache_len = result.length();
    h->text_cache = (char*)malloc(h->text_cache_len + 1);
    if (!h->text_cache) return -1;

    memcpy(h->text_cache, result.c_str(), h->text_cache_len + 1);
    *out_text = h->text_cache;
    *out_len = h->text_cache_len;
    return 0;
}

/**
 * 获取元数据
 * @param handle mobi_open 返回的句柄
 * @param title 标题缓冲区
 * @param title_len 标题缓冲区大小
 * @param author 作者缓冲区
 * @param author_len 作者缓冲区大小
 * @return 0 成功，-1 失败
 */
API int mobi_get_metadata(void* handle,
                          char* title, size_t title_len,
                          char* author, size_t author_len) {
    MobiHandle* h = static_cast<MobiHandle*>(handle);
    if (!h || !h->m) return -1;

    if (title && title_len > 0) {
        char full_name[512];
        if (mobi_get_fullname(h->m, full_name, sizeof(full_name)) == MOBI_SUCCESS) {
            strncpy(title, full_name, title_len - 1);
            title[title_len - 1] = '\0';
        } else {
            title[0] = '\0';
        }
    }

    if (author && author_len > 0) {
        // libmobi 的 EXTH 记录中，EXTH_AUTHOR 是作者
        const MOBIExthHeader* exth = mobi_get_exthrecord_by_tag(h->m, EXTH_AUTHOR);
        if (exth && exth->data) {
            strncpy(author, (const char*)exth->data, author_len - 1);
            author[author_len - 1] = '\0';
        } else {
            author[0] = '\0';
        }
    }

    return 0;
}

/**
 * 提取章节列表
 * @param handle mobi_open 返回的句柄
 * @param out_chapters 输出章节数组（调用者需用 mobi_free_chapters 释放）
 * @param out_count 输出章节数量
 * @return 0 成功，-1 失败
 */
API int mobi_get_chapters(void* handle, MobiChapter** out_chapters, int* out_count) {
    MobiHandle* h = static_cast<MobiHandle*>(handle);
    if (!h || !h->rawml) return -1;

    // 如果已提取过，直接返回
    if (h->chapters) {
        *out_chapters = h->chapters;
        *out_count = h->chapter_count;
        return 0;
    }

    if (!h->rawml->ncx || h->rawml->ncx->entries_count == 0) {
        *out_count = 0;
        *out_chapters = nullptr;
        return 0;
    }

    const size_t count = h->rawml->ncx->entries_count;
    h->chapters = (MobiChapter*)malloc(sizeof(MobiChapter) * count);
    if (!h->chapters) return -1;

    const MOBIPdbRecord* cncx_record = h->rawml->ncx->cncx_record;
    
    size_t chapter_idx = 0;
    for (size_t i = 0; i < count && chapter_idx < count; i++) {
        const MOBIIndexEntry* ncx_entry = &h->rawml->ncx->entries[i];
        
        // 获取章节标题
        uint32_t cncx_offset;
        unsigned int tag_ncx_text[] = {3, 0};
        if (mobi_get_indxentry_tagvalue(&cncx_offset, ncx_entry, tag_ncx_text) != MOBI_SUCCESS) {
            continue;
        }
        
        char* text = mobi_get_cncx_string_utf8(cncx_record, cncx_offset, h->rawml->ncx->encoding);
        if (!text) continue;

        // 获取层级
        uint32_t level = 0;
        unsigned int tag_ncx_level[] = {4, 0};
        mobi_get_indxentry_tagvalue(&level, ncx_entry, tag_ncx_level);

        // 获取位置偏移
        size_t offset = 0;
        if (mobi_is_rawml_kf8(h->rawml)) {
            uint32_t posfid, posoff;
            unsigned int tag_ncx_posfid[] = {6, 0};
            unsigned int tag_ncx_posoff[] = {6, 1};
            if (mobi_get_indxentry_tagvalue(&posfid, ncx_entry, tag_ncx_posfid) == MOBI_SUCCESS &&
                mobi_get_indxentry_tagvalue(&posoff, ncx_entry, tag_ncx_posoff) == MOBI_SUCCESS) {
                // 简化：使用 file 编号作为大致位置
                offset = posfid * 10000 + posoff;
            }
        } else {
            uint32_t filepos;
            unsigned int tag_ncx_filepos[] = {1, 0};
            if (mobi_get_indxentry_tagvalue(&filepos, ncx_entry, tag_ncx_filepos) == MOBI_SUCCESS) {
                offset = filepos;
            }
        }

        h->chapters[chapter_idx].title = text;
        h->chapters[chapter_idx].level = (int)level;
        h->chapters[chapter_idx].offset = offset;
        chapter_idx++;
    }

    h->chapter_count = (int)chapter_idx;
    *out_chapters = h->chapters;
    *out_count = h->chapter_count;
    
    return 0;
}

/**
 * 提取指定章节的文本
 * @param handle mobi_open 返回的句柄
 * @param chapter_index 章节索引
 * @param out_text 输出文本指针（需用 free 释放）
 * @param out_len 输出文本长度
 * @return 0 成功，-1 失败
 */
API int mobi_get_chapter_text(void* handle, int chapter_index, char** out_text, size_t* out_len) {
    MobiHandle* h = static_cast<MobiHandle*>(handle);
    if (!h || !h->rawml || !h->rawml->ncx) return -1;
    
    // 如果章节数据未初始化，先获取
    if (h->chapter_count == 0) {
        MobiChapter* chapters = NULL;
        int count = 0;
        mobi_get_chapters(handle, &chapters, &count);
        // chapters 会被存储在 h->chapters 中
    }
    
    if (chapter_index < 0 || chapter_index >= h->chapter_count) return -1;
    
    const MOBIIndexEntry* ncx_entry = &h->rawml->ncx->entries[chapter_index];
    
    uint32_t posfid = 0, posoff = 0;
    size_t end_offset = 0;
    
    if (mobi_is_rawml_kf8(h->rawml)) {
        unsigned int tag_ncx_posfid[] = {6, 0};
        unsigned int tag_ncx_posoff[] = {6, 1};
        mobi_get_indxentry_tagvalue(&posfid, ncx_entry, tag_ncx_posfid);
        mobi_get_indxentry_tagvalue(&posoff, ncx_entry, tag_ncx_posoff);
        
        // 获取下一章节的偏移作为结束位置
        if (chapter_index + 1 < h->chapter_count) {
            const MOBIIndexEntry* next_entry = &h->rawml->ncx->entries[chapter_index + 1];
            uint32_t next_fid = 0, next_off = 0;
            mobi_get_indxentry_tagvalue(&next_fid, next_entry, tag_ncx_posfid);
            mobi_get_indxentry_tagvalue(&next_off, next_entry, tag_ncx_posoff);
            
            if (next_fid == posfid) {
                end_offset = next_off;
            }
        }
    } else {
        unsigned int tag_ncx_filepos[] = {1, 0};
        mobi_get_indxentry_tagvalue(&posoff, ncx_entry, tag_ncx_filepos);
        
        if (chapter_index + 1 < h->chapter_count) {
            const MOBIIndexEntry* next_entry = &h->rawml->ncx->entries[chapter_index + 1];
            uint32_t next_off = 0;
            mobi_get_indxentry_tagvalue(&next_off, next_entry, tag_ncx_filepos);
            end_offset = next_off;
        }
    }
    
    // 获取对应的 markup 部分（直接遍历 markup 链表）
    MOBIPart* part = h->rawml->markup;
    while (part) {
        if (part->uid == posfid) {
            break;
        }
        part = part->next;
    }
    if (!part || !part->data || part->size == 0) {
        return -1;
    }
    
    // 提取文本
    const char* data = (const char*)part->data;
    size_t data_size = part->size;
    
    if (posoff >= data_size) posoff = 0;
    if (end_offset == 0 || end_offset > data_size || end_offset <= posoff) {
        end_offset = data_size;
    }
    
    size_t text_len = end_offset - posoff;
    
    // 转换为纯文本
    std::string result;
    bool in_tag = false;
    bool in_style = false;
    
    for (size_t i = posoff; i < end_offset && i < data_size; i++) {
        char c = data[i];
        
        if (!in_tag && i + 6 < data_size && strncasecmp(data + i, "<style", 6) == 0) {
            in_style = true;
        }
        if (in_style && i + 7 < data_size && strncasecmp(data + i, "</style", 7) == 0) {
            in_style = false;
            in_tag = true;
            continue;
        }
        
        if (in_style) continue;
        
        if (c == '<') {
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
            result += ' ';
        } else if (!in_tag) {
            result += c;
        }
    }
    
    if (result.empty()) {
        *out_text = nullptr;
        *out_len = 0;
        return -1;
    }
    
    *out_len = result.length();
    *out_text = (char*)malloc(*out_len + 1);
    if (!*out_text) return -1;
    
    memcpy(*out_text, result.c_str(), *out_len + 1);
    return 0;
}

/**
 * 释放章节文本
 */
API void mobi_free_chapter_text(char* text) {
    free(text);
}

/**
 * 释放章节数组
 * @param chapters mobi_get_chapters 返回的数组
 * @param count 章节数量
 */
API void mobi_free_chapters(MobiChapter* chapters, int count) {
    if (!chapters) return;
    for (int i = 0; i < count; i++) {
        free(chapters[i].title);
    }
    free(chapters);
}

/**
 * 释放 mobi_extract_text 返回的文本
 * @param text 文本指针
 */
API void mobi_free_text(char* text) {
    // 实际释放由 mobi_close 统一处理缓存
    (void)text;
}

/**
 * 关闭 MOBI 文件并释放资源
 * @param handle mobi_open 返回的句柄
 */
API void mobi_close(void* handle) {
    MobiHandle* h = static_cast<MobiHandle*>(handle);
    if (!h) return;

    if (h->chapters) {
        mobi_free_chapters(h->chapters, h->chapter_count);
    }
    if (h->text_cache) {
        free(h->text_cache);
    }
    if (h->rawml) {
        mobi_free_rawml(h->rawml);
    }
    if (h->m) {
        mobi_free(h->m);
    }
    delete h;
}

} // extern "C"
