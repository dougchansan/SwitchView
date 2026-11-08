#include "power.h"
#include "frame.h"
#include "http.h"

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Measured on the rig (ns2-savekit harness.cjs): 16 s forces the console off,
// 3 s boots it (1-2 s stopped working after the arm was reseated), a short
// press toggles sleep. A boot press within ~2 s of power-off is ignored.
#define TOGGLE_S 0.5
#define BOOT_S 3
#define OFF_S 16
#define OFF_SETTLE_MS 10000
#define OFF_VERIFY_S 26
#define PICTURE_WAIT_S 75

struct power {
  char url[256];
  struct capture *cap;
  power_log_fn log;
  void *ud;
  pthread_t th;
  atomic_int busy, quit, server;
  pthread_mutex_t lock;
  char action[32];
  char status[128];
};

static void say(struct power *p, const char *fmt, ...)
{
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&p->lock);
  snprintf(p->status, sizeof p->status, "%s", line);
  pthread_mutex_unlock(&p->lock);
  fprintf(stderr, "power: %s\n", line);
  if (p->log) {
    char l2[220];
    snprintf(l2, sizeof l2, "power: %s", line);
    p->log(p->ud, l2);
  }
}

static int call(struct power *p, const char *path, int timeout_ms, char *resp, size_t rl)
{
  char url[400];
  snprintf(url, sizeof url, "%s%s", p->url, path);
  int code = http_get(url, resp, rl, timeout_ms);
  atomic_store(&p->server, code == 200);
  return code == 200;
}

static int press(struct power *p, double seconds)
{
  char path[64], resp[512];
  snprintf(path, sizeof path, "/press?seconds=%g", seconds);
  int ok = call(p, path, (int)(seconds * 1000) + 30000, resp, sizeof resp);
  if (!ok) say(p, "fingerbot server at %s did not answer /press (is fingerbot.py serve running?)", p->url);
  return ok;
}

// Picture state from the capture card: 1 = drawn screen, 0 = no picture.
static int picture(struct power *p)
{
  struct frame_shot s;
  if (frame_grab(p->cap, &s, 800) != 0) return 0;  // card stopped delivering: no signal
  return !frame_flat(&s);
}

static void sleep_ms_q(struct power *p, int ms)
{
  for (int t = 0; t < ms && !atomic_load(&p->quit); t += 50) usleep(50000);
}

static int verify_off(struct power *p)
{
  uint32_t end = SDL_GetTicks() + OFF_VERIFY_S * 1000;
  int dark = 0;
  while ((int32_t)(end - SDL_GetTicks()) > 0 && !atomic_load(&p->quit)) {
    dark = picture(p) ? 0 : dark + 1;
    if (dark >= 3) return 1;  // ~1.5 s of no picture in a row
    sleep_ms_q(p, 500);
  }
  return 0;
}

static int wait_picture(struct power *p, int seconds)
{
  uint32_t end = SDL_GetTicks() + (uint32_t)seconds * 1000;
  while ((int32_t)(end - SDL_GetTicks()) > 0 && !atomic_load(&p->quit)) {
    if (picture(p)) return 1;
    sleep_ms_q(p, 1000);
  }
  return 0;
}

static void travel(struct power *p)
{
  const char *t = getenv("SV_FINGERBOT_TRAVEL");  // e.g. 100 after reseating the arm
  if (t && *t) {
    char path[64], resp[256];
    snprintf(path, sizeof path, "/travel?pct=%d", atoi(t));
    call(p, path, 15000, resp, sizeof resp);
  }
}

static int force_off(struct power *p)
{
  char resp[256];
  travel(p);
  call(p, "/up", 30000, resp, sizeof resp);
  sleep_ms_q(p, 800);
  say(p, "holding power %d s (force off)", OFF_S);
  if (!press(p, OFF_S)) return 0;
  say(p, "checking the picture went away");
  if (!verify_off(p)) {
    say(p, "console still shows a picture: the hold missed the button (reseat the arm or raise SV_FINGERBOT_TRAVEL)");
    return 0;
  }
  say(p, "console is off");
  return 1;
}

