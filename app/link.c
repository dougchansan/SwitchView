#include "link.h"
#include "flipper.h"
#include "http.h"
#include "switchview_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define KEEPALIVE_MS 20   // 50 Hz state refresh while idle
#define PING_MS 500       // link-health probe interval
#define UP_TIMEOUT_MS 2000
#define RESOCKET_MS 5000  // no echo for this long: rebuild the socket (interface may have changed)

// Text pacing: the firmware types ~10 chars/s from a 255-byte queue. A token
// bucket lets single keystrokes go out instantly while long pastes never
// overflow the Pico's queue.
#define TEXT_BUCKET_MAX 200.0f

// ESP32 pro-2 HTTP pad. A held button is re-sent with a short ms so the
// firmware's own timer releases it if SwitchView dies (a dead-man switch).
#define ESP_HOLD_MS 400
#define ESP_REFRESH_MS 150
#define ESP_MIN_PRESS_MS 50     // the console needs a few reports to see a press
#define ESP_STATUS_MS 2000
#define ESP_UP_MS 5000
#define ESP_QUEUE 32
// The console stops polling the pad's USB endpoint while it sleeps and does not
// always pick it up again after waking: the board still answers HTTP, but
// presses go nowhere. A console that is using the pad sends it output reports
// all the time (~16/s, "rawSet" on /usb, even when idle); when that count stops
// moving for this long the pad counts as stalled; the next press reboots the board (at most
// once per ESP_RESET_COOLDOWN_MS) so it re-attaches like a replugged pad.
#define ESP_STALL_MS 4000
#define ESP_RESET_COOLDOWN_MS 30000
#define ESP_REATTACH_MS 4000

// Flipper Bad KB jobs.
#define FLIP_IDLE_FLUSH_MS 500  // send a partial line after this pause (live typing)

// pro-2 button bits (tools/esp32_pro/esp32_pro.ino).
enum {
  E_Y = 0x0001, E_X = 0x0002, E_B = 0x0004, E_A = 0x0008,
  E_DOWN = 0x0100, E_UP = 0x0200, E_RIGHT = 0x0400, E_LEFT = 0x0800,
  E_R = 0x4000, E_ZR = 0x8000, E_MINUS = 0x10000, E_PLUS = 0x20000,
  E_R3 = 0x40000, E_L3 = 0x80000, E_HOME = 0x100000, E_CAPTURE = 0x200000,
  E_L = 0x400000, E_ZL = 0x800000,
};

enum { PAD_UDP, PAD_HTTP };
enum { KB_NONE, KB_UDP, KB_FLIPPER };

struct link {
  int pad, kb;
  char desc[160];
  struct link_state cur;

  // UDP
  int fd;
  struct sockaddr_in dst;
  int dirty;
  uint32_t last_state_ms, last_text_ms, last_ping_ms, last_echo_ms, last_resocket_ms;
  float tokens;
  int mdx, mdy, mwheel, mdirty;
  unsigned mbtn;
  char textq[1024];
  unsigned tq_head, tq_tail;

  // HTTP worker (ESP32 pad and/or Flipper keyboard)
  char pad_url[256], kb_url[256];
  pthread_t th;
  int have_thread;
  atomic_int quit;
  pthread_mutex_t lock;          // guards everything below
  uint32_t q[ESP_QUEUE];         // pending pad masks, in order
  int qn;
  uint32_t last_pushed;
  char *ftext;                   // pending Flipper text
  size_t ftext_len, ftext_cap;
  long ftext_last_ms;
  int flip_busy;                 // a job is being sent / typed
  long flip_typing_until;
  long esp_last_ok;
  long pad_stall_since;          // console stopped sending output reports at (0 = fine)
  long pad_raw_set;              // last "rawSet" count read from /usb (-1 = none yet)
  int pad_stalled;
  long last_pad_reset;

  struct link_stats st;
};

static long mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---------------- ESP32 pad mapping ---------------- */

