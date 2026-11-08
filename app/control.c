#include "control.h"
#include "http.h"
#include "switchview_proto.h"

#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct control {
  struct control_ctx ctx;
  int fd;
  pthread_t th;
  atomic_int quit;
  pthread_mutex_t lock;  // guards below
  struct link_state input;
  int input_active;
  char text[4096];
  size_t text_len;
  struct link_stats link;
  char link_desc[160];
};

/* ---------------- small helpers ---------------- */

struct buf {
  char *p;
  size_t n, cap;
};

static void bput(struct buf *b, const char *s, size_t n)
{
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void bprintf(struct buf *b, const char *fmt, ...)
{
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) bput(b, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

static void bjson(struct buf *b, const char *s)
{
  bput(b, "\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; bput(b, e, 2); }
    else if (c == '\n') bput(b, "\\n", 2);
    else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); bput(b, e, 6); }
    else bput(b, (const char *)&c, 1);
  }
  bput(b, "\"", 1);
}

static int hexval(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void urldecode(char *s)
{
  char *o = s;
  for (; *s; s++) {
    if (*s == '+') *o++ = ' ';
    else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
      *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
      s += 2;
    } else *o++ = *s;
  }
  *o = 0;
}

// Value of ?name=... into out (decoded); returns 1 if present.
static int qarg(const char *query, const char *name, char *out, size_t len)
{
  size_t nl = strlen(name);
  for (const char *p = query; p && *p;) {
    const char *amp = strchr(p, '&');
    size_t seg = amp ? (size_t)(amp - p) : strlen(p);
    if (seg > nl && !strncmp(p, name, nl) && p[nl] == '=') {
      size_t vl = seg - nl - 1 < len - 1 ? seg - nl - 1 : len - 1;
      memcpy(out, p + nl + 1, vl);
      out[vl] = 0;
      urldecode(out);
      return 1;
    }
    if (seg == nl && !strncmp(p, name, nl)) { out[0] = 0; return 1; }
    p = amp ? amp + 1 : NULL;
  }
  return 0;
}

static int qflag(const char *query, const char *name)
{
  char v[16];
  return qarg(query, name, v, sizeof v) && strcmp(v, "0") && strcmp(v, "false");
}

static void wait_ms(struct control *c, int ms)
{
  for (int t = 0; t < ms && !atomic_load(&c->quit); t += 5) usleep(5000);
}

/* ---------------- pad / text ---------------- */

static uint32_t parse_buttons(const char *list)
{
  static const struct { const char *name; uint32_t bit; } B[] = {
    {"A", SV_BTN_A}, {"B", SV_BTN_B}, {"X", SV_BTN_X}, {"Y", SV_BTN_Y},
    {"L", SV_BTN_L}, {"R", SV_BTN_R}, {"ZL", SV_BTN_ZL}, {"ZR", SV_BTN_ZR},
    {"PLUS", SV_BTN_PLUS}, {"MINUS", SV_BTN_MINUS}, {"HOME", SV_BTN_HOME},
    {"CAPTURE", SV_BTN_CAPTURE}, {"L3", SV_BTN_L3}, {"R3", SV_BTN_R3},
  };
  uint32_t bits = 0;
  char tmp[128];
  snprintf(tmp, sizeof tmp, "%s", list);
  for (char *tok = strtok(tmp, ", "); tok; tok = strtok(NULL, ", "))
    for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++)
      if (!strcasecmp(tok, B[i].name)) bits |= B[i].bit;
  return bits;
}

static int parse_hat(const char *d)
{
  static const char *const N[] = {"up", "upright", "right", "downright", "down",
                                  "downleft", "left", "upleft"};
  for (int i = 0; i < 8; i++) if (!strcasecmp(d, N[i])) return i;
  return -1;
}

static void set_input(struct control *c, uint32_t bits, int hat, int active)
{
  pthread_mutex_lock(&c->lock);
  c->input.buttons = bits;
  c->input.hat = (uint8_t)(hat < 0 ? SV_HAT_CENTER : hat);
  c->input_active = active;
  pthread_mutex_unlock(&c->lock);
}

static void queue_text(struct control *c, const char *t, size_t n)
{
  pthread_mutex_lock(&c->lock);
  if (n > sizeof c->text - 1 - c->text_len) n = sizeof c->text - 1 - c->text_len;
  memcpy(c->text + c->text_len, t, n);
  c->text_len += n;
  pthread_mutex_unlock(&c->lock);
}

