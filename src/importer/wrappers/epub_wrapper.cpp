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
#include <functional>

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

// trim 首尾空白
static std::string trim_str(const std::string& s) {
    size_t b = s.find_first_not_of(" \n\r\t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \n\r\t");
    return s.substr(b, e - b + 1);
}

// 去掉 href 中的 #anchor
static std::string strip_anchor(const std::string& href) {
    size_t h = href.find('#');
    return h == std::string::npos ? href : href.substr(0, h);
}

// 从 toc.ncx（EPUB2 权威目录）提取 文件路径 -> 章节标题 映射
// <title>/<h1> 经常为空或只有泛型模板文本，NCX 的 navLabel 才是真实章节名
static std::map<std::string, std::string> parse_ncx_titles(zip_t* za, const std::string& ncx_path) {
    std::map<std::string, std::string> titles;
    size_t len;
    char* data = zip_read_file(za, ncx_path.c_str(), &len);
    if (!data) return titles;
    
    xmlDocPtr doc = xmlReadMemory(data, (int)len, nullptr, nullptr, 0);
    free(data);
    if (!doc) return titles;
    
    std::string base = ncx_path.substr(0, ncx_path.find_last_of('/') + 1);
    xmlNodePtr root = xmlDocGetRootElement(doc);
    // 遍历所有 navPoint：navLabel>text 是标题，content@src 是目标文件
    std::function<void(xmlNodePtr)> walk = [&](xmlNodePtr node) {
        for (; node; node = node->next) {
            if (node->type == XML_ELEMENT_NODE &&
                strcasecmp((const char*)node->name, "navPoint") == 0) {
                std::string label, src;
                for (xmlNodePtr c = node->children; c; c = c->next) {
                    if (c->type != XML_ELEMENT_NODE) continue;
                    if (strcasecmp((const char*)c->name, "navLabel") == 0) {
                        for (xmlNodePtr t = c->children; t; t = t->next) {
                            if (t->type == XML_ELEMENT_NODE &&
                                strcasecmp((const char*)t->name, "text") == 0) {
                                xmlChar* content = xmlNodeGetContent(t);
                                if (content) { label = (const char*)content; xmlFree(content); }
                            }
                        }
                    } else if (strcasecmp((const char*)c->name, "content") == 0) {
                        xmlChar* s = xmlGetProp(c, BAD_CAST "src");
                        if (s) { src = (const char*)s; xmlFree(s); }
                    }
                }
                label = trim_str(label);
                if (!label.empty() && !src.empty()) {
                    std::string path = base + strip_anchor(src);
                    // 同一文件的第一个 navPoint 是章节标题，其余是小节
                    if (titles.find(path) == titles.end()) {
                        titles[path] = label;
                    }
                }
            }
            if (node->children) walk(node->children);
        }
    };
    if (root) walk(root->children);
    xmlFreeDoc(doc);
    return titles;
}

