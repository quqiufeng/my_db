/**
 * EPUB C Wrapper - 使用 libzip + libxml2 解析 EPUB 电子书
 * 暴露与 mobi/pdf 一致的 C ABI 接口
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <string>

#include <zip.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/xpath.h>

#define API __attribute__((visibility("default")))

extern "C" {

// 章节信息结构（与 MOBI/PDF 保持一致）
struct EpubChapter {
    char* title;
    int level;
    size_t offset;  // 在合并后文本中的字节偏移
};

} // extern "C"

// Opaque handle
struct EpubHandle {
    zip_t* za;
    char* text_cache;
    size_t text_cache_len;
    EpubChapter* chapters;
    int chapter_count;
    char title[256];
    char author[256];
};

// =============================================================================
// 内部工具函数
// =============================================================================

// 从 ZIP 中读取文件内容为字符串
static char* zip_read_file(zip_t* za, const char* path, size_t* out_len) {
    zip_stat_t st;
    if (zip_stat(za, path, 0, &st) != 0) return nullptr;
    
    zip_file_t* zf = zip_fopen(za, path, 0);
    if (!zf) return nullptr;
    
    char* buf = (char*)malloc(st.size + 1);
    if (!buf) {
        zip_fclose(zf);
        return nullptr;
    }
    
    zip_int64_t read = zip_fread(zf, buf, st.size);
    zip_fclose(zf);
    
    if (read < 0 || (zip_uint64_t)read != st.size) {
        free(buf);
        return nullptr;
    }
    
    buf[st.size] = '\0';
    if (out_len) *out_len = st.size;
    return buf;
}

// 获取 XML 节点的文本内容
static std::string xml_node_text(xmlNodePtr node) {
    std::string result;
    for (xmlNodePtr child = node->children; child; child = child->next) {
        if (child->type == XML_TEXT_NODE && child->content) {
            result += (const char*)child->content;
        }
    }
    return result;
}

// 递归提取 HTML 中的纯文本
// 递归提取 HTML 中的 Markdown 格式文本
static void extract_text_from_html(xmlNodePtr node, std::string& out) {
    if (!node) return;
    
    // 跳过 script 和 style
    if (node->type == XML_ELEMENT_NODE) {
        const char* name = (const char*)node->name;
        if (name && (strcasecmp(name, "script") == 0 || strcasecmp(name, "style") == 0)) {
            return;
        }
    }
    
    // === 前置处理：添加 Markdown 标记 ===
    bool is_pre = false;
    bool is_li = false;
    bool is_dt = false;
    bool is_blockquote = false;
    bool is_block = false;
    
    if (node->type == XML_ELEMENT_NODE) {
        const char* name = (const char*)node->name;
        
        // Headings
        if (name && strlen(name) == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            int h = name[1] - '0';
            if (!out.empty() && out.back() != '\n') out += '\n';
            for (int i = 0; i < h; i++) out += '#';
            out += ' ';
            is_block = true;
        }
        // Bold
        else if (name && (strcasecmp(name, "b") == 0 || strcasecmp(name, "strong") == 0)) {
            out += "**";
        }
        // Italic
        else if (name && (strcasecmp(name, "i") == 0 || strcasecmp(name, "em") == 0)) {
            out += "*";
        }
        // Code
        else if (name && (strcasecmp(name, "code") == 0 || strcasecmp(name, "tt") == 0)) {
            out += "`";
        }
        // Link: add opening bracket
        else if (name && strcasecmp(name, "a") == 0) {
            out += "[";
        }
        // List item
        else if (name && strcasecmp(name, "li") == 0) {
            is_li = true;
            if (!out.empty() && out.back() != '\n') out += '\n';
            out += "- ";
            is_block = true;
        }
        // Definition term
        else if (name && strcasecmp(name, "dt") == 0) {
            is_dt = true;
            if (!out.empty() && out.back() != '\n') out += '\n';
            out += "- ";
            is_block = true;
        }
        // Pre (code block)
        else if (name && strcasecmp(name, "pre") == 0) {
            is_pre = true;
            if (!out.empty() && out.back() != '\n') out += '\n';
            out += "```\n";
            is_block = true;
        }
        // Blockquote
        else if (name && strcasecmp(name, "blockquote") == 0) {
            is_blockquote = true;
            if (!out.empty() && out.back() != '\n') out += '\n';
            out += "> ";
            is_block = true;
        }
        // Horizontal rule
        else if (name && strcasecmp(name, "hr") == 0) {
            out += "\n---\n";
            return; // hr has no children
        }
        // Break
        else if (name && (strcasecmp(name, "br") == 0)) {
            out += "\n";
            return; // br has no children
        }
        // Block-level elements that need newline
        else if (name && (strcasecmp(name, "p") == 0 || strcasecmp(name, "div") == 0 ||
                         strcasecmp(name, "ul") == 0 || strcasecmp(name, "ol") == 0 ||
                         strcasecmp(name, "table") == 0 || strcasecmp(name, "tr") == 0)) {
            is_block = true;
        }
    }
    
    // === 处理文本节点 ===
    if (node->type == XML_TEXT_NODE && node->content) {
        const char* text = (const char*)node->content;
        // Skip pure whitespace
        bool has_content = false;
        for (const char* p = text; *p; p++) {
            if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                has_content = true;
                break;
            }
        }
        if (has_content) {
            // HTML entity decoding for text content
            std::string cleaned;
            for (const char* p = text; *p; p++) {
                if (*p == '&') {
                    if (strncmp(p, "&amp;", 5) == 0) { cleaned += '&'; p += 4; continue; }
                    if (strncmp(p, "&lt;", 4) == 0) { cleaned += '<'; p += 3; continue; }
                    if (strncmp(p, "&gt;", 4) == 0) { cleaned += '>'; p += 3; continue; }
                    if (strncmp(p, "&quot;", 6) == 0) { cleaned += '"'; p += 5; continue; }
                    if (strncmp(p, "&nbsp;", 6) == 0) { cleaned += ' '; p += 5; continue; }
                    if (strncmp(p, "&apos;", 6) == 0) { cleaned += '\''; p += 5; continue; }
                }
                cleaned += *p;
            }
            out += cleaned;
        }
    }
    
    // === 递归处理子节点 ===
    for (xmlNodePtr child = node->children; child; child = child->next) {
        extract_text_from_html(child, out);
    }
    
    // === 后置处理：添加闭合 Markdown 标记 ===
    if (node->type == XML_ELEMENT_NODE) {
        const char* name = (const char*)node->name;
        
        if (name && (strcasecmp(name, "b") == 0 || strcasecmp(name, "strong") == 0)) {
            out += "**";
        }
        else if (name && (strcasecmp(name, "i") == 0 || strcasecmp(name, "em") == 0)) {
            out += "*";
        }
        else if (name && (strcasecmp(name, "code") == 0 || strcasecmp(name, "tt") == 0)) {
            out += "`";
        }
        else if (name && strcasecmp(name, "a") == 0) {
            out += "]";
            // Extract href from attribute
            xmlChar* href = xmlGetProp(node, BAD_CAST "href");
            if (href) {
                out += "(";
                out += (const char*)href;
                out += ")";
                xmlFree(href);
            }
        }
        else if (name && strcasecmp(name, "pre") == 0) {
            out += "\n```\n";
        }
        else if (name && strcasecmp(name, "blockquote") == 0) {
            if (!out.empty() && out.back() != '\n') out += '\n';
        }
        else if (name && strcasecmp(name, "li") == 0) {
            if (!out.empty() && out.back() != '\n') out += '\n';
        }
        else if (name && strcasecmp(name, "img") == 0) {
            xmlChar* alt = xmlGetProp(node, BAD_CAST "alt");
            xmlChar* src = xmlGetProp(node, BAD_CAST "src");
            out += "![";
            out += alt ? (const char*)alt : "image";
            out += "](";
            out += src ? (const char*)src : "";
            out += ")";
            if (alt) xmlFree(alt);
            if (src) xmlFree(src);
        }
        
        // Block elements: ensure trailing newline
        if (is_block && !out.empty() && out.back() != '\n') {
            out += '\n';
        }
        // Heading: add extra newline after
        if (name && strlen(name) == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            out += '\n';
        }
    }
}


// 解析 META-INF/container.xml 获取 OPF 路径
static std::string get_opf_path(zip_t* za) {
    size_t len;
    char* data = zip_read_file(za, "META-INF/container.xml", &len);
    if (!data) return "";
    
    xmlDocPtr doc = xmlReadMemory(data, (int)len, nullptr, nullptr, 0);
    free(data);
    if (!doc) return "";
    
    std::string opf_path;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) {
        for (xmlNodePtr n1 = root->children; n1 && opf_path.empty(); n1 = n1->next) {
            if (n1->type == XML_ELEMENT_NODE && strcasecmp((const char*)n1->name, "rootfiles") == 0) {
                for (xmlNodePtr n2 = n1->children; n2 && opf_path.empty(); n2 = n2->next) {
                    if (n2->type == XML_ELEMENT_NODE && strcasecmp((const char*)n2->name, "rootfile") == 0) {
                        xmlChar* full_path = xmlGetProp(n2, BAD_CAST "full-path");
                        if (full_path) {
                            opf_path = (const char*)full_path;
                            xmlFree(full_path);
                        }
                    }
                }
            }
        }
    }
    
    xmlFreeDoc(doc);
    return opf_path;
}

// 计算相对于 OPF 目录的路径
static std::string resolve_path(const std::string& opf_path, const std::string& href) {
    size_t last_slash = opf_path.find_last_of('/');
    if (last_slash == std::string::npos) return href;
    std::string base = opf_path.substr(0, last_slash + 1);
    return base + href;
}

// =============================================================================
// API 实现
// =============================================================================

extern "C" {

/**
 * 打开 EPUB 文件
 */