static void wait_typed(struct control *c, int max_s)
{
  uint32_t end = SDL_GetTicks() + (uint32_t)max_s * 1000;
  for (;;) {
    pthread_mutex_lock(&c->lock);
    int busy = c->text_len > 0;
    pthread_mutex_unlock(&c->lock);
    if (!busy) break;
    if ((int32_t)(end - SDL_GetTicks()) <= 0) return;
    wait_ms(c, 10);
  }
  wait_ms(c, 100);
  for (;;) {
    pthread_mutex_lock(&c->lock);
    int busy = c->link.typing;
    pthread_mutex_unlock(&c->lock);
    if (!busy || (int32_t)(end - SDL_GetTicks()) <= 0) return;
    wait_ms(c, 20);
  }
}

/* ---------------- endpoints ---------------- */

static void add_log(struct buf *b, struct control *c, int n)
{
  static char lines[SCRIPT_LOG_LINES][SCRIPT_LOG_COLS];
  if (n > SCRIPT_LOG_LINES) n = SCRIPT_LOG_LINES;
  int k = script_log(c->ctx.script, lines, n);
  bput(b, "[", 1);
  for (int i = 0; i < k; i++) {
    if (i) bput(b, ",", 1);
    bjson(b, lines[i]);
  }
  bput(b, "]", 1);
}

static void state_json(struct control *c, struct buf *b, int want_text)
{
  struct frame_shot f;
  int got = frame_grab(c->ctx.cap, &f, 600) == 0;
  char match[64] = "";
  float dist = 999;
  if (got && c->ctx.frames) dist = frame_lib_match(c->ctx.frames, &f, match, sizeof match);
  char *text = calloc(1, 8192);
  if (want_text && c->ctx.ocr) {
    uint32_t since = SDL_GetTicks();
    ocr_request(c->ctx.ocr, 2500);
    uint32_t started = 0;
    for (int i = 0; i < 300; i++) {  // a fresh read (<=1.5 s)
      if (ocr_latest(c->ctx.ocr, text, 8192, &started) && (int32_t)(started - since) >= 0) break;
      wait_ms(c, 5);
    }
  }
  char screen[64], cur[160], pstat[160] = "";
  script_screen(c->ctx.script, screen, sizeof screen);
  script_current(c->ctx.script, cur, sizeof cur);
  if (c->ctx.power) power_status(c->ctx.power, pstat, sizeof pstat);
  pthread_mutex_lock(&c->lock);
  struct link_stats ls = c->link;
  char desc[160];
  snprintf(desc, sizeof desc, "%s", c->link_desc);
  pthread_mutex_unlock(&c->lock);

  bprintf(b, "{\"picture\":%s,\"luma_mean\":%.1f,\"luma_sd\":%.1f,",
          got && !frame_flat(&f) ? "true" : "false", got ? f.mean : 0, got ? f.sd : 0);
  bput(b, "\"match\":", 8);
  if (match[0] && dist < 15) bjson(b, match); else bput(b, "null", 4);
  bprintf(b, ",\"match_dist\":%.2f,\"script\":", dist);
  if (script_running(c->ctx.script)) bjson(b, cur); else bput(b, "null", 4);
  bput(b, ",\"last_screen_label\":", 21);
  bjson(b, screen);
  bprintf(b, ",\"link\":{\"up\":%s,\"desc\":", ls.up ? "true" : "false");
  bjson(b, desc);
  bprintf(b, ",\"typing\":%s,\"rtt_ms\":%.0f,\"sent\":%lu,\"errors\":%lu},\"power\":",
          ls.typing ? "true" : "false", ls.rtt_ms, ls.packets, ls.send_errors);
  if (c->ctx.power) {
    bprintf(b, "{\"busy\":%s,\"status\":", power_busy(c->ctx.power) ? "true" : "false");
    bjson(b, pstat);
    bput(b, "}", 1);
  } else bput(b, "null", 4);
  bprintf(b, ",\"ocr_engine\":\"%s\"", ocr_engine());
  if (want_text) { bput(b, ",\"text\":", 8); bjson(b, text); }
  bput(b, "}\n", 2);
  free(text);
}

