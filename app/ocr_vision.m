// macOS OCR backend: Apple Vision (VNRecognizeTextRequest) run in-process on
// the newest capture frame. Same API as app/ocr.c (Tesseract).
//
// Why: the old harness captured a PNG with ffmpeg, spawned a Vision CLI on it
// and parsed its output - 2-4 s per look. Here the frame is already in memory
// and Vision's fast path reads a 1080p frame in well under 100 ms on Apple
// silicon, so a navigation step costs a button press plus one read.
// SV_OCR_ACCURATE=1 switches to Vision's accurate model (slower, better on
// dense text such as a SmileBASIC listing).
//
// Built without ARC like capture_macos.m: objects are released explicitly.

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <SDL2/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ocr.h"

#define TEXT_MAX 8192

struct ocr {
  struct capture *cap;
  pthread_t th;
  atomic_int running, enabled;
  atomic_uint until_ms;

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

struct line {
  float x, y, w, h;  // top-left origin, 0..1 of the full frame
  char text[512];
};

static int cmp_lines(const void *a, const void *b)
{
  const struct line *p = a, *q = b;
  // Same row (vertical overlap) reads left to right; otherwise top to bottom.
  float pc = p->y + p->h / 2, qc = q->y + q->h / 2;
  if (fabsf(pc - qc) < (p->h < q->h ? p->h : q->h) * 0.5f) return p->x < q->x ? -1 : p->x > q->x;
  return pc < qc ? -1 : 1;
}

static void *ocr_thread(void *arg)
{
  struct ocr *o = arg;
  unsigned w = capture_width(o->cap), h = capture_height(o->cap);
  uint8_t *frame = malloc((size_t)w * h);
  unsigned last_seq = 0;
  static struct line lines[OCR_MAX_BOXES];
  static struct ocr_box boxes[OCR_MAX_BOXES];
  static char joined[TEXT_MAX], norm[TEXT_MAX];
  int accurate = getenv("SV_OCR_ACCURATE") && atoi(getenv("SV_OCR_ACCURATE")) > 0;
  CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();

  while (atomic_load(&o->running)) {
    if (!atomic_load(&o->enabled) && (int32_t)(atomic_load(&o->until_ms) - SDL_GetTicks()) <= 0) {
      usleep(20000);
      continue;
    }
    uint32_t started = SDL_GetTicks();
    atomic_store(&o->busy_since, started);
    atomic_store(&o->busy, 1);
    capture_want_luma(o->cap);
    unsigned seq = 0;
    for (int i = 0; i < 100 && atomic_load(&o->running); i++) {
      seq = capture_luma_seq(o->cap);
      if (seq && seq != last_seq) break;
      usleep(3000);
    }
    if (!seq || seq == last_seq) { atomic_store(&o->busy, 0); continue; }
    // The size can change after start-up (the network source begins with a
    // placeholder and switches to the stream's size), so check every pass.
    unsigned cw = capture_width(o->cap), ch = capture_height(o->cap);
    if (cw != w || ch != h) {
      uint8_t *nf = realloc(frame, (size_t)cw * ch);
      if (!nf) { atomic_store(&o->busy, 0); usleep(20000); continue; }
      frame = nf;
      w = cw;
      h = ch;
    }
    seq = capture_copy_luma(o->cap, frame);
    last_seq = seq;

    pthread_mutex_lock(&o->lock);
    float rx = o->rx, ry = o->ry, rw = o->rw, rh = o->rh;
    pthread_mutex_unlock(&o->lock);
    int roi = rw > 0 && rh > 0;

    int nl = 0;
    @autoreleasepool {
      CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, frame, (size_t)w * h, NULL);
      CGImageRef img = CGImageCreate(w, h, 8, 8, w, gray, (CGBitmapInfo)kCGImageAlphaNone, dp, NULL,
                                     false, kCGRenderingIntentDefault);
      VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
      req.recognitionLevel = accurate ? VNRequestTextRecognitionLevelAccurate
                                      : VNRequestTextRecognitionLevelFast;
      req.usesLanguageCorrection = NO;
      // Vision's ROI origin is bottom-left.
      if (roi) req.regionOfInterest = CGRectMake(rx, 1.0 - ry - rh, rw, rh);
      VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
      NSError *err = nil;
      if ([handler performRequests:@[ req ] error:&err]) {
        for (VNRecognizedTextObservation *ob in req.results) {
          if (nl >= OCR_MAX_BOXES) break;
          VNRecognizedText *cand = [[ob topCandidates:1] firstObject];
          if (!cand || cand.confidence < 0.3f) continue;
          CGRect b = ob.boundingBox;  // normalised to the ROI, bottom-left origin
          if (roi) b = CGRectMake(rx + b.origin.x * rw, (1.0 - ry - rh) + b.origin.y * rh,
                                  b.size.width * rw, b.size.height * rh);
          struct line *L = &lines[nl++];
          L->x = (float)b.origin.x;
          L->y = (float)(1.0 - b.origin.y - b.size.height);
          L->w = (float)b.size.width;
          L->h = (float)b.size.height;
          snprintf(L->text, sizeof L->text, "%s", cand.string.UTF8String ?: "");
        }
      } else if (err) {
        fprintf(stderr, "ocr: vision: %s\n", err.localizedDescription.UTF8String);
      }
      [handler release];
      [req release];
      CGImageRelease(img);
      CGDataProviderRelease(dp);
    }

    qsort(lines, (size_t)nl, sizeof lines[0], cmp_lines);
    size_t jn = 0;
    joined[0] = 0;
    int nb = 0;
    for (int i = 0; i < nl; i++) {
      jn += (size_t)snprintf(joined + jn, jn < sizeof joined ? sizeof joined - jn : 0, "%s\n", lines[i].text);
      if (jn >= sizeof joined) jn = sizeof joined - 1;
      // Word boxes: split the line, widths proportional to character count
      // (Vision fast mode reports lines; this is close enough for overlays
      // and band queries).
      size_t len = strlen(lines[i].text);
      if (!len) continue;
      const char *t = lines[i].text;
      size_t k = 0;
      while (k < len && nb < OCR_MAX_BOXES) {
        while (k < len && t[k] == ' ') k++;
        size_t s = k;
        while (k < len && t[k] != ' ') k++;
        if (k == s) break;
        struct ocr_box *bx = &boxes[nb++];
        float x0 = lines[i].x + lines[i].w * (float)s / len, x1 = lines[i].x + lines[i].w * (float)k / len;
        bx->x = (int)(x0 * w);
        bx->y = (int)(lines[i].y * h);
        bx->w = (int)((x1 - x0) * w);
        bx->h = (int)(lines[i].h * h);
        char word[64];
        size_t wl = k - s < sizeof word - 1 ? k - s : sizeof word - 1;
        memcpy(word, t + s, wl);
        word[wl] = 0;
        normalize(word, bx->word, sizeof bx->word);
      }
    }
    normalize(joined, norm, sizeof norm);

    pthread_mutex_lock(&o->lock);
    memcpy(o->text, norm, sizeof o->text);
    memcpy(o->boxes, boxes, sizeof(boxes[0]) * (size_t)nb);
    o->nboxes = nb;
    o->seq++;
    o->started_ms = started;
    pthread_mutex_unlock(&o->lock);
    atomic_store(&o->busy, 0);
  }
  CGColorSpaceRelease(gray);
  free(frame);
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
const char *ocr_engine(void) { return "vision"; }

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