// 从 nav.xhtml（EPUB3 目录）提取 文件路径 -> 章节标题 映射
static std::map<std::string, std::string> parse_nav_titles(zip_t* za, const std::string& nav_path) {
    std::map<std::string, std::string> titles;
    size_t len;
    char* data = zip_read_file(za, nav_path.c_str(), &len);
    if (!data) return titles;
    
    htmlDocPtr doc = htmlReadMemory(data, (int)len, nullptr, "UTF-8",
                                    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    free(data);
    if (!doc) return titles;
    
    std::string base = nav_path.substr(0, nav_path.find_last_of('/') + 1);
    xmlNodePtr root = xmlDocGetRootElement(doc);
    std::function<void(xmlNodePtr)> walk = [&](xmlNodePtr node) {
        for (; node; node = node->next) {
            if (node->type == XML_ELEMENT_NODE &&
                strcasecmp((const char*)node->name, "a") == 0) {
                xmlChar* href = xmlGetProp(node, BAD_CAST "href");
                xmlChar* content = xmlNodeGetContent(node);
                if (href && content) {
                    std::string label = trim_str((const char*)content);
                    std::string path = base + strip_anchor((const char*)href);
                    if (!label.empty() && titles.find(path) == titles.end()) {
                        titles[path] = label;
                    }
                }
                if (href) xmlFree(href);
                if (content) xmlFree(content);
            }
            if (node->children) walk(node->children);
        }
    };
    if (root) walk(root->children);
    xmlFreeDoc(doc);
    return titles;
}


// =============================================================================
// 共享解析助手（epub_extract_text 与 epub_get_book 共用）
// =============================================================================

struct OpfInfo {
    std::map<std::string, std::string> manifest;  // id -> href
    std::vector<std::string> spine_ids;           // idref 顺序
};

// 解析 OPF：metadata（书名/作者）+ manifest + spine
static bool parse_opf(EpubHandle* h, const std::string& opf_path, OpfInfo& info) {
    size_t opf_len;
    char* opf_data = zip_read_file(h->za, opf_path.c_str(), &opf_len);
    if (!opf_data) return false;
    
    xmlDocPtr opf_doc = xmlReadMemory(opf_data, (int)opf_len, nullptr, nullptr, 0);
    free(opf_data);
    if (!opf_doc) return false;
    
    xmlNodePtr opf_root = xmlDocGetRootElement(opf_doc);
    if (!opf_root) { xmlFreeDoc(opf_doc); return false; }
    
    for (xmlNodePtr child = opf_root->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        const char* name = (const char*)child->name;
        
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
        else if (strcasecmp(name, "manifest") == 0) {
            for (xmlNodePtr item = child->children; item; item = item->next) {
                if (item->type != XML_ELEMENT_NODE) continue;
                if (strcasecmp((const char*)item->name, "item") != 0) continue;
                xmlChar* id = xmlGetProp(item, BAD_CAST "id");
                xmlChar* href = xmlGetProp(item, BAD_CAST "href");
                if (id && href) info.manifest[(const char*)id] = (const char*)href;
                if (id) xmlFree(id);
                if (href) xmlFree(href);
            }
        }
        else if (strcasecmp(name, "spine") == 0) {
            for (xmlNodePtr itemref = child->children; itemref; itemref = itemref->next) {
                if (itemref->type != XML_ELEMENT_NODE) continue;
                if (strcasecmp((const char*)itemref->name, "itemref") != 0) continue;
                xmlChar* idref = xmlGetProp(itemref, BAD_CAST "idref");
                if (idref) {
                    info.spine_ids.push_back((const char*)idref);
                    xmlFree(idref);
                }
            }
        }
    }
    xmlFreeDoc(opf_doc);
    return true;
}

// 从权威目录（NCX/nav）加载 文件路径 -> 章节标题 映射
static std::map<std::string, std::string> load_toc_titles(zip_t* za, const std::string& opf_path,
                                                          const std::map<std::string, std::string>& manifest) {
    std::string ncx_href, nav_href;
    for (const auto& kv : manifest) {
        const std::string& href = kv.second;
        if (href.size() > 4 && href.compare(href.size() - 4, 4, ".ncx") == 0) {
            ncx_href = href;
        }
        if (href.find("nav") != std::string::npos &&
            (href.find(".xhtml") != std::string::npos || href.find(".html") != std::string::npos)) {
            nav_href = href;
        }
    }
    std::map<std::string, std::string> toc;
    if (!ncx_href.empty()) {
        toc = parse_ncx_titles(za, resolve_path(opf_path, ncx_href));
    }
    if (toc.empty() && !nav_href.empty()) {
        toc = parse_nav_titles(za, resolve_path(opf_path, nav_href));
    }
    return toc;
}

// 解析单个 spine XHTML 的章节标题：权威目录优先，<h1>/<title> 兜底
static std::string resolve_chapter_title(htmlDocPtr html_doc,
                                         const std::map<std::string, std::string>& toc_titles,
                                         const std::string& file_path) {
    std::string ch_title;
    auto toc_it = toc_titles.find(file_path);
    if (toc_it != toc_titles.end()) {
        ch_title = toc_it->second;
    }
    if (ch_title.empty()) {
        xmlXPathContextPtr ctx = xmlXPathNewContext(html_doc);
        if (ctx) {
            xmlXPathObjectPtr title_obj = xmlXPathEvalExpression(BAD_CAST "//h1", ctx);
            if (title_obj && title_obj->nodesetval && title_obj->nodesetval->nodeNr > 0) {
                xmlChar* content = xmlNodeGetContent(title_obj->nodesetval->nodeTab[0]);
                if (content) { ch_title = (const char*)content; xmlFree(content); }
            }
            if (trim_str(ch_title).empty()) {
                if (title_obj) xmlXPathFreeObject(title_obj);
                title_obj = xmlXPathEvalExpression(BAD_CAST "//title", ctx);
                if (title_obj && title_obj->nodesetval && title_obj->nodesetval->nodeNr > 0) {
                    xmlChar* content = xmlNodeGetContent(title_obj->nodesetval->nodeTab[0]);
                    if (content) { ch_title = (const char*)content; xmlFree(content); }
                }
            }
            if (title_obj) xmlXPathFreeObject(title_obj);
            xmlXPathFreeContext(ctx);
        }
    }
    return trim_str(ch_title);
}


extern "C" {

/**
 * 打开 EPUB 文件
 */
API void* epub_open(const char* path) {    int err;
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
    
    // 2. 解析 OPF（metadata + manifest + spine）
    OpfInfo opf;
    if (!parse_opf(h, opf_path, opf)) return -1;
    
    std::vector<EpubChapter> temp_chapters;
    
    // 3. 按 spine 顺序读取各章节
    std::string full_text;
    size_t current_offset = 0;
    
    // 从权威目录提取章节标题映射（NCX 优先，EPUB3 nav 兜底）
    std::map<std::string, std::string> toc_titles = load_toc_titles(h->za, opf_path, opf.manifest);
    
    for (const auto& idref : opf.spine_ids) {
        auto it = opf.manifest.find(idref);
        if (it == opf.manifest.end()) continue;
        
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
            
            std::string ch_title = resolve_chapter_title(html_doc, toc_titles, file_path);
            
            if (!ch_title.empty()) {
                EpubChapter ch;
                ch.title = strdup(ch_title.c_str());
                ch.level = 1;
                ch.offset = current_offset;
                temp_chapters.push_back(ch);
            }
            
            // 如果 <title> 是纯数字（如 Kobo 格式），从 nav.xhtml 获取真实标题
            if (!ch_title.empty()) {
                bool is_numeric = true;
                for (char c : ch_title) {
                    if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && (c < '0' || c > '9')) {
                        is_numeric = false; break;
                    }
                }
                if (is_numeric && temp_chapters.size() > 0) {
                    // 标记最后一个章节的标题为待替换
                    temp_chapters.back().title[0] = '\0';
                }
            }
            
            if (!chapter_text.empty()) {
                full_text += chapter_text;
                full_text += "\n\n";
                current_offset = full_text.length();
            }
        }
        
        xmlFreeDoc(html_doc);
    }
    
    // 3.5 从 XHTML 内容中提取真实章节标题
    // 对于标题是纯数字的章节，从章节文本中提取 "Chapter X:" 开头的行作为标题
    {
        size_t text_pos = 0;
        for (size_t ci = 0; ci < temp_chapters.size(); ci++) {
            auto& ch = temp_chapters[ci];
            if (ch.title[0] == '\0') {
                // 从 full_text 的 text_pos 位置找 "Chapter " 开头的行
                std::string search = "Chapter ";
                size_t pos = full_text.find(search, text_pos);
                if (pos != std::string::npos && pos < text_pos + 2000) {
                    size_t end = full_text.find('\n', pos);
                    if (end == std::string::npos) end = pos + 100;
                    std::string title = full_text.substr(pos, end - pos);
                    // Trim
                    size_t s = title.find_first_not_of(" \n\r\t");
                    size_t e = title.find_last_not_of(" \n\r\t");
                    if (s != std::string::npos && e != std::string::npos) {
                        free(ch.title);
                        ch.title = strdup(title.substr(s, e - s + 1).c_str());
                    }
                }
            }
            // 更新到下一章节偏移
            if (ci + 1 < temp_chapters.size()) {
                text_pos = temp_chapters[ci + 1].offset;
            }
        }
    }
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

// =============================================================================
// 统一章节接口：直接输出 {title, level, text}，调用者无需关心 spine/偏移
// =============================================================================

struct EpubBookChapter {
    char* title;
    int level;
    char* text;
    size_t text_len;
};

/**
 * 获取整本书的章节列表（含正文）
 * 每个 spine XHTML 一章；无标题文件给 "untitled" 兜底名（不丢内容）
 * @return 0 成功；调用者用 epub_free_book 释放
 */
API int epub_get_book(void* handle, EpubBookChapter** out_chapters, int* out_count) {
    EpubHandle* h = (EpubHandle*)handle;
    if (!h || !h->za) return -1;
    
    std::string opf_path = get_opf_path(h->za);
    if (opf_path.empty()) return -1;
    
    OpfInfo opf;
    if (!parse_opf(h, opf_path, opf)) return -1;
    
    std::map<std::string, std::string> toc_titles = load_toc_titles(h->za, opf_path, opf.manifest);
    
    std::vector<EpubBookChapter> result;
    std::string full_text;
    int untitled_idx = 0;
    
    for (const auto& idref : opf.spine_ids) {
        auto it = opf.manifest.find(idref);
        if (it == opf.manifest.end()) continue;
        
        std::string file_path = resolve_path(opf_path, it->second);
        
        size_t html_len;
        char* html_data = zip_read_file(h->za, file_path.c_str(), &html_len);
        if (!html_data) continue;
        
        htmlDocPtr html_doc = htmlReadMemory(html_data, (int)html_len, nullptr, "UTF-8",
                                              HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
        free(html_data);
        if (!html_doc) continue;
        
        xmlNodePtr html_root = xmlDocGetRootElement(html_doc);
        if (html_root) {
            std::string chapter_text;
            extract_text_from_html(html_root, chapter_text);
            chapter_text = trim_str(chapter_text);
            if (!chapter_text.empty()) {
                std::string ch_title = resolve_chapter_title(html_doc, toc_titles, file_path);
                if (ch_title.empty()) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "untitled-%d", ++untitled_idx);
                    ch_title = buf;
                }
                EpubBookChapter ch;
                ch.title = strdup(ch_title.c_str());
                ch.level = 1;
                ch.text_len = chapter_text.length();
                ch.text = (char*)malloc(ch.text_len + 1);
                if (ch.text) memcpy(ch.text, chapter_text.c_str(), ch.text_len + 1);
                result.push_back(ch);
                full_text += chapter_text;
                full_text += "\n\n";
            }
        }
        xmlFreeDoc(html_doc);
    }
    
    // 极端情况：spine 全部为空，返回单个全文伪章节
    if (result.empty() && !full_text.empty()) {
        EpubBookChapter ch;
        ch.title = strdup("full");
        ch.level = 0;
        ch.text_len = full_text.length();
        ch.text = (char*)malloc(ch.text_len + 1);
        if (ch.text) memcpy(ch.text, full_text.c_str(), ch.text_len + 1);
        result.push_back(ch);
    }
    
    EpubBookChapter* arr = (EpubBookChapter*)malloc(sizeof(EpubBookChapter) * (result.size() ? result.size() : 1));
    for (size_t i = 0; i < result.size(); i++) arr[i] = result[i];
    *out_chapters = arr;
    *out_count = (int)result.size();
    return 0;
}

API void epub_free_book(EpubBookChapter* chapters, int count) {
    if (!chapters) return;
    for (int i = 0; i < count; i++) {
        free(chapters[i].title);
        free(chapters[i].text);
    }
    free(chapters);
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