static void wait_script(struct control *c, int max_s)
{
  uint32_t end = SDL_GetTicks() + (uint32_t)max_s * 1000;
  wait_ms(c, 50);
  while (script_running(c->ctx.script) && (int32_t)(end - SDL_GetTicks()) > 0) wait_ms(c, 50);
}

static int safe_name(const char *s)
{
  if (!*s || strstr(s, "..") || strchr(s, '/')) return 0;
  for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '.')) return 0;
  return 1;
}

static int route(struct control *c, const char *method, const char *path, const char *query,
                 const char *body, size_t body_len, struct buf *out, const char **ctype)
{
  char v[512];
  *ctype = "application/json";
  if (!strcmp(path, "/") || !strcmp(path, "/help")) {
    *ctype = "text/plain";
    bprintf(out, "SwitchView control API\n"
                 "/state  /goto?target=smilebasic|seedsow|console[&wait=1]  /run?script=X.lua[&wait=1]\n"
                 "/stop  /log?n=40  /tap?btn=A&ms=120  /dpad?dir=down&ms=100\n"
                 "POST /type (body) or /type?text=..[&wait=1]  /power?action=toggle|boot|off|restart[&wait=1]\n"
                 "/learn?label=X  /forget?label=X  /frame.pgm\n");
    return 200;
  }
  if (!strcmp(path, "/state")) { state_json(c, out, !qarg(query, "notext", v, sizeof v)); return 200; }
  if (!strcmp(path, "/log")) {
    int n = qarg(query, "n", v, sizeof v) ? atoi(v) : 40;
    bput(out, "{\"log\":", 7);
    add_log(out, c, n > 0 ? n : 40);
    bput(out, "}\n", 2);
    return 200;
  }
  if (!strcmp(path, "/goto") || !strcmp(path, "/run")) {
    char name[160];
    if (!strcmp(path, "/goto")) {
      if (!qarg(query, "target", v, sizeof v) || !safe_name(v)) { bprintf(out, "{\"error\":\"target?\"}\n"); return 400; }
      snprintf(name, sizeof name, "goto_%s.lua", v);
    } else {
      if (!qarg(query, "script", v, sizeof v) || !safe_name(v)) { bprintf(out, "{\"error\":\"script?\"}\n"); return 400; }
      snprintf(name, sizeof name, "%s%s", v, strstr(v, ".lua") ? "" : ".lua");
    }
    if (script_start_name(c->ctx.script, name) != 0) {
      bprintf(out, "{\"error\":\"could not start %s\"}\n", name);
      return 404;
    }
    if (qflag(query, "wait")) {
      wait_script(c, qarg(query, "timeout", v, sizeof v) ? atoi(v) : 600);
      bput(out, "{\"done\":", 8);
      bput(out, script_running(c->ctx.script) ? "false" : "true", script_running(c->ctx.script) ? 5 : 4);
      bput(out, ",\"log\":", 7);
      add_log(out, c, 25);
      bput(out, "}\n", 2);
    } else {
      bprintf(out, "{\"started\":\"%s\"}\n", name);
    }
    return 200;
  }
  if (!strcmp(path, "/companion")) {
    *ctype = "text/plain";
    if (!c->ctx.companion) { bprintf(out, "no companion watcher\n"); return 404; }
    static struct comp_line cl[COMP_LINES];
    int want = qarg(query, "n", v, sizeof v) ? atoi(v) : 40;
    int n = companion_lines(c->ctx.companion, cl, want > 0 && want <= COMP_LINES ? want : 40);
    for (int i = 0; i < n; i++) bprintf(out, "%s\n", cl[i].text);
    return 200;
  }
  if (!strcmp(path, "/stop")) { script_stop(c->ctx.script); bprintf(out, "{\"stopped\":true}\n"); return 200; }
  if (!strcmp(path, "/tap")) {
    // Adds to whatever the panel and a running script press (see main.c), like
    // a second pair of hands on the same pad.
    uint32_t bits = qarg(query, "btn", v, sizeof v) ? parse_buttons(v) : 0;
    int ms = qarg(query, "ms", v, sizeof v) ? atoi(v) : 120;
    if (!bits) { bprintf(out, "{\"error\":\"btn?\"}\n"); return 400; }
    set_input(c, bits, -1, 1);
    wait_ms(c, ms > 0 ? ms : 120);
    set_input(c, 0, -1, 1);
    wait_ms(c, 60);
    set_input(c, 0, -1, 0);
    bprintf(out, "{\"ok\":true}\n");
    return 200;
  }
  if (!strcmp(path, "/dpad")) {
    int hat = qarg(query, "dir", v, sizeof v) ? parse_hat(v) : -1;
    int ms = qarg(query, "ms", v, sizeof v) ? atoi(v) : 100;
    if (hat < 0) { bprintf(out, "{\"error\":\"dir?\"}\n"); return 400; }
    set_input(c, 0, hat, 1);
    wait_ms(c, ms > 0 ? ms : 100);
    set_input(c, 0, -1, 1);
    wait_ms(c, 60);
    set_input(c, 0, -1, 0);
    bprintf(out, "{\"ok\":true}\n");
    return 200;
  }
  if (!strcmp(path, "/type")) {
    static char t[4096];
    size_t n = 0;
    if (!strcmp(method, "POST") && body_len) {
      n = body_len < sizeof t ? body_len : sizeof t - 1;
      memcpy(t, body, n);
    } else if (qarg(query, "text", t, sizeof t)) {
      n = strlen(t);
    }
    if (!n) { bprintf(out, "{\"error\":\"no text\"}\n"); return 400; }
    queue_text(c, t, n);
    if (qflag(query, "wait")) wait_typed(c, 120);
    bprintf(out, "{\"queued\":%zu}\n", n);
    return 200;
  }
  if (!strcmp(path, "/power")) {
    if (!c->ctx.power) { bprintf(out, "{\"error\":\"no fingerbot (-B)\"}\n"); return 404; }
    if (!qarg(query, "action", v, sizeof v) || power_start(c->ctx.power, v) != 0) {
      bprintf(out, "{\"error\":\"unknown action or busy\"}\n");
      return 409;
    }
    if (qflag(query, "wait")) { wait_ms(c, 50); while (power_busy(c->ctx.power) && !atomic_load(&c->quit)) wait_ms(c, 100); }
    char st[160];
    power_status(c->ctx.power, st, sizeof st);
    bput(out, "{\"status\":", 10);
    bjson(out, st);
    bput(out, "}\n", 2);
    return 200;
  }
  if (!strcmp(path, "/learn") || !strcmp(path, "/forget")) {
    if (!c->ctx.frames || !qarg(query, "label", v, sizeof v)) { bprintf(out, "{\"error\":\"label?\"}\n"); return 400; }
    if (!strcmp(path, "/forget")) {
      bprintf(out, "{\"removed\":%d}\n", frame_lib_forget(c->ctx.frames, v));
      return 200;
    }
    struct frame_shot f;
    int ok = frame_grab(c->ctx.cap, &f, 1000) == 0 && frame_lib_learn(c->ctx.frames, v, &f) == 0;
    bprintf(out, "{\"learned\":%s,\"screens\":%d}\n", ok ? "true" : "false", frame_lib_count(c->ctx.frames));
    return ok ? 200 : 500;
  }
  if (!strcmp(path, "/frame.pgm")) {
    unsigned w = capture_width(c->ctx.cap), h = capture_height(c->ctx.cap);
    uint8_t *luma = malloc((size_t)w * h);
    unsigned before = capture_luma_seq(c->ctx.cap);
    capture_want_luma(c->ctx.cap);
    for (int i = 0; i < 100 && capture_luma_seq(c->ctx.cap) == before; i++) { capture_want_luma(c->ctx.cap); wait_ms(c, 5); }
    if (!capture_copy_luma(c->ctx.cap, luma)) { free(luma); bprintf(out, "{\"error\":\"no frame\"}\n"); return 503; }
    *ctype = "image/x-portable-graymap";
    bprintf(out, "P5\n%u %u\n255\n", w, h);
    bput(out, (const char *)luma, (size_t)w * h);
    free(luma);
    return 200;
  }
  bprintf(out, "{\"error\":\"not found\"}\n");
  return 404;
}

