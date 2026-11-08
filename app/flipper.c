#include "flipper.h"
#include "switchview_proto.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define REMOTE "/ext/badusb/switchview.txt"
#define SCRIPT_MAX 60000

// Protobuf fields of PB_Main (flipperzero-protobuf flipper.proto).
enum {
  F_ID = 1, F_STATUS = 2, F_HAS_NEXT = 3,
  F_STORAGE_WRITE = 11, F_APP_START = 16, F_APP_LOCK_STATUS = 17, F_APP_LOCK_REPLY = 18,
  F_GUI_INPUT = 23,
};
enum { KEY_OK = 4, KEY_BACK = 5 };
enum { EV_PRESS = 0, EV_RELEASE = 1, EV_SHORT = 2 };

static int env_int(const char *name, int def, int lo, int hi)
{
  const char *v = getenv(name);
  int n = v && *v ? atoi(v) : def;
  return n < lo ? lo : n > hi ? hi : n;
}

static long mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---------------- protobuf ---------------- */

struct pb { uint8_t *p; size_t n, cap; };

static void pb_raw(struct pb *b, const void *d, size_t n)
{
  if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void pb_var(struct pb *b, uint64_t v)
{
  uint8_t t[10];
  int k = 0;
  while (v > 127) { t[k++] = (uint8_t)((v & 127) | 128); v >>= 7; }
  t[k++] = (uint8_t)v;
  pb_raw(b, t, (size_t)k);
}
static void pb_int(struct pb *b, int field, uint64_t v) { pb_var(b, (uint64_t)field << 3); pb_var(b, v); }
static void pb_blob(struct pb *b, int field, const void *d, size_t n)
{
  pb_var(b, ((uint64_t)field << 3) | 2);
  pb_var(b, n);
  pb_raw(b, d, n);
}

// Decoded top-level fields of one reply we care about.
struct reply { uint64_t id, status, has_next; int lock; };

static int pb_get_var(const uint8_t *d, size_t n, size_t *i, uint64_t *out)
{
  uint64_t v = 0;
  for (int s = 0; s < 64; s += 7) {
    if (*i >= n) return -1;
    uint8_t c = d[(*i)++];
    v |= (uint64_t)(c & 127) << s;
    if (c < 128) { *out = v; return 0; }
  }
  return -1;
}

static int parse_reply(const uint8_t *d, size_t n, struct reply *r)
{
  memset(r, 0, sizeof *r);
  r->lock = -1;
  size_t i = 0;
  while (i < n) {
    uint64_t key, v;
    if (pb_get_var(d, n, &i, &key)) return -1;
    int field = (int)(key >> 3), type = (int)(key & 7);
    if (type == 0) {
      if (pb_get_var(d, n, &i, &v)) return -1;
      if (field == F_ID) r->id = v;
      else if (field == F_STATUS) r->status = v;
      else if (field == F_HAS_NEXT) r->has_next = v;
    } else if (type == 2) {
      if (pb_get_var(d, n, &i, &v) || v > n - i) return -1;
      if (field == F_APP_LOCK_REPLY) {
        // AppLockStatusResponse { bool locked = 1; }
        size_t j = 0;
        uint64_t k2, val = 0;
        r->lock = 0;
        while (j < v) {
          if (pb_get_var(d + i, (size_t)v, &j, &k2)) break;
          if ((k2 & 7) != 0) break;
          if (pb_get_var(d + i, (size_t)v, &j, &val)) break;
          if ((k2 >> 3) == 1) r->lock = val != 0;
        }
      }
      i += (size_t)v;
    } else {
      return -1;
    }
  }
  return 0;
}

/* ---------------- transport ---------------- */

struct conn { int fd; uint64_t id; char *err; size_t errlen; };

static int fail(struct conn *c, const char *msg)
{
  if (c->err && c->errlen) snprintf(c->err, c->errlen, "%s", msg);
  return -1;
}

static int send_all(int fd, const uint8_t *p, size_t n)
{
  while (n) {
    ssize_t w = send(fd, p, n, 0);
    if (w > 0) { p += w; n -= (size_t)w; continue; }
    if (w < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static int recv_exact(int fd, uint8_t *p, size_t n, int timeout_ms)
{
  long end = mono_ms() + timeout_ms;
  while (n) {
    long left = end - mono_ms();
    if (left <= 0) return -1;
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    int r = poll(&pf, 1, (int)left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    ssize_t k = recv(fd, p, n, 0);
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

static int recv_msg(struct conn *c, struct reply *r, int timeout_ms)
{
  uint64_t len = 0;
  for (int s = 0;; s += 7) {
    uint8_t b;
    if (s > 28 || recv_exact(c->fd, &b, 1, timeout_ms)) return -1;
    len |= (uint64_t)(b & 127) << s;
    if (b < 128) break;
  }
  if (len > 65536) return -1;
  uint8_t *buf = malloc(len ? len : 1);
  int ok = recv_exact(c->fd, buf, len, timeout_ms) == 0 && parse_reply(buf, len, r) == 0;
  free(buf);
  return ok ? 0 : -1;
}

static int send_main(struct conn *c, struct pb *inner_main_fields)
{
  struct pb frame = {0};
  pb_var(&frame, inner_main_fields->n);
  pb_raw(&frame, inner_main_fields->p, inner_main_fields->n);
  int r = send_all(c->fd, frame.p, frame.n);
  free(frame.p);
  return r;
}

// One request; waits for the final reply with our id. Returns 0, or -1.
static int request(struct conn *c, int field, const void *payload, size_t n, struct reply *last)
{
  struct pb m = {0};
  pb_int(&m, F_ID, ++c->id);
  pb_blob(&m, field, payload, n);
  int r = send_main(c, &m);
  free(m.p);
  if (r) return fail(c, "bridge send failed");
  for (;;) {
    struct reply rp;
    if (recv_msg(c, &rp, 8000)) return fail(c, "no reply from the Flipper (bridge up? Flipper at its desktop?)");
    if (rp.id != c->id) continue;  // stray screen frames / old replies
    if (rp.status) {
      char m2[96];
      snprintf(m2, sizeof m2, "Flipper RPC status %llu (another app open?)", (unsigned long long)rp.status);
      return fail(c, m2);
    }
    if (last) *last = rp;
    if (!rp.has_next) return 0;
  }
}

static int connect_host(const char *host, int port)
{
  char ps[16];
  snprintf(ps, sizeof ps, "%d", port);
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
  if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return -1; }
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  int r = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (r < 0) { close(fd); return -1; }
  return fd;
}

static int upload(struct conn *c, const char *path, const uint8_t *data, size_t n)
{
  // StorageWriteRequest split into <=512-byte File.data records under one id.
  c->id++;
  for (size_t off = 0; off < n; off += 512) {
    size_t k = n - off < 512 ? n - off : 512;
    struct pb file = {0}, wr = {0}, m = {0};
    pb_blob(&file, 4, data + off, k);
    pb_blob(&wr, 1, path, strlen(path));
    pb_blob(&wr, 2, file.p, file.n);
    pb_int(&m, F_ID, c->id);
    pb_int(&m, F_HAS_NEXT, off + 512 < n);
    pb_blob(&m, F_STORAGE_WRITE, wr.p, wr.n);
    int r = send_main(c, &m);
    free(file.p); free(wr.p); free(m.p);
    if (r) return fail(c, "bridge send failed during upload");
  }
  for (;;) {
    struct reply rp;
    if (recv_msg(c, &rp, 8000)) return fail(c, "no reply to the upload");
    if (rp.id != c->id) continue;
    if (rp.status) return fail(c, "Flipper rejected the script upload (SD card?)");
    if (!rp.has_next) return 0;
  }
}

static int input(struct conn *c, int key)
{
  static const int seq[3] = {EV_PRESS, EV_SHORT, EV_RELEASE};
  for (int i = 0; i < 3; i++) {
    struct pb in = {0};
    pb_int(&in, 1, (uint64_t)key);
    pb_int(&in, 2, (uint64_t)seq[i]);
    int r = request(c, F_GUI_INPUT, in.p, in.n, NULL);
    free(in.p);
    if (r) return -1;
  }
  return 0;
}

static int app_locked(struct conn *c)
{
  struct reply rp;
  if (request(c, F_APP_LOCK_STATUS, "", 0, &rp)) return -1;
  return rp.lock;
}

/* ---------------- script ---------------- */

// en-UK: characters that need Shift (what flipper2jb's tested layout says).
static int needs_shift(int ch)
{
  if (ch >= 'A' && ch <= 'Z') return 1;
  return strchr("!\"$%^&*()_+{}:@~<>?|", ch) != NULL && ch != 0;
}

struct script { char *p; size_t n, cap; long ms; };

static void sadd(struct script *s, const char *fmt, ...)
{
  char line[64];
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (k <= 0) return;
  if (s->n + (size_t)k + 1 > s->cap) { s->cap = (s->n + (size_t)k + 1) * 2; s->p = realloc(s->p, s->cap); }
  memcpy(s->p + s->n, line, (size_t)k);
  s->n += (size_t)k;
  s->p[s->n] = 0;
}

static void key(struct script *s, const char *name, int shift, int ms)
{
  if (shift) { sadd(s, "HOLD SHIFT\nDELAY %d\n", ms); s->ms += ms; }
  sadd(s, "HOLD %s\nDELAY %d\nRELEASE %s\nDELAY %d\n", name, ms, name, ms);
  s->ms += 2L * ms;
  if (shift) { sadd(s, "RELEASE SHIFT\nDELAY %d\n", ms); s->ms += ms; }
}

// Builds the script; returns bytes of input covered (may stop early on size).
static size_t build(struct script *s, const char *text, size_t len)
{
  // Key hold and gap; raise SV_FLIPPER_KEY_MS if the console drops characters.
  int ms = env_int("SV_FLIPPER_KEY_MS", 10, 5, 250);
  // Lead-in after Bad KB starts, while the console re-enumerates the USB
  // keyboard. Raise SV_FLIPPER_START_MS if the first characters of a command go
  // missing (the console wasn't ready yet).
  int start = env_int("SV_FLIPPER_START_MS", 900, 300, 15000);
  sadd(s, "REM SwitchView keyboard job\nDELAY %d\n", start);
  s->ms += start;
  size_t i = 0;
  for (; i < len && s->n < SCRIPT_MAX; i++) {
    unsigned char c = (unsigned char)text[i];
    char one[2] = {(char)c, 0};
    switch (c) {
    case '\n': key(s, "ENTER", 0, ms > 40 ? ms : 40); break;
    case '\r': if (i + 1 < len && text[i + 1] == '\n') break; key(s, "ENTER", 0, 100); break;
    case '\b': key(s, "BACKSPACE", 0, ms); break;
    case '\t': key(s, "TAB", 0, ms); break;
    case 27: key(s, "ESC", 0, ms); break;
    case ' ': key(s, "SPACE", 0, ms); break;
    case SV_KEY_CAPSLOCK: key(s, "CAPSLOCK", 0, 100); break;
    case SV_KEY_UP: key(s, "UP", 0, ms); break;
    case SV_KEY_DOWN: key(s, "DOWN", 0, ms); break;
    case SV_KEY_LEFT: key(s, "LEFT", 0, ms); break;
    case SV_KEY_RIGHT: key(s, "RIGHT", 0, ms); break;
    default:
      if (c > 32 && c < 127) key(s, one, needs_shift(c), ms);
      break;  // anything else is not typeable on the tested layout: skipped
    }
  }
  return i;
}

double flipper_estimate(const char *text, size_t len)
{
  struct script s = {0};
  build(&s, text, len);
  free(s.p);
  return s.ms / 1000.0 + 4.0;
}

long flipper_type(const char *host, int port, const char *text, size_t len, char *err, size_t errlen)
{
  struct conn c = {.fd = -1, .id = (uint64_t)(mono_ms() % 100000) + 1000, .err = err, .errlen = errlen};
  if (err && errlen) err[0] = 0;
  struct script s = {0};
  size_t used = build(&s, text, len);
  if (!used) { free(s.p); return 0; }

  c.fd = connect_host(host, port);
  if (c.fd < 0) { free(s.p); fail(&c, "cannot connect to the Flipper bridge"); return -1; }
  long rc = -1;
  struct pb st = {0};
  if (app_locked(&c) == 1) { fail(&c, "an app is open on the Flipper; return it to the desktop"); goto out; }
  if (upload(&c, REMOTE, (const uint8_t *)s.p, s.n)) goto out;
  pb_blob(&st, 1, "Bad KB", 6);
  pb_blob(&st, 2, REMOTE, strlen(REMOTE));
  if (request(&c, F_APP_START, st.p, st.n, NULL)) goto out;
  usleep((useconds_t)env_int("SV_FLIPPER_LOAD_MS", 650, 200, 10000) * 1000);  // Bad KB loads the script first
  if (input(&c, KEY_OK)) goto out;
  {
    // Wait the script's own duration plus parser/USB overhead, then leave.
    long wait = s.ms + s.ms / 8 + env_int("SV_FLIPPER_CLOSE_MS", 350, 0, 30000);
    long end = mono_ms() + wait;
    while (mono_ms() < end) {
      usleep(150 * 1000);
      int lk = app_locked(&c);
      if (lk == 0) { rc = (long)used; goto out; }  // already closed itself
      if (lk < 0) goto out;
    }
    if (input(&c, KEY_BACK)) goto out;
    for (int k = 0; k < 10; k++) {
      if (app_locked(&c) == 0) { rc = (long)used; goto out; }
      usleep(200 * 1000);
    }
    fail(&c, "Bad KB did not close after typing");
  }
out:
  free(st.p);
  free(s.p);
  close(c.fd);
  return rc;
}
