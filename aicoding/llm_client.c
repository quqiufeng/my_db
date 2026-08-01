#define _GNU_SOURCE
#include "llm_client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct llm_client {
    llm_config_t config;
    CURL*        curl;
};

struct membuf {
    char*  data;
    size_t size;
    size_t cap;
};

struct stream_ctx {
    llm_stream_cb_t cb;
    void*           userdata;
    char*           chunk;   /* buffered incomplete line */
    size_t          chunk_len;
};

static size_t write_cb(const void* ptr, size_t size, size_t nmemb, void* userdata) {
    struct membuf* buf = userdata;
    size_t add = size * nmemb;
    if (buf->size + add + 1 > buf->cap) {
        buf->cap = buf->cap ? buf->cap * 2 : 4096;
        while (buf->size + add + 1 > buf->cap) buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->size, ptr, add);
    buf->size += add;
    buf->data[buf->size] = '\0';
    return add;
}

static int append_chunk(struct stream_ctx* ctx, const char* data, size_t len) {
    char* p = realloc(ctx->chunk, ctx->chunk_len + len + 1);
    if (!p) return -1;
    ctx->chunk = p;
    memcpy(ctx->chunk + ctx->chunk_len, data, len);
    ctx->chunk_len += len;
    ctx->chunk[ctx->chunk_len] = '\0';
    return 0;
}

static int flush_line(struct stream_ctx* ctx, char* line, size_t len) {
    /* SSE lines starting with "data: " */
    if (len > 6 && memcmp(line, "data: ", 6) == 0) {
        const char* payload = line + 6;
        if (strcmp(payload, "[DONE]") == 0) return 0;
        if (getenv("OPENCODE_DEBUG"))
            fprintf(stderr, "[STREAM CHUNK] %.500s\n", payload);
        if (ctx->cb) {
            /* Deliver the raw chunk payload; the caller parses it. */
            int rc = ctx->cb(payload, ctx->userdata);
            if (rc != 0) return 1; /* abort */
        }
    }
    return 0;
}

static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    struct stream_ctx* ctx = userdata;
    size_t add = size * nmemb;
    append_chunk(ctx, ptr, add);

    /* Process complete lines */
    char* start = ctx->chunk;
    char* newline;
    while ((newline = memchr(start, '\n', ctx->chunk_len - (start - ctx->chunk))) != NULL) {
        *newline = '\0';
        if (flush_line(ctx, start, newline - start) != 0) return 0; /* abort */
        start = newline + 1;
    }

    /* Shift remaining */
    size_t remaining = ctx->chunk_len - (start - ctx->chunk);
    if (remaining > 0 && start != ctx->chunk) {
        memmove(ctx->chunk, start, remaining);
    }
    ctx->chunk_len = remaining;
    if (ctx->chunk) ctx->chunk[remaining] = '\0';
    return add;
}

static char* json_escape(const char* s) {
    /* Minimal JSON string escaping for request body assembly */
    size_t len = 0;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
                len += 2; break;
            default: len += 1;
        }
    }
    char* out = malloc(len + 1);
    if (!out) return NULL;
    char* d = out;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  *d++ = '\\'; *d++ = '"';  break;
            case '\\': *d++ = '\\'; *d++ = '\\'; break;
            case '\b': *d++ = '\\'; *d++ = 'b';  break;
            case '\f': *d++ = '\\'; *d++ = 'f';  break;
            case '\n': *d++ = '\\'; *d++ = 'n';  break;
            case '\r': *d++ = '\\'; *d++ = 'r';  break;
            case '\t': *d++ = '\\'; *d++ = 't';  break;
            default:   *d++ = *p; break;
        }
    }
    *d = '\0';
    return out;
}

static void write_escaped(struct membuf* buf, const char* s) {
    char* esc = json_escape(s);
    write_cb(esc, strlen(esc), 1, buf);
    free(esc);
}

static void write_s(struct membuf* buf, const char* s) {
    write_cb(s, strlen(s), 1, buf);
}

static char* build_request_json_openai_messages(const llm_client_t* c,
                                                const char* system_prompt,
                                                const char* messages_json,
                                                const char* tools_json,
                                                int stream) {
    struct membuf buf = {0};
    write_s(&buf, "{\"model\":\"");
    write_escaped(&buf, c->config.model);
    write_s(&buf, "\",\"messages\":[");
    write_s(&buf, "{\"role\":\"system\",\"content\":\"");
    write_escaped(&buf, system_prompt);
    write_s(&buf, "\"},");
    write_s(&buf, messages_json); /* already a JSON array */
    write_s(&buf, "]");

    if (tools_json) {
        write_s(&buf, ",\"tools\":");
        write_s(&buf, tools_json);
    }

    if (stream) {
        write_s(&buf, ",\"stream\":true");
    }

    write_s(&buf, ",\"temperature\":");
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%f", c->config.temperature);
    write_s(&buf, tmp);
    write_s(&buf, "}");
    return buf.data;
}