API void* epub_open(const char* path) {
    int err;
    zip_t* za = zip_open(path, 0, &err);
    if (!za) {
        fprintf(stderr, "[EPUB] 无法打开文件: %s (err=%d)\n", path, err);
        return nullptr;
    }
    
    EpubHandle* handle = new EpubHandle();
    handle->za = za;
    handle->text_cache = nullptr;
    handle->text_cache_len = 0;
    handle->chapters = nullptr;
    handle->chapter_count = 0;
    handle->title[0] = '\0';
    handle->author[0] = '\0';
    
    return handle;
}

/**
 * 提取完整文本
 */
API int epub_extract_text(void* handle, char** out_text, size_t* out_len) {
    EpubHandle* h = (EpubHandle*)handle;
    if (!h || !h->za) return -1;
    
    // 如果已缓存，直接返回
    if (h->text_cache) {
        *out_text = h->text_cache;
        *out_len = h->text_cache_len;
        return 0;
    }
    
    // 1. 找到 OPF 文件
    std::string opf_path = get_opf_path(h->za);
    if (opf_path.empty()) {
        fprintf(stderr, "[EPUB] 无法找到 OPF 文件\n");
        return -1;
    }
    
    // 2. 读取并解析 OPF
    size_t opf_len;
    char* opf_data = zip_read_file(h->za, opf_path.c_str(), &opf_len);
    if (!opf_data) return -1;
    
    xmlDocPtr opf_doc = xmlReadMemory(opf_data, (int)opf_len, nullptr, nullptr, 0);
    free(opf_data);
    if (!opf_doc) return -1;
    
    xmlNodePtr opf_root = xmlDocGetRootElement(opf_doc);
    if (!opf_root) {
        xmlFreeDoc(opf_doc);
        return -1;
    }
    
    // 收集 manifest 和 spine
    std::map<std::string, std::string> manifest;  // id -> href
    std::vector<std::string> spine_ids;            // idref 顺序
    std::vector<EpubChapter> temp_chapters;
    
    for (xmlNodePtr child = opf_root->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        const char* name = (const char*)child->name;
        
        // 解析 metadata
        if (strcasecmp(name, "metadata") == 0) {
            for (xmlNodePtr m = child->children; m; m = m->next) {
                if (m->type != XML_ELEMENT_NODE) continue;
                const char* mname = (const char*)m->name;
                if (strcasecmp(mname, "title") == 0 || strcasecmp(mname, "dc:title") == 0) {
                    std::string t = xml_node_text(m);
                    if (!t.empty() && h->title[0] == '\0') {
                        strncpy(h->title, t.c_str(), sizeof(h->title) - 1);
                    }
                } else if (strcasecmp(mname, "creator") == 0 || strcasecmp(mname, "dc:creator") == 0) {
                    std::string a = xml_node_text(m);
                    if (!a.empty() && h->author[0] == '\0') {
                        strncpy(h->author, a.c_str(), sizeof(h->author) - 1);
                    }
                }
            }
        }
        // 解析 manifest
        else if (strcasecmp(name, "manifest") == 0) {
            for (xmlNodePtr item = child->children; item; item = item->next) {
                if (item->type != XML_ELEMENT_NODE) continue;
                if (strcasecmp((const char*)item->name, "item") != 0) continue;
                
                xmlChar* id = xmlGetProp(item, BAD_CAST "id");
                xmlChar* href = xmlGetProp(item, BAD_CAST "href");
                if (id && href) {
                    manifest[(const char*)id] = (const char*)href;
                }
                if (id) xmlFree(id);
                if (href) xmlFree(href);
            }
        }
        // 解析 spine
        else if (strcasecmp(name, "spine") == 0) {
            for (xmlNodePtr itemref = child->children; itemref; itemref = itemref->next) {
                if (itemref->type != XML_ELEMENT_NODE) continue;
                if (strcasecmp((const char*)itemref->name, "itemref") != 0) continue;
                
                xmlChar* idref = xmlGetProp(itemref, BAD_CAST "idref");
                if (idref) {
                    spine_ids.push_back((const char*)idref);
                    xmlFree(idref);
                }
            }
        }
    }
    
    // 3. 按 spine 顺序读取各章节
    std::string full_text;
    size_t current_offset = 0;
    
    for (const auto& idref : spine_ids) {
        auto it = manifest.find(idref);
        if (it == manifest.end()) continue;
        
        std::string file_path = resolve_path(opf_path, it->second);
        
        size_t html_len;
        char* html_data = zip_read_file(h->za, file_path.c_str(), &html_len);
        if (!html_data) continue;
        
        // 使用 HTML 解析器（更宽容）
        htmlDocPtr html_doc = htmlReadMemory(html_data, (int)html_len, nullptr, "UTF-8",
                                              HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
        free(html_data);
        if (!html_doc) continue;
        
        xmlNodePtr html_root = xmlDocGetRootElement(html_doc);
        if (html_root) {
            std::string chapter_text;
            extract_text_from_html(html_root, chapter_text);
            
            // 记录章节信息（如果有标题）
            // 尝试从 HTML <title> 或 <h1> 获取章节标题
            std::string ch_title;
            xmlXPathContextPtr ctx = xmlXPathNewContext(html_doc);
            if (ctx) {
                xmlXPathObjectPtr title_obj = xmlXPathEvalExpression(BAD_CAST "//title", ctx);
                if (title_obj && title_obj->nodesetval && title_obj->nodesetval->nodeNr > 0) {
                    ch_title = xml_node_text(title_obj->nodesetval->nodeTab[0]);
                }
                if (ch_title.empty()) {
                    xmlXPathFreeObject(title_obj);
                    title_obj = xmlXPathEvalExpression(BAD_CAST "//h1", ctx);
                    if (title_obj && title_obj->nodesetval && title_obj->nodesetval->nodeNr > 0) {
                        ch_title = xml_node_text(title_obj->nodesetval->nodeTab[0]);
                    }
                }
                if (title_obj) xmlXPathFreeObject(title_obj);
                xmlXPathFreeContext(ctx);
            }
            
            if (!ch_title.empty()) {
                EpubChapter ch;
                ch.title = strdup(ch_title.c_str());
                ch.level = 1;
                ch.offset = current_offset;
                temp_chapters.push_back(ch);
            }
            
            if (!chapter_text.empty()) {
                full_text += chapter_text;
                full_text += "\n\n";
                current_offset = full_text.length();
            }
        }
        
        xmlFreeDoc(html_doc);
    }
    
    xmlFreeDoc(opf_doc);
    
    // 4. 保存结果
    h->text_cache_len = full_text.length();
    h->text_cache = (char*)malloc(h->text_cache_len + 1);
    if (h->text_cache) {
        memcpy(h->text_cache, full_text.c_str(), h->text_cache_len);
        h->text_cache[h->text_cache_len] = '\0';
    }
    
    // 保存章节
    h->chapter_count = (int)temp_chapters.size();
    if (h->chapter_count > 0) {
        h->chapters = (EpubChapter*)malloc(sizeof(EpubChapter) * h->chapter_count);
        for (int i = 0; i < h->chapter_count; i++) {
            h->chapters[i] = temp_chapters[i];
        }
    }
    
    *out_text = h->text_cache;
    *out_len = h->text_cache_len;
    return 0;
}

/**
 * 获取元数据
 */
API int epub_get_metadata(void* handle, char* title, size_t title_len,
                          char* author, size_t author_len) {
    EpubHandle* h = (EpubHandle*)handle;
    if (!h) return -1;
    
    // 确保已解析（触发文本提取以解析 OPF）
    if (!h->text_cache) {
        char* tmp_text;
        size_t tmp_len;
        epub_extract_text(handle, &tmp_text, &tmp_len);
    }
    
    if (title && title_len > 0) {
        strncpy(title, h->title, title_len - 1);
        title[title_len - 1] = '\0';
    }
    if (author && author_len > 0) {
        strncpy(author, h->author, author_len - 1);
        author[author_len - 1] = '\0';
    }
    
    return 0;
}

/**
 * 获取章节列表
 */
API int epub_get_chapters(void* handle, EpubChapter** out_chapters, int* out_count) {
    EpubHandle* h = (EpubHandle*)handle;
    if (!h) return -1;
    
    // 确保已解析
    if (!h->text_cache) {
        char* tmp_text;
        size_t tmp_len;
        epub_extract_text(handle, &tmp_text, &tmp_len);
    }
    
    *out_chapters = h->chapters;
    *out_count = h->chapter_count;
    return 0;
}

/**
 * 释放章节数组（供调用方使用）
 */
API void epub_free_chapters(EpubChapter* chapters, int count) {
    // 实际上章节内存由 handle 管理，这里不需要做什么
    // 但为了 API 一致性保留
    (void)chapters;
    (void)count;
}

/**
 * 关闭 EPUB 文件并释放资源
 */
API void epub_close(void* handle) {
    EpubHandle* h = (EpubHandle*)handle;
    if (!h) return;
    
    if (h->text_cache) {
        free(h->text_cache);
    }
    
    if (h->chapters) {
        for (int i = 0; i < h->chapter_count; i++) {
            if (h->chapters[i].title) free(h->chapters[i].title);
        }
        free(h->chapters);
    }
    
    if (h->za) {
        zip_close(h->za);
    }
    
    delete h;
}

} // extern "C"