static uint32_t esp_mask(const struct link_state *s)
{
  static const struct { uint32_t sv, esp; } MAP[] = {
    {SV_BTN_A, E_A}, {SV_BTN_B, E_B}, {SV_BTN_X, E_X}, {SV_BTN_Y, E_Y},
    {SV_BTN_L, E_L}, {SV_BTN_R, E_R}, {SV_BTN_ZL, E_ZL}, {SV_BTN_ZR, E_ZR},
    {SV_BTN_PLUS, E_PLUS}, {SV_BTN_MINUS, E_MINUS}, {SV_BTN_HOME, E_HOME},
    {SV_BTN_CAPTURE, E_CAPTURE}, {SV_BTN_L3, E_L3}, {SV_BTN_R3, E_R3},
  };
  uint32_t m = 0;
  for (unsigned i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
    if (s->buttons & MAP[i].sv) m |= MAP[i].esp;
  int up = 0, down = 0, left = 0, right = 0;
  switch (s->hat) {
  case SV_HAT_UP: up = 1; break;
  case SV_HAT_UPRIGHT: up = right = 1; break;
  case SV_HAT_RIGHT: right = 1; break;
  case SV_HAT_DOWNRIGHT: down = right = 1; break;
  case SV_HAT_DOWN: down = 1; break;
  case SV_HAT_DOWNLEFT: down = left = 1; break;
  case SV_HAT_LEFT: left = 1; break;
  case SV_HAT_UPLEFT: up = left = 1; break;
  default:
    // pro-2 has no analog sticks: a firmly pushed left stick drives the
    // d-pad, which is what menu navigation needs.
    if (s->ly > 20000) up = 1;
    if (s->ly < -20000) down = 1;
    if (s->lx > 20000) right = 1;
    if (s->lx < -20000) left = 1;
  }
  if (up) m |= E_UP;
  if (down) m |= E_DOWN;
  if (left) m |= E_LEFT;
  if (right) m |= E_RIGHT;
  return m;
}

static int esp_send(struct link *l, uint32_t mask)
{
  char url[320], resp[256];
  if (mask) snprintf(url, sizeof url, "%s/pro?mask=0x%X&ms=%d", l->pad_url, mask, ESP_HOLD_MS);
  else snprintf(url, sizeof url, "%s/pro?mask=0", l->pad_url);
  long t0 = mono_ms();
  int code = http_get(url, resp, sizeof resp, 1500);
  pthread_mutex_lock(&l->lock);
  if (code == 200) {
    l->st.packets++;
    l->st.rtt_ms = (float)(mono_ms() - t0);
    l->esp_last_ok = mono_ms();
  } else {
    l->st.send_errors++;
  }
  pthread_mutex_unlock(&l->lock);
  return code == 200;
}

/* ---------------- Flipper Bad KB ---------------- */

// One keyboard job through the Flipper RPC bridge (app/flipper.c). Blocks
// while Bad KB types; the pro-2 board pauses the pad for the bridge session
// anyway. Returns how many input bytes were typed.
static size_t flip_job(struct link *l, const char *text, size_t len)
{
  char host[200], err[200];
  const char *h = l->kb_url + 7;
  size_t hn = strcspn(h, ":/");
  snprintf(host, sizeof host, "%.*s", (int)hn, h);
  int port = getenv("SV_FLIPPER_PORT") ? atoi(getenv("SV_FLIPPER_PORT")) : 8080;
  pthread_mutex_lock(&l->lock);
  l->flip_typing_until = mono_ms() + (long)(flipper_estimate(text, len) * 1000);
  pthread_mutex_unlock(&l->lock);
  long n = flipper_type(host, port, text, len, err, sizeof err);
  pthread_mutex_lock(&l->lock);
  l->flip_typing_until = 0;
  if (n >= 0) { l->st.packets++; l->esp_last_ok = mono_ms(); }
  else l->st.send_errors++;
  // Pad changes made while typing were not delivered: keep only the latest.
  if (l->qn > 1) { l->q[0] = l->q[l->qn - 1]; l->qn = 1; }
  pthread_mutex_unlock(&l->lock);
  if (n < 0) {
    fprintf(stderr, "link: Flipper typing failed: %s\n", err);
    return len;  // drop it rather than retyping half a line
  }
  return n > 0 ? (size_t)n : len;
}

/* ---------------- worker ---------------- */

static void *worker(void *arg)
{
  struct link *l = arg;
  uint32_t sent = 0;
  long sent_ms = 0, last_status = 0;
  while (!atomic_load(&l->quit)) {
    long now = mono_ms();
    int did = 0;

    if (l->pad == PAD_HTTP) {
      uint32_t next = 0;
      int have = 0;
      pthread_mutex_lock(&l->lock);
      if (l->qn) {
        next = l->q[0];
        memmove(l->q, l->q + 1, sizeof l->q[0] * (size_t)--l->qn);
        have = 1;
      }
      pthread_mutex_unlock(&l->lock);
      if (have && next && l->pad_stalled && mono_ms() - l->last_pad_reset >= ESP_RESET_COOLDOWN_MS &&
          mono_ms() >= l->flip_typing_until) {
        // Someone is pressing buttons, so the console is presumably awake but
        // has dropped the pad: reboot the board so it re-attaches.
        char url[300], resp[128];
        fprintf(stderr, "link: pad stalled; rebooting the board so the console re-attaches it\n");
        snprintf(url, sizeof url, "%s/reboot", l->pad_url);
        http_get(url, resp, sizeof resp, 1500);
        pthread_mutex_lock(&l->lock);
        l->last_pad_reset = mono_ms();
        l->pad_stalled = 0;
        l->pad_stall_since = 0;
        pthread_mutex_unlock(&l->lock);
        usleep(ESP_REATTACH_MS * 1000);
        sent = 0;
      }
      if (have && next != sent) {
        // Let a press live long enough to be seen before changing it.
        if (sent && now - sent_ms < ESP_MIN_PRESS_MS) usleep((useconds_t)(ESP_MIN_PRESS_MS - (now - sent_ms)) * 1000);
        esp_send(l, next);
        sent = next;
        sent_ms = mono_ms();
        did = 1;
      } else if (sent && now - sent_ms >= ESP_REFRESH_MS) {
        esp_send(l, sent);  // keep the hold alive (dead-man timer on the board)
        sent_ms = mono_ms();
        did = 1;
      } else if (!sent && now - last_status >= ESP_STATUS_MS) {
        char url[300], resp[256];
        snprintf(url, sizeof url, "%s/status", l->pad_url);
        long t0 = mono_ms();
        if (http_get(url, resp, sizeof resp, 1500) == 200 && strstr(resp, "\"ok\":true")) {
          pthread_mutex_lock(&l->lock);
          l->esp_last_ok = mono_ms();
          l->st.rtt_ms = (float)(mono_ms() - t0);
          pthread_mutex_unlock(&l->lock);
        }
        char usb[1024];
        snprintf(url, sizeof url, "%s/usb", l->pad_url);
        if (http_get(url, usb, sizeof usb, 1500) == 200 && strstr(usb, "\"mounted\":true")) {
          const char *rs = strstr(usb, "\"rawSet\":");
          long raw = rs ? atol(rs + 9) : -1;
          long t = mono_ms();
          pthread_mutex_lock(&l->lock);
          int stuck = raw >= 0 && raw == l->pad_raw_set;
          l->pad_raw_set = raw;
          if (!stuck) l->pad_stall_since = 0, l->pad_stalled = 0;
          else if (!l->pad_stall_since) l->pad_stall_since = t;
          else if (t - l->pad_stall_since >= ESP_STALL_MS && !l->pad_stalled) {
            l->pad_stalled = 1;
            fprintf(stderr, "link: the console is not polling the pad (asleep, or lost it after waking)\n");
          }
          pthread_mutex_unlock(&l->lock);
        }
        last_status = mono_ms();
      }
    } else if (l->kb == KB_FLIPPER && now - last_status >= ESP_STATUS_MS) {
      // UDP pad, Flipper keyboard: still report whether the bridge board answers.
      char url[300], resp[256];
      snprintf(url, sizeof url, "%s/status", l->kb_url);
      if (http_get(url, resp, sizeof resp, 1500) == 200) {
        pthread_mutex_lock(&l->lock);
        l->esp_last_ok = mono_ms();
        pthread_mutex_unlock(&l->lock);
      }
      last_status = mono_ms();
    }

    if (l->kb == KB_FLIPPER) {
      char *job = NULL;
      size_t jl = 0;
      pthread_mutex_lock(&l->lock);
      if (l->ftext_len && (memchr(l->ftext, '\n', l->ftext_len) ||
                           mono_ms() - l->ftext_last_ms >= FLIP_IDLE_FLUSH_MS)) {
        // Take everything up to and including the last newline; a trailing
        // partial line goes too once typing has paused.
        size_t take = l->ftext_len;
        if (mono_ms() - l->ftext_last_ms < FLIP_IDLE_FLUSH_MS)
          while (take && l->ftext[take - 1] != '\n') take--;
        job = malloc(take);
        memcpy(job, l->ftext, take);
        jl = take;
        memmove(l->ftext, l->ftext + take, l->ftext_len - take);
        l->ftext_len -= take;
        l->flip_busy = 1;
      }
      pthread_mutex_unlock(&l->lock);
      if (job) {
        size_t used = flip_job(l, job, jl);
        pthread_mutex_lock(&l->lock);
        if (used < jl) {  // requeue the rest in front of anything typed since
          size_t rest = jl - used;
          if (l->ftext_len + rest > l->ftext_cap) {
            l->ftext_cap = (l->ftext_len + rest) * 2 + 256;
            l->ftext = realloc(l->ftext, l->ftext_cap);
          }
          memmove(l->ftext + rest, l->ftext, l->ftext_len);
          memcpy(l->ftext, job + used, rest);
          l->ftext_len += rest;
          l->ftext_last_ms = 0;  // flush it without waiting for a pause
        }
        l->flip_busy = 0;
        pthread_mutex_unlock(&l->lock);
        free(job);
        did = 1;
      }
    }
    if (!did) usleep(4000);
  }
  if (l->pad == PAD_HTTP) esp_send(l, 0);  // leave the Switch with everything released
  return NULL;
}

/* ---------------- UDP ---------------- */

static int open_socket(void)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

static void send_packet(struct link *l, const sv_packet *p)
{
  if (sendto(l->fd, p, SV_PACKET_SIZE, MSG_DONTWAIT, (struct sockaddr *)&l->dst, sizeof l->dst) < 0)
    l->st.send_errors++;
  else
    l->st.packets++;
}

static void trim_url(char *dst, size_t n, const char *src)
{
  snprintf(dst, n, "%s", src);
  size_t k = strlen(dst);
  while (k && dst[k - 1] == '/') dst[--k] = 0;
}

struct link *link_open(const char *host, int port, const char *kb_url)
{
  struct link *l = calloc(1, sizeof *l);
  l->fd = -1;
  l->cur.hat = SV_HAT_CENTER;
  l->dirty = 1;
  l->tokens = TEXT_BUCKET_MAX;
  l->st.leds = -1;
  l->pad_raw_set = -1;
  pthread_mutex_init(&l->lock, NULL);

  if (!strncmp(host, "http://", 7)) {
    l->pad = PAD_HTTP;
    trim_url(l->pad_url, sizeof l->pad_url, host);
  } else {
    l->pad = PAD_UDP;
    l->fd = open_socket();
    if (l->fd < 0) { perror("socket"); free(l); return NULL; }
    l->dst.sin_family = AF_INET;
    l->dst.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &l->dst.sin_addr) != 1) {
      fprintf(stderr, "link: bad host %s (use an IP, or http://IP for the ESP32 pro-2 board)\n", host);
      close(l->fd);
      free(l);
      return NULL;
    }
  }

  if (kb_url && !strcmp(kb_url, "none")) l->kb = KB_NONE;
  else if (kb_url && !strncmp(kb_url, "http://", 7)) { l->kb = KB_FLIPPER; trim_url(l->kb_url, sizeof l->kb_url, kb_url); }
  else if (l->pad == PAD_HTTP) { l->kb = KB_FLIPPER; snprintf(l->kb_url, sizeof l->kb_url, "%s", l->pad_url); }
  else l->kb = KB_UDP;
  l->st.kb_flipper = l->kb == KB_FLIPPER;

  int n = l->pad == PAD_HTTP ? snprintf(l->desc, sizeof l->desc, "esp32 %s", l->pad_url + 7)
                             : snprintf(l->desc, sizeof l->desc, "udp %s:%d", host, port);
  if (l->kb == KB_FLIPPER)
    snprintf(l->desc + n, sizeof l->desc - (size_t)n, " + flipper %s", l->kb_url + 7);

  if (l->pad == PAD_HTTP || l->kb == KB_FLIPPER) {
    if (pthread_create(&l->th, NULL, worker, l) == 0) l->have_thread = 1;
  }
  return l;
}

