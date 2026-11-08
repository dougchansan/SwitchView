#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// http://host[:port][/path] -> parts. Returns 0 on success.
static int parse_url(const char *url, char *host, size_t hl, char *port, size_t pl,
                     char *path, size_t ptl)
{
  if (strncmp(url, "http://", 7)) return -1;
  const char *h = url + 7;
  const char *slash = strchr(h, '/');
  const char *end = slash ? slash : h + strlen(h);
  const char *colon = memchr(h, ':', (size_t)(end - h));
  size_t n = (size_t)((colon ? colon : end) - h);
  if (!n || n >= hl) return -1;
  memcpy(host, h, n);
  host[n] = 0;
  if (colon) {
    size_t m = (size_t)(end - colon - 1);
    if (!m || m >= pl) return -1;
    memcpy(port, colon + 1, m);
    port[m] = 0;
  } else {
    snprintf(port, pl, "80");
  }
  snprintf(path, ptl, "%s", slash ? slash : "/");
  return 0;
}

static int wait_fd(int fd, short ev, long deadline)
{
  for (;;) {
    long left = deadline - now_ms();
    if (left <= 0) return 0;
    struct pollfd p = {.fd = fd, .events = ev};
    int r = poll(&p, 1, (int)left);
    if (r > 0) return 1;
    if (r < 0 && errno != EINTR) return -1;
  }
}

static int connect_to(const char *host, const char *port, long deadline)
{
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return -1; }
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  int r = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
  if (r < 0) {
    if (wait_fd(fd, POLLOUT, deadline) <= 0) { close(fd); return -1; }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { close(fd); return -1; }
  }
  return fd;
}

static int send_all(int fd, const char *buf, size_t len, long deadline)
{
  while (len) {
    ssize_t w = send(fd, buf, len, 0);
    if (w > 0) { buf += w; len -= (size_t)w; continue; }
    if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
      if (wait_fd(fd, POLLOUT, deadline) <= 0) return -1;
      continue;
    }
    return -1;
  }
  return 0;
}

int http_request(const char *url, const char *method, const char *body, size_t body_len,
                 char *resp, size_t resp_len, int timeout_ms)
{
  char host[256], port[16], path[2048];
  if (resp && resp_len) resp[0] = 0;
  if (parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path)) return -1;
  long deadline = now_ms() + timeout_ms;
  int fd = connect_to(host, port, deadline);
  if (fd < 0) return -1;

  char head[2600];
  int hn = snprintf(head, sizeof head,
                    "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: switchview\r\n"
                    "Connection: close\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n",
                    method, path, host, body ? body_len : 0);
  if (hn <= 0 || hn >= (int)sizeof head || send_all(fd, head, (size_t)hn, deadline) ||
      (body && body_len && send_all(fd, body, body_len, deadline))) {
    close(fd);
    return -1;
  }

  // Read everything (servers close after the reply), keep what fits.
  size_t cap = 65536, n = 0;
  char *buf = malloc(cap + 1);
  for (;;) {
    int w = wait_fd(fd, POLLIN, deadline);
    if (w <= 0) { if (!n) { free(buf); close(fd); return -1; } break; }
    ssize_t r = recv(fd, buf + n, cap - n, 0);
    if (r > 0) { n += (size_t)r; if (n == cap) break; continue; }
    if (r == 0) break;
    if (errno == EAGAIN || errno == EINTR) continue;
    break;
  }
  close(fd);
  buf[n] = 0;

  int status = -1;
  if (sscanf(buf, "HTTP/%*s %d", &status) != 1) status = -1;
  if (resp && resp_len) {
    char *b = strstr(buf, "\r\n\r\n");
    b = b ? b + 4 : buf + n;
    snprintf(resp, resp_len, "%s", b);
  }
  free(buf);
  return status;
}

void http_urlencode(const char *src, char *dst, size_t dst_len)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const unsigned char *s = (const unsigned char *)src; *s && n + 4 < dst_len; s++) {
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
        *s == '-' || *s == '_' || *s == '.' || *s == '~') {
      dst[n++] = (char)*s;
    } else {
      dst[n++] = '%';
      dst[n++] = hex[*s >> 4];
      dst[n++] = hex[*s & 15];
    }
  }
  if (dst_len) dst[n] = 0;
}
