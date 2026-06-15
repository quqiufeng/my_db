#define _GNU_SOURCE
#include "http_async.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct http_async_req {
    char* url;
    char* method;
    char* headers;
    char* body;

    char* response;
    size_t response_len;

    int state;      /* 0 = running, 1 = done, -1 = error */
    pthread_t thread;
    pthread_mutex_t mutex;
};

struct membuf {
    char* data;
    size_t size;
    size_t cap;
};

static size_t write_cb(const void* ptr, size_t size, size_t nmemb, void* userdata) {
    struct membuf* buf = userdata;
    size_t add = size * nmemb;
    if (buf->size + add + 1 > buf->cap) {
        buf->cap = buf->cap ? buf->cap * 2 : 4096;
        while (buf->size + add + 1 > buf->cap) buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
    }
    if (buf->data) {
        memcpy(buf->data + buf->size, ptr, add);
        buf->size += add;
        buf->data[buf->size] = '\0';
    }
    return add;
}

static void* http_thread(void* arg) {
    http_async_req_t* req = arg;

    CURL* curl = curl_easy_init();
    if (!curl) {
        pthread_mutex_lock(&req->mutex);
        req->state = -1;
        pthread_mutex_unlock(&req->mutex);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);

    struct membuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    struct curl_slist* hdrs = NULL;
    if (req->headers && *req->headers) {
        char* h = strdup(req->headers);
        char* p = h;
        while (p && *p) {
            char* end = strstr(p, "\r\n");
            if (!end) end = strchr(p, '\n');
            if (end) {
                char save = *end;
                *end = '\0';
                if (*p) hdrs = curl_slist_append(hdrs, p);
                *end = save;
                p = end + ((save == '\r' && *(end + 1) == '\n') ? 2 : 1);
            } else {
                if (*p) hdrs = curl_slist_append(hdrs, p);
                break;
            }
        }
        free(h);
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (req->body && *req->body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    }

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    pthread_mutex_lock(&req->mutex);
    if (res == CURLE_OK) {
        req->response = resp.data;
        req->response_len = resp.size;
        req->state = 1;
    } else {
        free(resp.data);
        req->state = -1;
    }
    pthread_mutex_unlock(&req->mutex);

    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);
    return NULL;
}

http_async_req_t* http_async_request(const char* url,
                                     const char* method,
                                     const char* headers,
                                     const char* body) {
    if (!url || !method) return NULL;

    http_async_req_t* req = calloc(1, sizeof(*req));
    if (!req) return NULL;

    pthread_mutex_init(&req->mutex, NULL);

    req->url = strdup(url);
    req->method = strdup(method);
    if (headers) req->headers = strdup(headers);
    if (body) req->body = strdup(body);

    if (pthread_create(&req->thread, NULL, http_thread, req) != 0) {
        http_async_free(req);
        return NULL;
    }

    return req;
}

int http_async_poll(http_async_req_t* req) {
    if (!req) return -1;
    pthread_mutex_lock(&req->mutex);
    int s = req->state;
    pthread_mutex_unlock(&req->mutex);
    return s;
}

const char* http_async_response(http_async_req_t* req) {
    if (!req) return NULL;
    pthread_mutex_lock(&req->mutex);
    const char* r = (req->state == 1) ? req->response : NULL;
    pthread_mutex_unlock(&req->mutex);
    return r;
}

void http_async_free(http_async_req_t* req) {
    if (!req) return;
    pthread_mutex_lock(&req->mutex);
    req->state = 0;
    pthread_mutex_unlock(&req->mutex);
    pthread_join(req->thread, NULL);
    free(req->response);
    free(req->url);
    free(req->method);
    free(req->headers);
    free(req->body);
    pthread_mutex_destroy(&req->mutex);
    free(req);
}