void link_close(struct link *l)
{
  if (!l) return;
  struct link_state zero = {.hat = SV_HAT_CENTER};
  link_set_state(l, &zero);
  if (l->pad == PAD_UDP) link_poll(l, l->last_state_ms + KEEPALIVE_MS + 1);
  if (l->have_thread) {
    atomic_store(&l->quit, 1);
    pthread_join(l->th, NULL);
  }
  if (l->fd >= 0) close(l->fd);
  free(l->ftext);
  pthread_mutex_destroy(&l->lock);
  free(l);
}

void link_set_state(struct link *l, const struct link_state *s)
{
  if (memcmp(&l->cur, s, sizeof *s) == 0) return;
  l->cur = *s;
  l->dirty = 1;
  if (l->pad == PAD_HTTP) {
    uint32_t m = esp_mask(s);
    pthread_mutex_lock(&l->lock);
    if (m != l->last_pushed) {
      if (l->qn == ESP_QUEUE) {  // worker far behind: keep order, drop the middle
        l->q[ESP_QUEUE - 1] = m;
      } else {
        l->q[l->qn++] = m;
      }
      l->last_pushed = m;
    }
    pthread_mutex_unlock(&l->lock);
  }
}

void link_mouse(struct link *l, int dx, int dy, int wheel, unsigned buttons)
{
  if (l->pad != PAD_UDP) return;
  l->mdx += dx; l->mdy += dy; l->mwheel += wheel;
  if (buttons != l->mbtn) { l->mbtn = buttons; l->mdirty = 1; }
  if (dx || dy || wheel) l->mdirty = 1;
}

