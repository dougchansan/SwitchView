#include "frame.h"

#include <SDL2/SDL.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FLAT_SD 5.0f      // a drawn screen has far more contrast than this
#define LIB_MAX 256

int frame_grab(struct capture *cap, struct frame_shot *out, int timeout_ms)
{
  unsigned w = capture_width(cap), h = capture_height(cap);
  if (!w || !h) return -1;
  unsigned before = capture_luma_seq(cap);
  capture_want_luma(cap);
  uint32_t end = SDL_GetTicks() + (uint32_t)timeout_ms;
  while (capture_luma_seq(cap) == before) {
    if ((int32_t)(end - SDL_GetTicks()) <= 0) return -1;
    capture_want_luma(cap);
    usleep(2000);
  }
  uint8_t *luma = malloc((size_t)w * h);
  if (!luma) return -1;
  out->seq = capture_copy_luma(cap, luma);

  // Box-average into FT_W x FT_H cells, sampling every other pixel.
  double sum = 0, sum2 = 0;
  unsigned n = 0;
  for (int cy = 0; cy < FT_H; cy++) {
    unsigned y0 = cy * h / FT_H, y1 = (cy + 1) * h / FT_H;
    for (int cx = 0; cx < FT_W; cx++) {
      unsigned x0 = cx * w / FT_W, x1 = (cx + 1) * w / FT_W;
      unsigned acc = 0, k = 0;
      for (unsigned y = y0; y < y1; y += 2)
        for (unsigned x = x0; x < x1; x += 2) {
          unsigned v = luma[(size_t)y * w + x];
          acc += v;
          sum += v;
          sum2 += (double)v * v;
          k++;
        }
      n += k;
      out->px[cy * FT_W + cx] = (uint8_t)(k ? acc / k : 0);
    }
  }
  free(luma);
  out->mean = n ? (float)(sum / n) : 0;
  double var = n ? sum2 / n - (sum / n) * (sum / n) : 0;
  out->sd = var > 0 ? (float)sqrt(var) : 0;
  return 0;
}

float frame_dist(const struct frame_shot *a, const struct frame_shot *b)
{
  unsigned acc = 0;
  for (int i = 0; i < FT_W * FT_H; i++) acc += (unsigned)abs((int)a->px[i] - (int)b->px[i]);
  return (float)acc / (FT_W * FT_H);
}

int frame_flat(const struct frame_shot *s) { return s->sd < FLAT_SD; }

/* ---------------- screen library ---------------- */

struct entry {
  char label[64];
  uint8_t px[FT_W * FT_H];
};

struct frame_lib {
  char dir[1024];
  pthread_mutex_t lock;
  struct entry e[LIB_MAX];
  int n;
};

static void base_label(const char *name, char *out, size_t len)
{
  snprintf(out, len, "%s", name);
  char *dot = strrchr(out, '.');
  if (dot) *dot = 0;
  char *hash = strchr(out, '#');
  if (hash) *hash = 0;
}

static void lib_load(struct frame_lib *lib)
{
  lib->n = 0;
  DIR *d = opendir(lib->dir);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d)) && lib->n < LIB_MAX) {
    size_t ln = strlen(de->d_name);
    if (ln < 4 || strcmp(de->d_name + ln - 3, ".sv")) continue;
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", lib->dir, de->d_name);
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    struct entry *e = &lib->e[lib->n];
    if (fread(e->px, 1, sizeof e->px, f) == sizeof e->px) {
      base_label(de->d_name, e->label, sizeof e->label);
      lib->n++;
    }
    fclose(f);
  }
  closedir(d);
}

struct frame_lib *frame_lib_open(const char *dir)
{
  struct frame_lib *lib = calloc(1, sizeof *lib);
  snprintf(lib->dir, sizeof lib->dir, "%s", dir);
  pthread_mutex_init(&lib->lock, NULL);
  lib_load(lib);
  return lib;
}

void frame_lib_close(struct frame_lib *lib)
{
  if (!lib) return;
  pthread_mutex_destroy(&lib->lock);
  free(lib);
}

static int safe_label(const char *label)
{
  if (!*label || strlen(label) > 48) return 0;
  for (const char *c = label; *c; c++)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
          *c == '-' || *c == '_'))
      return 0;
  return 1;
}

int frame_lib_learn(struct frame_lib *lib, const char *label, const struct frame_shot *s)
{
  if (!safe_label(label)) return -1;
  mkdir(lib->dir, 0755);
  pthread_mutex_lock(&lib->lock);
  char path[1400];
  int k = 0;
  for (;; k++) {  // keep every sample: label.sv, label#1.sv, ...
    if (k) snprintf(path, sizeof path, "%s/%s#%d.sv", lib->dir, label, k);
    else snprintf(path, sizeof path, "%s/%s.sv", lib->dir, label);
    if (access(path, F_OK) != 0) break;
  }
  FILE *f = fopen(path, "wb");
  int ok = f && fwrite(s->px, 1, sizeof s->px, f) == sizeof s->px;
  if (f) fclose(f);
  if (ok && lib->n < LIB_MAX) {
    snprintf(lib->e[lib->n].label, sizeof lib->e[0].label, "%s", label);
    memcpy(lib->e[lib->n].px, s->px, sizeof s->px);
    lib->n++;
  }
  pthread_mutex_unlock(&lib->lock);
  return ok ? 0 : -1;
}

int frame_lib_forget(struct frame_lib *lib, const char *label)
{
  if (!safe_label(label)) return -1;
  pthread_mutex_lock(&lib->lock);
  DIR *d = opendir(lib->dir);
  int removed = 0;
  if (d) {
    struct dirent *de;
    while ((de = readdir(d))) {
      char base[64];
      size_t ln = strlen(de->d_name);
      if (ln < 4 || strcmp(de->d_name + ln - 3, ".sv")) continue;
      base_label(de->d_name, base, sizeof base);
      if (strcmp(base, label)) continue;
      char path[1400];
      snprintf(path, sizeof path, "%s/%s", lib->dir, de->d_name);
      if (unlink(path) == 0) removed++;
    }
    closedir(d);
  }
  lib_load(lib);
  pthread_mutex_unlock(&lib->lock);
  return removed;
}

float frame_lib_match(struct frame_lib *lib, const struct frame_shot *s, char *buf, size_t len)
{
  float best = 999;
  if (buf && len) buf[0] = 0;
  pthread_mutex_lock(&lib->lock);
  for (int i = 0; i < lib->n; i++) {
    struct frame_shot ref;
    memcpy(ref.px, lib->e[i].px, sizeof ref.px);
    float d = frame_dist(s, &ref);
    if (d < best) {
      best = d;
      if (buf && len) snprintf(buf, len, "%s", lib->e[i].label);
    }
  }
  pthread_mutex_unlock(&lib->lock);
  return best;
}

int frame_lib_count(struct frame_lib *lib)
{
  pthread_mutex_lock(&lib->lock);
  int n = lib->n;
  pthread_mutex_unlock(&lib->lock);
  return n;
}
