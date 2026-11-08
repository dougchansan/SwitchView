// Tiny blocking HTTP/1.0 client for the LAN gadgets SwitchView drives (the
// ESP32 Pro Controller board, its Flipper bridge, the fingerbot server).
// Plain http:// only; every call has a hard timeout. Thread-safe.
#ifndef SV_HTTP_H
#define SV_HTTP_H

#include <stddef.h>

// Returns the HTTP status code, or -1 on connect/IO failure / timeout.
// The response body (NUL-terminated, truncated to resp_len-1) goes to resp
// when resp is non-NULL. body may be NULL for a GET.
int http_request(const char *url, const char *method, const char *body, size_t body_len,
                 char *resp, size_t resp_len, int timeout_ms);

static inline int http_get(const char *url, char *resp, size_t resp_len, int timeout_ms)
{
  return http_request(url, "GET", NULL, 0, resp, resp_len, timeout_ms);
}

// Percent-encode src into dst (RFC 3986 unreserved kept as-is).
void http_urlencode(const char *src, char *dst, size_t dst_len);

#endif