void link_send_text(struct link *l, const char *text)
{
  if (l->kb == KB_FLIPPER) {
    size_t n = strlen(text);
    pthread_mutex_lock(&l->lock);
    if (l->ftext_len + n > l->ftext_cap) {
      l->ftext_cap = (l->ftext_len + n) * 2 + 256;
      l->ftext = realloc(l->ftext, l->ftext_cap);
    }
    memcpy(l->ftext + l->ftext_len, text, n);
    l->ftext_len += n;
    l->ftext_last_ms = mono_ms();
    pthread_mutex_unlock(&l->lock);
    return;
  }
  if (l->kb != KB_UDP) return;
  for (; *text; text++) {
    unsigned next = (l->tq_head + 1) % sizeof l->textq;
    if (next == l->tq_tail) break;
    l->textq[l->tq_head] = *text;
    l->tq_head = next;
  }
}

static void udp_poll(struct link *l, uint32_t now_ms)
{
  if (l->dirty || now_ms - l->last_state_ms >= KEEPALIVE_MS) {
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_STATE,
                   .buttons = l->cur.buttons, .lx = l->cur.lx, .ly = l->cur.ly,
                   .rx = l->cur.rx, .ry = l->cur.ry, .hat = l->cur.hat};
    send_packet(l, &p);
    l->dirty = 0;
    l->last_state_ms = now_ms;
    l->st.last_send_ms = now_ms;
  }

  if (l->mdirty) {
    int cl = l->mdx < -32767 ? -32767 : l->mdx > 32767 ? 32767 : l->mdx;
    int cy = l->mdy < -32767 ? -32767 : l->mdy > 32767 ? 32767 : l->mdy;
    int cw = l->mwheel < -127 ? -127 : l->mwheel > 127 ? 127 : l->mwheel;
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_MOUSE, .buttons = l->mbtn,
                   .lx = (int16_t)cl, .ly = (int16_t)cy, .rx = (int16_t)cw, .hat = SV_HAT_CENTER};
    send_packet(l, &p);
    l->mdx -= cl; l->mdy -= cy; l->mwheel -= cw;
    l->mdirty = 0;
  }

  l->tokens += (now_ms - l->last_text_ms) * (SV_TEXT_CHARS_PER_SEC / 1000.0f);
  if (l->tokens > TEXT_BUCKET_MAX) l->tokens = TEXT_BUCKET_MAX;
  l->last_text_ms = now_ms;
  if (l->tq_head != l->tq_tail && l->tokens >= 1.0f) {
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_TEXT, .hat = SV_HAT_CENTER};
    while (p.length < SV_TEXT_MAX && l->tq_head != l->tq_tail && l->tokens >= 1.0f) {
      p.text[p.length++] = (uint8_t)l->textq[l->tq_tail];
      l->tq_tail = (l->tq_tail + 1) % sizeof l->textq;
      l->tokens -= 1.0f;
    }
    send_packet(l, &p);
    l->st.last_send_ms = now_ms;
  }

  // Link health: ping, read echoes, and rebuild the socket if the Pico has
  // gone quiet for a while (covers the PC changing networks underneath us).
  if (now_ms - l->last_ping_ms >= PING_MS) {
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_PING, .buttons = now_ms,
                   .hat = SV_HAT_CENTER};
    send_packet(l, &p);
    l->last_ping_ms = now_ms;
  }
  sv_packet in;
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  while (recvfrom(l->fd, &in, sizeof in, MSG_DONTWAIT, (struct sockaddr *)&from, &fl) >= SV_PACKET_SIZE) {
    if (in.magic[0] == SV_MAGIC0 && in.magic[1] == SV_MAGIC1 && in.type == SV_PKT_PING) {
      l->last_echo_ms = now_ms;
      l->st.rtt_ms = (float)(now_ms - in.buttons);
      l->st.leds = in.text[1] ? in.text[0] : -1;
    }
  }
  if (!l->last_echo_ms || now_ms - l->last_echo_ms >= UP_TIMEOUT_MS) {
    if (now_ms - l->last_resocket_ms >= RESOCKET_MS) {
      int fd = open_socket();
      if (fd >= 0) { close(l->fd); l->fd = fd; }
      l->last_resocket_ms = now_ms;
    }
  }
}

