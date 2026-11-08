#include "ocr.h"

#include <SDL2/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tesseract/capi.h>
#include <unistd.h>

#define TEXT_MAX 8192

struct ocr {
  struct capture *cap;
  pthread_t th;
  atomic_int running, enabled;
  atomic_uint until_ms;  // ocr_request deadline

  pthread_mutex_t lock;  // guards region and result
  float rx, ry, rw, rh;
  char text[TEXT_MAX];
  unsigned seq;
  uint32_t started_ms;
  struct ocr_box boxes[OCR_MAX_BOXES];
  int nboxes;
  atomic_int busy;
  atomic_uint busy_since;
};

// Lower-case and collapse whitespace so rules can match loosely.
static void normalize(const char *in, char *out, size_t len)
{
  size_t n = 0;
  int space = 1;
  for (; *in && n + 1 < len; in++) {
    unsigned char ch = (unsigned char)*in;
    if (isspace(ch)) {
      if (!space) { out[n++] = ' '; space = 1; }
    } else {
      out[n++] = (char)tolower(ch);
      space = 0;
    }
  }
  if (n && out[n - 1] == ' ') n--;
  out[n] = 0;
}

static void *ocr_thread(void *arg)
{
  struct ocr *o = arg;
  TessBaseAPI *api = TessBaseAPICreate();
  if (TessBaseAPIInit3(api, NULL, "eng") != 0) {
    fprintf(stderr, "ocr: tesseract init failed (is tesseract-ocr-eng installed?)\n");
    TessBaseAPIDelete(api);
    return NULL;
  }
  // Sparse text: game/console UIs are scattered labels, not paragraphs.
  TessBaseAPISetPageSegMode(api, PSM_SPARSE_TEXT);
  TessBaseAPISetVariable(api, "user_defined_dpi", "150");

  unsigned w = capture_width(o->cap), h = capture_height(o->cap);
  uint8_t *frame = malloc((size_t)w * h);
  unsigned last_seq = 0;
  char raw_norm[TEXT_MAX];
  static struct ocr_box boxes[OCR_MAX_BOXES];

  while (atomic_load(&o->running)) {
    if (!atomic_load(&o->enabled) && (int32_t)(atomic_load(&o->until_ms) - SDL_GetTicks()) <= 0) {
      usleep(20000);
      continue;
    }

    uint32_t started = SDL_GetTicks();
    atomic_store(&o->busy_since, started);
    atomic_store(&o->busy, 1);
    capture_want_luma(o->cap);
    // Wait (briefly) for the main loop to hand over the next frame.
    unsigned seq = 0;
    for (int i = 0; i < 100 && atomic_load(&o->running); i++) {
      seq = capture_copy_luma(o->cap, frame);
      if (seq && seq != last_seq) break;
      usleep(5000);
    }
    if (!seq || seq == last_seq) { atomic_store(&o->busy, 0); continue; }
    last_seq = seq;

    pthread_mutex_lock(&o->lock);
    float rx = o->rx, ry = o->ry, rw = o->rw, rh = o->rh;
    pthread_mutex_unlock(&o->lock);

    TessBaseAPISetImage(api, frame, (int)w, (int)h, 1, (int)w);
    if (rw > 0 && rh > 0)
      TessBaseAPISetRectangle(api, (int)(rx * w), (int)(ry * h), (int)(rw * w), (int)(rh * h));
    char *raw = TessBaseAPIGetUTF8Text(api);
    normalize(raw ? raw : "", raw_norm, sizeof raw_norm);
    if (raw) TessDeleteText(raw);

    // Word boxes for the on-screen overlay; low-confidence noise is dropped.
    int nb = 0;
    TessResultIterator *it = TessBaseAPIGetIterator(api);
    if (it) {
      TessPageIterator *pit = TessResultIteratorGetPageIterator(it);
      do {
        if (nb >= OCR_MAX_BOXES) break;
        char *word = TessResultIteratorGetUTF8Text(it, RIL_WORD);
        float conf = TessResultIteratorConfidence(it, RIL_WORD);
        int l, t, r, b;
        // Keep only confident, word-shaped hits: OCR on game art produces
        // plenty of junk, so demand real letters and a sane box.
        int alnum = 0;
        for (const char *c = word ? word : ""; *c; c++) if (isalnum((unsigned char)*c)) alnum++;
        if (word && conf >= 72 && alnum >= 2 &&
            TessPageIteratorBoundingBox(pit, RIL_WORD, &l, &t, &r, &b) &&
            r > l && b > t && (b - t) >= 10 && (b - t) <= (int)h / 6 &&
            (r - l) >= (b - t) / 2 && (r - l) <= (int)w / 2) {
          struct ocr_box *bx = &boxes[nb++];
          bx->x = l; bx->y = t; bx->w = r - l; bx->h = b - t;
          normalize(word, bx->word, sizeof bx->word);
        }
        if (word) TessDeleteText(word);
      } while (TessPageIteratorNext(pit, RIL_WORD));
      TessResultIteratorDelete(it);
    }
    TessBaseAPIClear(api);

    pthread_mutex_lock(&o->lock);
    memcpy(o->text, raw_norm, sizeof o->text);
    memcpy(o->boxes, boxes, sizeof(boxes[0]) * (size_t)nb);
    o->nboxes = nb;
    o->seq++;
    o->started_ms = started;
    pthread_mutex_unlock(&o->lock);
    atomic_store(&o->busy, 0);
  }

  free(frame);
  TessBaseAPIEnd(api);
  TessBaseAPIDelete(api);
  return NULL;
}