static void *run(void *arg)
{
  struct power *p = arg;
  char action[32];
  pthread_mutex_lock(&p->lock);
  snprintf(action, sizeof action, "%s", p->action);
  pthread_mutex_unlock(&p->lock);

  if (!strcmp(action, "toggle")) {
    int before = picture(p);
    say(p, "%s: short press", before ? "sleep" : "wake");
    if (press(p, TOGGLE_S)) {
      if (!before) say(p, wait_picture(p, 20) ? "awake (picture up)" : "no picture after the wake press; try Boot");
      else say(p, "pressed (console should be asleep)");
    }
  } else if (!strcmp(action, "boot")) {
    travel(p);
    say(p, "boot press %d s", BOOT_S);
    if (press(p, BOOT_S)) {
      say(p, "waiting for a picture (HDMI takes ~40 s)");
      say(p, wait_picture(p, PICTURE_WAIT_S) ? "console is up" : "no picture yet after the boot press");
    }
  } else if (!strcmp(action, "menu")) {
    // Hold to raise the HOME Power menu, then leave the dialog up for a script
    // (or the user) to pick Restart. The exact hold that shows the menu (vs a
    // short press that sleeps) is console-specific; tune with SV_MENU_S.
    const char *e = getenv("SV_MENU_S");
    double menu_s = e && atof(e) > 0 ? atof(e) : 2.0;
    travel(p);
    say(p, "menu press %.1f s", menu_s);
    if (press(p, menu_s)) say(p, "power menu should be up (pick Restart)");
  } else if (!strcmp(action, "off")) {
    force_off(p);
  } else if (!strcmp(action, "restart")) {
    if (force_off(p)) {
      say(p, "letting the power-off finish (%d s)", OFF_SETTLE_MS / 1000);
      sleep_ms_q(p, OFF_SETTLE_MS);
      say(p, "boot press %d s", BOOT_S);
      if (press(p, BOOT_S)) {
        say(p, "waiting for a picture (HDMI takes ~40 s)");
        if (wait_picture(p, PICTURE_WAIT_S)) say(p, "restart done: console is up");
        else {
          say(p, "no picture after 75 s; pressing boot once more");
          if (press(p, BOOT_S) && wait_picture(p, 45)) say(p, "restart done: console is up");
          else say(p, "console never produced a picture after the restart");
        }
      }
    }
  }
  atomic_store(&p->busy, 0);
  return NULL;
}

struct power *power_create(const char *url, struct capture *cap, power_log_fn log, void *ud)
{
  struct power *p = calloc(1, sizeof *p);
  snprintf(p->url, sizeof p->url, "%s", url);
  size_t n = strlen(p->url);
  while (n && p->url[n - 1] == '/') p->url[--n] = 0;
  p->cap = cap;
  p->log = log;
  p->ud = ud;
  atomic_store(&p->server, -1);
  pthread_mutex_init(&p->lock, NULL);
  snprintf(p->status, sizeof p->status, "idle");
  return p;
}

void power_destroy(struct power *p)
{
  if (!p) return;
  atomic_store(&p->quit, 1);
  // A running press is on the fingerbot server's clock; don't hang on exit.
  for (int i = 0; i < 40 && atomic_load(&p->busy); i++) usleep(50000);
  if (!atomic_load(&p->busy)) {
    pthread_mutex_destroy(&p->lock);
    free(p);
  }
}

int power_start(struct power *p, const char *action)
{
  if (strcmp(action, "toggle") && strcmp(action, "boot") && strcmp(action, "off") &&
      strcmp(action, "restart") && strcmp(action, "menu"))
    return -1;
  int expected = 0;
  if (!atomic_compare_exchange_strong(&p->busy, &expected, 1)) return -1;
  pthread_mutex_lock(&p->lock);
  snprintf(p->action, sizeof p->action, "%s", action);
  pthread_mutex_unlock(&p->lock);
  if (pthread_create(&p->th, NULL, run, p) != 0) { atomic_store(&p->busy, 0); return -1; }
  pthread_detach(p->th);
  return 0;
}

int power_busy(struct power *p) { return atomic_load(&p->busy); }

void power_status(struct power *p, char *buf, size_t len)
{
  pthread_mutex_lock(&p->lock);
  snprintf(buf, len, "%s", p->status);
  pthread_mutex_unlock(&p->lock);
}

int power_server_up(struct power *p) { return atomic_load(&p->server); }
const char *power_url(struct power *p) { return p->url; }