void link_poll(struct link *l, uint32_t now_ms)
{
  if (l->pad == PAD_UDP) udp_poll(l, now_ms);

  int up;
  pthread_mutex_lock(&l->lock);
  if (l->pad == PAD_UDP) up = l->last_echo_ms && now_ms - l->last_echo_ms < UP_TIMEOUT_MS;
  else up = l->esp_last_ok && mono_ms() - l->esp_last_ok < ESP_UP_MS && !l->pad_stalled;
  unsigned q = l->kb == KB_FLIPPER ? (unsigned)l->ftext_len
                                   : (l->tq_head + sizeof l->textq - l->tq_tail) % sizeof l->textq;
  l->st.text_queued = q;
  l->st.typing = q > 0 || l->flip_busy || (l->kb == KB_FLIPPER && mono_ms() < l->flip_typing_until);
  pthread_mutex_unlock(&l->lock);
  if (up != l->st.up) fprintf(stderr, "link: board %s\n", up ? "up" : "not answering");
  l->st.up = up;
}

void link_get_stats(struct link *l, struct link_stats *out)
{
  pthread_mutex_lock(&l->lock);
  *out = l->st;
  pthread_mutex_unlock(&l->lock);
}

const char *link_describe(struct link *l) { return l->desc; }
int link_has_mouse(struct link *l) { return l->pad == PAD_UDP; }
