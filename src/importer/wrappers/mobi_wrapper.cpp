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
#include "parse_rawml.h"

// Forward declaration of internal libmobi function
MOBI_RET mobi_get_offset_by_posoff(uint32_t *file_number, size_t *offset, const MOBIRawml *rawml, const size_t pos_fid, const size_t pos_off);

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
    bool in_pre = false;
    
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)data[i];
        
        // Check for style/script start
        if (!in_tag && !in_style && !in_script) {
            size_t remain = size - i;
            if (remain > 6 && strncasecmp(data + i, "<style", 6) == 0 &&
                (data[i+6] == '>' || data[i+6] == ' ')) {
                in_style = true; continue;
            }
            if (remain > 7 && strncasecmp(data + i, "<script", 7) == 0 &&
                (data[i+7] == '>' || data[i+7] == ' ')) {
                in_script = true; continue;
            }
        }
        // Check for style/script end
        if (in_style && c == '<' && size - i > 8 &&
            strncasecmp(data + i, "</style", 7) == 0) {
            in_style = false; i += 7; continue;
        }
        if (in_script && c == '<' && size - i > 9 &&
            strncasecmp(data + i, "</script", 8) == 0) {
            in_script = false; i += 8; continue;
        }
        if (in_style || in_script) continue;
        
        if (c == '<') {
            in_tag = true;
            // Read tag name
            size_t j = i + 1;
            bool is_close = false;
            if (j < size && data[j] == '/') { is_close = true; j++; }
            size_t name_start = is_close ? i + 2 : i + 1;
            while (j < size && data[j] != '>' && data[j] != ' ' && data[j] != '\t') j++;
            std::string tag;
            if (name_start < size && j > name_start && j <= size) {
                tag = std::string(data + name_start, j - name_start);
            }
            for (auto& ch : tag) ch = tolower(ch);
            
            // Find tag end
            const char* end_ptr = nullptr;
            if (i < size) {
                end_ptr = (const char*)memchr(data + i, '>', size - i);
            }
            if (!end_ptr || tag.empty()) {
                in_tag = false;
                continue;
            }
            
            // Heading conversion
            if (tag == "h1" || tag == "h2" || tag == "h3" ||
                tag == "h4" || tag == "h5" || tag == "h6") {
                if (!is_close) {
                    if (!result.empty() && result.back() != '\n') result += '\n';
                    int h = tag[1] - '0';
                    for (int k = 0; k < h; k++) result += '#';
                    result += ' ';
                } else {
                    result += '\n';
                }
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Block elements: wrap with newlines
            if (tag == "p" || tag == "div") {
                if (!is_close && !result.empty() && result.back() != '\n') result += '\n';
                if (is_close) result += '\n';
                i = end_ptr - data; in_tag = false; continue;
            }
            if (tag == "br" || tag == "br/") {
                result += '\n';
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Inline formatting
            if (tag == "b" || tag == "strong") {
                result += is_close ? "**" : "**";
                i = end_ptr - data; in_tag = false; continue;
            }
            if (tag == "i" || tag == "em") {
                result += is_close ? "*" : "*";
                i = end_ptr - data; in_tag = false; continue;
            }
            if (tag == "code" || tag == "tt") {
                result += is_close ? "`" : "`";
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Code block
            if (tag == "pre") {
                in_pre = !is_close;
                if (!is_close) result += "\n```\n";
                else result += "\n```\n";
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Lists
            if (tag == "li") {
                if (!is_close) result += "\n- ";
                i = end_ptr - data; in_tag = false; continue;
            }
            if (tag == "ul" || tag == "ol") {
                if (!is_close && !result.empty() && result.back() != '\n') result += '\n';
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Blockquote
            if (tag == "blockquote") {
                if (!is_close) result += "\n> ";
                else result += "\n";
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Link: just keep [text](url) marker
            if (tag == "a") {
                if (!is_close) {
                    result += "[";
                } else {
                    result += "]";
                }
                i = end_ptr - data; in_tag = false; continue;
            }
            
            // Unknown tag: skip
            i = end_ptr - data;
            in_tag = false;
            continue;
        }
        
        if (c == '>') {
            in_tag = false;
            continue;
        }
        
        // Text content
        if (!in_tag && !in_pre) {
            // HTML entities
            if (c == '&' && size - i > 3) {
                size_t r = size - i;
                if (r >= 5 && strncmp(data + i, "&amp;", 5) == 0) { result += '&'; i += 4; continue; }
                if (r >= 4 && strncmp(data + i, "&lt;", 4) == 0) { result += '<'; i += 3; continue; }
                if (r >= 4 && strncmp(data + i, "&gt;", 4) == 0) { result += '>'; i += 3; continue; }
                if (r >= 6 && strncmp(data + i, "&quot;", 6) == 0) { result += '"'; i += 5; continue; }
                if (r >= 6 && strncmp(data + i, "&nbsp;", 6) == 0) { result += ' '; i += 5; continue; }
                if (r >= 6 && strncmp(data + i, "&apos;", 6) == 0) { result += '\''; i += 5; continue; }
                if (data[i+1] == '#') {
                    char* end = nullptr;
                    long code = strtol(data + i + 2, &end, 10);
                    if (end && *end == ';' && code > 0x20 && code < 0x10000) {
                        char utf8[8]; int len = 0;
                        if (code < 0x80) utf8[len++] = code;
                        else if (code < 0x800) { utf8[len++] = 0xC0 | (code >> 6); utf8[len++] = 0x80 | (code & 0x3F); }
                        else { utf8[len++] = 0xE0 | (code >> 12); utf8[len++] = 0x80 | ((code >> 6) & 0x3F); utf8[len++] = 0x80 | (code & 0x3F); }
                        result.append(utf8, len);
                        i = end - data; continue;
                    }
                }
            }
            result += c;
        } else if (in_pre) {
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

    // 辅助函数：跳过 UTF-8 续字节，找到有效字符起始
    auto skip_utf8_continuation = [](const char* data, size_t size) -> size_t {
        size_t offset = 0;
        while (offset < size) {
            unsigned char c = (unsigned char)data[offset];
            // 如果是续字节 (0x80-0xBF)，跳过
            if ((c & 0xC0) == 0x80) {
                offset++;
            } else {
                break;
            }
        }
        return offset;
    };
    
    // 收集所有文本部分
    std::string raw_text;
    
    // 首先尝试从 markup 链表提取（AZW3/KF8 格式）
    MOBIPart* parts = h->rawml->markup;
    if (parts) {
        while (parts) {
            if (parts->data && parts->size > 0) {
                size_t skip = skip_utf8_continuation((const char*)parts->data, parts->size);
                extract_text_from_html((const char*)parts->data + skip, parts->size - skip, raw_text);
            }
            parts = parts->next;
        }
    }
    
    // 如果没有 markup，或者内容很少，再从 flow 提取
    if (raw_text.length() < 1000) {
        parts = h->rawml->flow;
        while (parts) {
            if (parts->data && parts->size > 0) {
                size_t skip = skip_utf8_continuation((const char*)parts->data, parts->size);
                extract_text_from_html((const char*)parts->data + skip, parts->size - skip, raw_text);
            }
            parts = parts->next;
        }
    }
    
    // 保留完整文本（包括目录、推荐序等），由 import_book 自行识别章节边界
    std::string result = raw_text;

    // 分配并缓存结果（支持中间包含 \0 的文本）
    h->text_cache_len = result.length();
    h->text_cache = (char*)malloc(h->text_cache_len + 1);
    if (!h->text_cache) return -1;

    // 逐字节复制，避免 std::string::c_str() 在 \0 处截断
    for (size_t i = 0; i < h->text_cache_len; i++) {
        h->text_cache[i] = result[i];
    }
    h->text_cache[h->text_cache_len] = '\0';
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
        // 注意：exth->data 是原始字节，不保证 NUL 结尾，必须按 exth->size 截断
        const MOBIExthHeader* exth = mobi_get_exthrecord_by_tag(h->m, EXTH_AUTHOR);
        if (exth && exth->data && exth->size > 0) {
            size_t n = exth->size < author_len - 1 ? exth->size : author_len - 1;
            memcpy(author, exth->data, n);
            author[n] = '\0';
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
            // KF8/AZW3: posfid/posoff 是 part 内部偏移，不是全文拼接后的字节偏移
            // 在 full-text 模式下无法直接使用，设为 0 表示不可靠
            offset = 0;
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
        
        // KF8: 使用 libmobi 的 mobi_get_offset_by_posoff 获取实际的 part UID 和字节偏移
        uint32_t file_nr = 0;
        size_t actual_offset = 0;
        if (mobi_get_offset_by_posoff(&file_nr, &actual_offset, h->rawml, posfid, posoff) == MOBI_SUCCESS) {
            posfid = file_nr;
            posoff = actual_offset;
        }
        
        // 获取下一章节的偏移作为结束位置
        if (chapter_index + 1 < h->chapter_count) {
            const MOBIIndexEntry* next_entry = &h->rawml->ncx->entries[chapter_index + 1];
            uint32_t next_fid = 0, next_off = 0;
            mobi_get_indxentry_tagvalue(&next_fid, next_entry, tag_ncx_posfid);
            mobi_get_indxentry_tagvalue(&next_off, next_entry, tag_ncx_posoff);
            
            // 同样转换 next_fid
            uint32_t next_file_nr = 0;
            size_t next_actual_offset = 0;
            if (mobi_get_offset_by_posoff(&next_file_nr, &next_actual_offset, h->rawml, next_fid, next_off) == MOBI_SUCCESS) {
                next_fid = next_file_nr;
                next_off = next_actual_offset;
            }
            
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
    
    // 获取对应的 markup 部分（直接遍历 markup 链表，如找不到则查找 flow 链表）
    MOBIPart* part = h->rawml->markup;
    while (part) {
        if (part->uid == posfid) {
            break;
        }
        part = part->next;
    }
    if (!part || !part->data || part->size == 0) {
        part = h->rawml->flow;
        while (part) {
            if (part->uid == posfid) {
                break;
            }
            part = part->next;
        }
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
    
    // 修复：如果 posoff 落在 HTML 标签中间，跳到标签结束处
    {
        size_t i = posoff;
        // 向前查找最近的 <
        size_t tag_start = posoff;
        while (tag_start > 0 && data[tag_start] != '<') tag_start--;
        if (data[tag_start] == '<') {
            // 向前查找 >
            size_t tag_end = posoff;
            while (tag_end < data_size && data[tag_end] != '>') tag_end++;
            if (tag_end < data_size && data[tag_end] == '>' && posoff > tag_start && posoff < tag_end) {
                // posoff 在 <...> 内部，跳到 > 之后
                posoff = tag_end + 1;
            }
        }
    }
    
    // 修复 UTF-8：确保 posoff 在有效的 UTF-8 字符起始位置
    while (posoff < data_size && posoff < end_offset) {
        unsigned char c = (unsigned char)data[posoff];
        // 如果当前字节是 UTF-8 续字节 (0x80-0xBF)，向前移动到字符起始
        if ((c & 0xC0) == 0x80) {
            posoff++;
        } else {
            break;
        }
    }
    
    // 关键：上面的标签/UTF-8 修复可能把 posoff 推过 end_offset，
    // 此时 end_offset - posoff 作为 size_t 会下溢成巨大值，导致越界读崩溃
    if (posoff >= end_offset) {
        *out_text = nullptr;
        *out_len = 0;
        return -1;
    }
    
    size_t text_len = end_offset - posoff;
    if (text_len == 0) {
        *out_text = nullptr;
        *out_len = 0;
        return -1;
    }
    
    // Convert to Markdown using shared extract_text_from_html
    std::string result;
    extract_text_from_html(data + posoff, end_offset - posoff, result);
    
    if (result.empty()) {
        *out_text = nullptr;
        *out_len = 0;
        return -1;
    }
    
    *out_len = result.length();
    *out_text = (char*)malloc(*out_len + 1);
    if (!*out_text) return -1;
    
    for (size_t i = 0; i < *out_len; i++) {
        (*out_text)[i] = result[i];
    }
    (*out_text)[*out_len] = '\0';
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
