#ifndef HTTP_ASYNC_H
#define HTTP_ASYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque async HTTP request handle. */
typedef struct http_async_req http_async_req_t;

/* Start an asynchronous HTTP request.
 * url:     full URL
 * method:  "GET", "POST", etc.
 * headers: newline-separated headers, e.g. "Content-Type: json\r\nAuthorization: Bearer x"
 * body:    request body (may be NULL)
 * Returns a request handle or NULL on error. The handle must be freed with http_async_free().
 */
http_async_req_t* http_async_request(const char* url,
                                     const char* method,
                                     const char* headers,
                                     const char* body);

/* Poll the request state.
 * Returns: 0 = in progress, 1 = completed, -1 = failed
 */
int http_async_poll(http_async_req_t* req);

/* Get the response body after completion.
 * Returns NULL if not complete. The string is owned by the request handle.
 */
const char* http_async_response(http_async_req_t* req);

/* Free the request and all associated resources. */
void http_async_free(http_async_req_t* req);

#ifdef __cplusplus
}
#endif

#endif
