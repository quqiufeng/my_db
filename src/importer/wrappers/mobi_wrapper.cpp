/**
 * MOBI/AZW3 C Wrapper - 封装 libmobi 为 C 接口供 Python ctypes 调用
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

extern "C" {
#include "mobi.h"

#define API __attribute__((visibility("default")))

// Opaque handle
struct MobiHandle {
    MOBIData* m;
    MOBIRawml* rawml;
    char* text_cache;
    size_t text_cache_len;
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

    return handle;
}

/**
 * 提取纯文本（去除 HTML 标签）
 * @param handle mobi_open 返回的句柄
 * @param out_text 输出文本指针（需调用 mobi_free_text 释放）
 * @param out_len 输出文本长度
 * @return 0 成功，-1 失败
 */
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
    std::string result;
    MOBIPart* parts = h->rawml->flow;
    while (parts) {
        if (parts->data && parts->size > 0) {
            // 简单 HTML 标签去除：跳过 <...> 内容
            const char* data = (const char*)parts->data;
            size_t size = parts->size;
            bool in_tag = false;
            for (size_t i = 0; i < size; i++) {
                char c = data[i];
                if (c == '<') {
                    in_tag = true;
                } else if (c == '>') {
                    in_tag = false;
                } else if (!in_tag) {
                    result += c;
                }
            }
            result += "\n";
        }
        parts = parts->next;
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