/* ---------------- server ---------------- */

static void serve_one(struct control *c, int fd)
{
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  size_t cap = 16384, n = 0;
  char *req = malloc(cap + 1);
  char *hend = NULL;
  while (!hend && n < cap) {
    ssize_t r = recv(fd, req + n, cap - n, 0);
    if (r <= 0) break;
    n += (size_t)r;
    req[n] = 0;
    hend = strstr(req, "\r\n\r\n");
  }
  if (!hend) { free(req); return; }
  size_t clen = 0;
  for (char *h = req; h < hend; h = strstr(h, "\r\n") + 2) {
    if (!strncasecmp(h, "Content-Length:", 15)) clen = (size_t)atol(h + 15);
    if (!strstr(h, "\r\n")) break;
  }
  size_t hlen = (size_t)(hend + 4 - req);
  if (clen > 1 << 20) clen = 1 << 20;
  if (hlen + clen > cap) { req = realloc(req, hlen + clen + 1); cap = hlen + clen; hend = req + hlen - 4; }
  while (n < hlen + clen) {
    ssize_t r = recv(fd, req + n, hlen + clen - n, 0);
    if (r <= 0) break;
    n += (size_t)r;
  }
  req[n] = 0;

  char method[8] = "", target[2048] = "";
  sscanf(req, "%7s %2047s", method, target);
  char *query = strchr(target, '?');
  if (query) *query++ = 0;

  struct buf out = {0};
  const char *ctype = "application/json";
  int code = route(c, method, target, query ? query : "", req + hlen, n > hlen ? n - hlen : 0, &out, &ctype);
  char head[256];
  int hn = snprintf(head, sizeof head,
                    "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                    code, code == 200 ? "OK" : "Error", ctype, out.n);
  send(fd, head, (size_t)hn, 0);
  for (size_t off = 0; off < out.n;) {
    ssize_t w = send(fd, out.p + off, out.n - off, 0);
    if (w <= 0) break;
    off += (size_t)w;
  }
  free(out.p);
  free(req);
}

// Mark an fd close-on-exec so children we spawn (the NS2 host via the Lua
// runner, companion tails) don't inherit it. Without this the host inherited
// our listening socket and kept 127.0.0.1:<port> bound after we exited, so the
// next SwitchView could not bind its control API until that orphan died.
static void set_cloexec(int fd)
{
  int f = fcntl(fd, F_GETFD);
  if (f >= 0) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

static void *server(void *arg)
{
  struct control *c = arg;
  while (!atomic_load(&c->quit)) {
    struct pollfd p = {.fd = c->fd, .events = POLLIN};
    if (poll(&p, 1, 200) <= 0) continue;
    int fd = accept(c->fd, NULL, NULL);
    if (fd < 0) continue;
    set_cloexec(fd);  // a spawn mid-request must not inherit the client socket
    serve_one(c, fd);
    close(fd);
  }
  return NULL;
}

struct control *control_start(int port, const struct control_ctx *ctx)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  set_cloexec(fd);  // the NS2 host we spawn must not inherit the listening port
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never exposed to the LAN
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
    fprintf(stderr, "control: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
    close(fd);
    return NULL;
  }
  struct control *c = calloc(1, sizeof *c);
  c->ctx = *ctx;
  c->fd = fd;
  c->input.hat = SV_HAT_CENTER;
  pthread_mutex_init(&c->lock, NULL);
  if (pthread_create(&c->th, NULL, server, c) != 0) {
    close(fd);
    free(c);
    return NULL;
  }
  fprintf(stderr, "control: http://127.0.0.1:%d/\n", port);
  return c;
}

void control_stop(struct control *c)
{
  if (!c) return;
  atomic_store(&c->quit, 1);
  pthread_join(c->th, NULL);
  close(c->fd);
  pthread_mutex_destroy(&c->lock);
  free(c);
}

void control_get_input(struct control *c, struct link_state *out, int *active)
{
  pthread_mutex_lock(&c->lock);
  *out = c->input;
  *active = c->input_active;
  pthread_mutex_unlock(&c->lock);
}

int control_take_text(struct control *c, char *buf, size_t len)
{
  pthread_mutex_lock(&c->lock);
  size_t n = c->text_len < len - 1 ? c->text_len : len - 1;
  memcpy(buf, c->text, n);
  buf[n] = 0;
  memmove(c->text, c->text + n, c->text_len - n);
  c->text_len -= n;
  pthread_mutex_unlock(&c->lock);
  return n > 0;
}

void control_set_link(struct control *c, const struct link_stats *st, const char *desc)
{
  pthread_mutex_lock(&c->lock);
  c->link = *st;
  snprintf(c->link_desc, sizeof c->link_desc, "%s", desc);
  pthread_mutex_unlock(&c->lock);
}