struct ocr *ocr_create(struct capture *cap)
{
  struct ocr *o = calloc(1, sizeof *o);
  o->cap = cap;
  pthread_mutex_init(&o->lock, NULL);
  atomic_store(&o->running, 1);
  if (pthread_create(&o->th, NULL, ocr_thread, o) != 0) {
    pthread_mutex_destroy(&o->lock);
    free(o);
    return NULL;
  }
  return o;
}

void ocr_destroy(struct ocr *o)
{
  if (!o) return;
  atomic_store(&o->running, 0);
  pthread_join(o->th, NULL);
  pthread_mutex_destroy(&o->lock);
  free(o);
}

void ocr_set_enabled(struct ocr *o, int on) { atomic_store(&o->enabled, on); }
void ocr_request(struct ocr *o, unsigned ms) { atomic_store(&o->until_ms, SDL_GetTicks() + ms); }
const char *ocr_engine(void) { return "tesseract"; }

void ocr_set_region(struct ocr *o, float x, float y, float w, float h)
{
  pthread_mutex_lock(&o->lock);
  o->rx = x; o->ry = y; o->rw = w; o->rh = h;
  pthread_mutex_unlock(&o->lock);
}

unsigned ocr_latest(struct ocr *o, char *buf, size_t len, uint32_t *started_ms)
{
  pthread_mutex_lock(&o->lock);
  unsigned seq = o->seq;
  if (buf && len) snprintf(buf, len, "%s", o->text);
  if (started_ms) *started_ms = o->started_ms;
  pthread_mutex_unlock(&o->lock);
  return seq;
}

unsigned ocr_boxes(struct ocr *o, struct ocr_box *out, int max, int *n)
{
  pthread_mutex_lock(&o->lock);
  int k = o->nboxes < max ? o->nboxes : max;
  memcpy(out, o->boxes, sizeof(out[0]) * (size_t)k);
  unsigned seq = o->seq;
  pthread_mutex_unlock(&o->lock);
  *n = k;
  return seq;
}

int ocr_busy(struct ocr *o, uint32_t *since_ms)
{
  if (since_ms) *since_ms = atomic_load(&o->busy_since);
  return atomic_load(&o->busy);
}

int ocr_enabled(struct ocr *o) { return atomic_load(&o->enabled); }