static char* build_request_json_openai(const llm_client_t* c,
                                       const char* system_prompt,
                                       const char* user_prompt,
                                       const char* tools_json,
                                       int stream) {
    struct membuf buf = {0};
    write_s(&buf, "{\"model\":\"");
    write_escaped(&buf, c->config.model);
    write_s(&buf, "\",\"messages\":[");
    write_s(&buf, "{\"role\":\"system\",\"content\":\"");
    write_escaped(&buf, system_prompt);
    write_s(&buf, "\"},{\"role\":\"user\",\"content\":\"");
    write_escaped(&buf, user_prompt);
    write_s(&buf, "\"}]");

    if (tools_json) {
        write_s(&buf, ",\"tools\":");
        write_s(&buf, tools_json);
    }

    if (stream) {
        write_s(&buf, ",\"stream\":true");
    }

    write_s(&buf, ",\"temperature\":");
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%f", c->config.temperature);
    write_s(&buf, tmp);
    write_s(&buf, "}");
    return buf.data;
}

static char* build_request_json_anthropic(const llm_client_t* c,
                                          const char* system_prompt,
                                          const char* user_prompt,
                                          const char* tools_json,
                                          int stream) {
    struct membuf buf = {0};
    write_s(&buf, "{\"model\":\"");
    write_escaped(&buf, c->config.model);
    write_s(&buf, "\",\"max_tokens\":");
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%d", c->config.max_tokens);
    write_s(&buf, tmp);
    write_s(&buf, ",\"system\":\"");
    write_escaped(&buf, system_prompt);
    write_s(&buf, "\",\"messages\":[{\"role\":\"user\",\"content\":\"");
    write_escaped(&buf, user_prompt);
    write_s(&buf, "\"}]");

    if (tools_json) {
        write_s(&buf, ",\"tools\":");
        write_s(&buf, tools_json);
    }

    if (stream) {
        write_s(&buf, ",\"stream\":true");
    }

    write_s(&buf, "}");
    return buf.data;
}

static char* build_request_json(const llm_client_t* c,
                                const char* system_prompt,
                                const char* user_prompt,
                                const char* tools_json,
                                int stream) {
    if (c->config.protocol == LLM_PROTOCOL_ANTHROPIC) {
        return build_request_json_anthropic(c, system_prompt, user_prompt, tools_json, stream);
    }
    return build_request_json_openai(c, system_prompt, user_prompt, tools_json, stream);
}

static CURL* setup_post_openai(llm_client_t* c, const char* url, struct curl_slist** headers) {
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;

    *headers = NULL;
    *headers = curl_slist_append(*headers, "Content-Type: application/json");
    char auth[1024];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->config.api_key);
    *headers = curl_slist_append(*headers, auth);

    if (c->config.user_agent && *c->config.user_agent) {
        char ua[1024];
        snprintf(ua, sizeof(ua), "User-Agent: %s", c->config.user_agent);
        *headers = curl_slist_append(*headers, ua);
    }
    if (c->config.extra_header && *c->config.extra_header) {
        char* copy = strdup(c->config.extra_header);
        if (copy) {
            char* save = NULL;
            for (char* line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                *headers = curl_slist_append(*headers, line);
            }
            free(copy);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    return curl;
}

static CURL* setup_post_anthropic(llm_client_t* c, const char* url, struct curl_slist** headers) {
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;

    *headers = NULL;
    *headers = curl_slist_append(*headers, "Content-Type: application/json");
    char key_hdr[1024];
    snprintf(key_hdr, sizeof(key_hdr), "x-api-key: %s", c->config.api_key);
    *headers = curl_slist_append(*headers, key_hdr);
    *headers = curl_slist_append(*headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    return curl;
}

static CURL* setup_post(llm_client_t* c, const char* url, struct curl_slist** headers) {
    if (c->config.protocol == LLM_PROTOCOL_ANTHROPIC) {
        return setup_post_anthropic(c, url, headers);
    }
    return setup_post_openai(c, url, headers);
}

/* Build an LLM config from the environment (LLM_PROTOCOL + provider vars).
 * Caller must free the returned strings with llm_config_free_fields. */
void llm_config_from_env(llm_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    const char* proto = getenv("LLM_PROTOCOL");
    cfg->protocol = (proto && strcasecmp(proto, "anthropic") == 0) ? LLM_PROTOCOL_ANTHROPIC : LLM_PROTOCOL_OPENAI;

    if (cfg->protocol == LLM_PROTOCOL_ANTHROPIC) {
        cfg->base_url = strdup(getenv("ANTHROPIC_BASE_URL") ? getenv("ANTHROPIC_BASE_URL") : "");
        cfg->api_key  = strdup(getenv("ANTHROPIC_API_KEY") ? getenv("ANTHROPIC_API_KEY") : "");
        cfg->model    = strdup(getenv("ANTHROPIC_MODEL") ? getenv("ANTHROPIC_MODEL") : "");
    } else {
        cfg->base_url = strdup(getenv("OPENAI_BASE_URL") ? getenv("OPENAI_BASE_URL") : "");
        cfg->api_key  = strdup(getenv("OPENAI_API_KEY") ? getenv("OPENAI_API_KEY") : "");
        cfg->model    = strdup(getenv("OPENAI_MODEL") ? getenv("OPENAI_MODEL") : "");

        /* DeepSeek uses OpenAI-compatible protocol but has its own env vars as aliases. */
        if (!cfg->base_url[0]) { free(cfg->base_url); cfg->base_url = strdup(getenv("DEEPSEEK_BASE_URL") ? getenv("DEEPSEEK_BASE_URL") : ""); }
        if (!cfg->api_key[0])  { free(cfg->api_key);  cfg->api_key  = strdup(getenv("DEEPSEEK_API_KEY") ? getenv("DEEPSEEK_API_KEY") : ""); }
        if (!cfg->model[0])    { free(cfg->model);    cfg->model    = strdup(getenv("DEEPSEEK_MODEL") ? getenv("DEEPSEEK_MODEL") : ""); }
    }
    cfg->user_agent = strdup(getenv("LLM_USER_AGENT") ? getenv("LLM_USER_AGENT") : "");
    cfg->extra_header = strdup(getenv("LLM_EXTRA_HEADER") ? getenv("LLM_EXTRA_HEADER") : "");
    const char* temp = getenv("LLM_TEMPERATURE");
    cfg->temperature = temp ? atof(temp) : 0.7;
    cfg->max_tokens = 4096;
}

void llm_config_free_fields(llm_config_t* cfg) {
    free(cfg->base_url);
    free(cfg->api_key);
    free(cfg->model);
    free(cfg->user_agent);
    free(cfg->extra_header);
}

llm_client_t* llm_client_create(const llm_config_t* config) {    if (!config || !config->base_url || !config->api_key || !config->model) return NULL;
    llm_client_t* c = calloc(1, sizeof(llm_client_t));
    if (!c) return NULL;

    c->config.protocol = config->protocol;
    c->config.base_url = strdup(config->base_url);
    c->config.api_key = strdup(config->api_key);
    c->config.model = strdup(config->model);
    c->config.user_agent = config->user_agent ? strdup(config->user_agent) : NULL;
    c->config.extra_header = config->extra_header ? strdup(config->extra_header) : NULL;
    c->config.temperature = config->temperature > 0 ? config->temperature : 0.7;
    c->config.max_tokens = config->max_tokens > 0 ? config->max_tokens : 4096;
    c->curl = NULL;
    return c;
}

int llm_client_protocol(const llm_client_t* c) {
    return c ? c->config.protocol : LLM_PROTOCOL_OPENAI;
}

const char* llm_client_model(const llm_client_t* c) {
    return c ? c->config.model : NULL;
}

void llm_client_free(llm_client_t* c) {
    if (!c) return;
    free(c->config.base_url);
    free(c->config.api_key);
    free(c->config.model);
    free(c->config.user_agent);
    free(c->config.extra_header);
    if (c->curl) curl_easy_cleanup(c->curl);
    free(c);
}

char* llm_complete(llm_client_t* c,
                   const char* system_prompt,
                   const char* user_prompt,
                   const char* tools_json) {
    if (!c || !system_prompt || !user_prompt) return NULL;

    char url[2048];
    if (c->config.protocol == LLM_PROTOCOL_ANTHROPIC) {
        snprintf(url, sizeof(url), "%s/v1/messages", c->config.base_url);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", c->config.base_url);
    }

    struct curl_slist* headers = NULL;
    CURL* curl = setup_post(c, url, &headers);
    if (!curl) return NULL;

    char* body = build_request_json(c, system_prompt, user_prompt, tools_json, 0);
    if (getenv("OPENCODE_DEBUG")) {
        fprintf(stderr, "[LLM REQ] %s\n", body);
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    struct membuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "llm_complete curl error: %s\n", curl_easy_strerror(res));
        free(body);
        free(resp.data);
        return NULL;
    }
    free(body);
    return resp.data;
}

char* llm_complete_messages(llm_client_t* c,
                            const char* system_prompt,
                            const char* messages_json,
                            const char* tools_json) {
    if (!c || !system_prompt || !messages_json) return NULL;

    char url[2048];
    if (c->config.protocol == LLM_PROTOCOL_ANTHROPIC) {
        snprintf(url, sizeof(url), "%s/v1/messages", c->config.base_url);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", c->config.base_url);
    }

    struct curl_slist* headers = NULL;
    CURL* curl = setup_post(c, url, &headers);
    if (!curl) return NULL;

    char* body = build_request_json_openai_messages(c, system_prompt, messages_json, tools_json, 0);
    if (getenv("OPENCODE_DEBUG")) {
        fprintf(stderr, "[LLM MESSAGES] %s\n", messages_json);
        fprintf(stderr, "[LLM MESSAGES FIRST 20] ");
        for (int i = 0; i < 20 && messages_json[i]; i++) fprintf(stderr, "%02x ", (unsigned char)messages_json[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[LLM REQ] %s\n", body);
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    struct membuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "llm_complete_messages curl error: %s\n", curl_easy_strerror(res));
        free(body);
        free(resp.data);
        return NULL;
    }
    free(body);
    return resp.data;
}

char* llm_complete_raw(llm_client_t* c, const char* request_body_json) {
    if (!c || !request_body_json) return NULL;

    char url[2048];
    if (c->config.protocol == LLM_PROTOCOL_ANTHROPIC) {
        snprintf(url, sizeof(url), "%s/v1/messages", c->config.base_url);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", c->config.base_url);
    }

    struct curl_slist* headers = NULL;
    CURL* curl = setup_post(c, url, &headers);
    if (!curl) return NULL;

    if (getenv("OPENCODE_DEBUG")) {
        fprintf(stderr, "[LLM RAW REQ] %s\n", request_body_json);
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body_json);

    struct membuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "llm_complete_raw curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return NULL;
    }
    if (getenv("OPENCODE_DEBUG")) {
        fprintf(stderr, "[LLM RAW RESP] %.4000s\n", resp.data ? resp.data : "(null)");
    }
    return resp.data;
}

int llm_complete_stream(llm_client_t* c,
                        const char* system_prompt,
                        const char* user_prompt,
                        const char* tools_json,
                        llm_stream_cb_t cb,
                        void* userdata) {
    if (!c || !system_prompt || !user_prompt) return -1;

    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", c->config.base_url);

    struct curl_slist* headers = NULL;
    CURL* curl = setup_post(c, url, &headers);
    if (!curl) return -1;

    char* body = build_request_json(c, system_prompt, user_prompt, tools_json, 1);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    struct stream_ctx ctx = {0};
    ctx.cb = cb;
    ctx.userdata = userdata;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(body);
    free(ctx.chunk);

    if (res != CURLE_OK) {
        fprintf(stderr, "llm_complete_stream curl error: %s\n", curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

int llm_complete_raw_stream(llm_client_t* c,
                            const char* request_body_json,
                            llm_stream_cb_t cb,
                            void* userdata) {
    if (!c || !request_body_json) return -1;

    char url[2048];
    if (c->config.protocol == LLM_PROTOCOL_ANTHROPIC) {
        snprintf(url, sizeof(url), "%s/v1/messages", c->config.base_url);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", c->config.base_url);
    }

    struct curl_slist* headers = NULL;
    CURL* curl = setup_post(c, url, &headers);
    if (!curl) return -1;

    /* Inject "stream":true before the final '}' of the request body. */
    size_t blen = strlen(request_body_json);
    char* body = malloc(blen + 32);
    if (!body) {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return -1;
    }
    if (blen > 0 && request_body_json[blen - 1] == '}') {
        memcpy(body, request_body_json, blen - 1);
        strcpy(body + blen - 1, ",\"stream\":true}");
    } else {
        strcpy(body, request_body_json);
    }

    if (getenv("OPENCODE_DEBUG")) {
        fprintf(stderr, "[LLM RAW STREAM REQ] %s\n", body);
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    struct stream_ctx ctx = {0};
    ctx.cb = cb;
    ctx.userdata = userdata;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(body);
    free(ctx.chunk);

    if (res != CURLE_OK) {
        fprintf(stderr, "llm_complete_raw_stream curl error: %s\n", curl_easy_strerror(res));
        return -1;
    }
    return 0;
}
