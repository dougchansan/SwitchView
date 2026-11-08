// macOS capture backend: AVFoundation for video (newest-frame-wins, NV12
// straight off the capture card so the GPU does the colour conversion) and
// SDL's CoreAudio capture for sound. Mirrors the Linux V4L2/ALSA backend
// behind app/capture.h.
//
// Build without ARC: the capture struct owns its Objective-C objects
// explicitly, which keeps the plain-C ABI of the header intact.

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "capture_impl.h"

#define AUDIO_RATE 48000
#define AUDIO_BYTES_PER_MS (AUDIO_RATE * 4 / 1000)

struct lcap;

@interface SVFrameSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) struct lcap *cap;
@end

struct lcap {
  struct capture base;  // first: see capture_impl.h
  // video
  unsigned width, height;
  char video_name[128];
  SDL_Texture *tex;
  Uint32 tex_format;  // SDL_PIXELFORMAT_NV12, or ARGB8888 for the BGRA fallback
  AVCaptureSession *session;
  AVCaptureVideoDataOutput *output;
  SVFrameSink *sink;
  dispatch_queue_t queue;
  // newest frame, handed over by the capture queue
  pthread_mutex_t frame_lock;
  pthread_cond_t frame_cond;
  CVPixelBufferRef latest;  // retained
  unsigned long seq, taken;
  // audio (SDL capture device; CoreAudio underneath)
  int have_audio;
  SDL_AudioDeviceID in, out;
  unsigned max_bytes;
  pthread_t ath;
  atomic_int audio_running;
  atomic_int muted;
  // OCR snapshot
  atomic_int luma_wanted;
  pthread_mutex_t luma_lock;
  uint8_t *luma;
  unsigned luma_seq;
};

@implementation SVFrameSink
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
  (void)output;
  (void)connection;
  struct lcap *c = self.cap;
  if (!c) return;
  CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!pb) return;
  CVPixelBufferRetain(pb);
  pthread_mutex_lock(&c->frame_lock);
  CVPixelBufferRef old = c->latest;
  c->latest = pb;
  c->seq++;
  pthread_cond_broadcast(&c->frame_cond);
  pthread_mutex_unlock(&c->frame_lock);
  if (old) CVPixelBufferRelease(old);
}
@end

/* ---------------- devices ---------------- */

// An iPhone/iPad (Continuity Camera, Desk View) shows up as an "external"
// camera whenever it is nearby; it is never the capture card.
static int is_phone(AVCaptureDevice *d)
{
  if (@available(macOS 14.0, *))
    if ([d.deviceType isEqualToString:AVCaptureDeviceTypeContinuityCamera]) return 1;
  const char *n = d.localizedName.UTF8String ?: "";
  return strcasestr(n, "iphone") || strcasestr(n, "ipad") || strcasestr(n, "desk view");
}

// Capture cards only. Set SV_ALLOW_ANY_CAMERA=1 to smoke-test with a webcam.
static NSArray *video_devices(void)
{
  AVCaptureDeviceDiscoverySession *ds = [AVCaptureDeviceDiscoverySession
      discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeExternal ]
                            mediaType:AVMediaTypeVideo
                             position:AVCaptureDevicePositionUnspecified];
  NSMutableArray *devs = [NSMutableArray array];
  for (AVCaptureDevice *d in ds.devices)
    if (!is_phone(d)) [devs addObject:d];
  const char *any = getenv("SV_ALLOW_ANY_CAMERA");
  if (devs.count == 0 && any && atoi(any) > 0) {
    AVCaptureDevice *d = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (d) [devs addObject:d];
  }
  return devs;
}

// Picks a device from the -d argument: an index, part of the name or unique
// ID, or the V4L2 default spelling which means "the first capture card".
static AVCaptureDevice *pick_device(const char *want)
{
  NSArray *devs = video_devices();
  if (devs.count == 0) {
    fprintf(stderr, "video: no capture card found (is it plugged in? phones are ignored)\n");
    return nil;
  }
  int idx = -1;
  if (want && *want && strncmp(want, "/dev/video", 10) != 0) {
    char *end = NULL;
    long n = strtol(want, &end, 10);
    if (end && *end == 0) idx = (int)n;
  }
  if (idx >= 0) {
    if ((NSUInteger)idx >= devs.count) {
      fprintf(stderr, "video: device %d does not exist (use -L to list)\n", idx);
      return nil;
    }
    return devs[(NSUInteger)idx];
  }
  if (want && *want && strncmp(want, "/dev/video", 10) != 0) {
    for (AVCaptureDevice *d in devs) {
      if (strcasestr(d.localizedName.UTF8String, want) || strcasestr(d.uniqueID.UTF8String, want))
        return d;
    }
    fprintf(stderr, "video: no device matches '%s' (use -L to list)\n", want);
    return nil;
  }
  // Prefer something that names itself like a capture card.
  static const char *const hints[] = {"usb3.0 video", "elgato", "cam link", "capture", "hdmi", "uvc"};
  for (size_t h = 0; h < sizeof hints / sizeof hints[0]; h++)
    for (AVCaptureDevice *d in devs)
      if (strcasestr(d.localizedName.UTF8String ?: "", hints[h])) return d;
  return devs[0];
}

void capture_list_devices(void)
{
  NSArray *devs = video_devices();
  if (devs.count == 0) fprintf(stderr, "video: no capture devices found\n");
  NSUInteger i = 0;
  for (AVCaptureDevice *d in devs) {
    CMVideoDimensions dim =
        CMVideoFormatDescriptionGetDimensions(d.activeFormat.formatDescription);
    fprintf(stderr, "video: %lu %s (%s) %dx%d\n", (unsigned long)i, d.localizedName.UTF8String,
            d.uniqueID.UTF8String, (int)dim.width, (int)dim.height);
    i++;
  }

  if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
  int n = SDL_GetNumAudioDevices(1);
  if (n <= 0) fprintf(stderr, "audio: no capture devices found (%s)\n", SDL_GetError());
  for (int k = 0; k < n; k++)
    fprintf(stderr, "audio: %d %s\n", k, SDL_GetAudioDeviceName(k, 1));
}

/* ---------------- audio ---------------- */

static void *audio_thread(void *p)
{
  struct lcap *c = p;
  int16_t buf[256 * 2];  // ~5 ms at 48 kHz: keeps the output queue short
  while (atomic_load(&c->audio_running)) {
    if (SDL_GetQueuedAudioSize(c->in) < sizeof buf) { SDL_Delay(1); continue; }
    Uint32 n = SDL_DequeueAudio(c->in, buf, sizeof buf);
    if (!n) { SDL_Delay(1); continue; }
    if (atomic_load(&c->muted)) continue;
    // Drop input rather than let the output queue (and latency) grow.
    if (SDL_GetQueuedAudioSize(c->out) > c->max_bytes) continue;
    SDL_QueueAudio(c->out, buf, n);
  }
  return NULL;
}

// -a is an SDL capture device name (substring) on macOS; the ALSA default
// spelling means "the default input".
static int audio_start(struct lcap *c, const struct capture_opts *o)
{
  const char *want = o->audio_dev;
  if (want && (strncmp(want, "hw:CARD", 7) == 0 || strcmp(want, "default") == 0)) want = NULL;

  const char *pick = NULL;
  char name[256];
  if (!want || !*want) {
    // Prefer the input that belongs to the capture card: "USB3.0 Video" and
    // "USB3.0 Audio" share a prefix, as do the iPhone camera and microphone.
    int n = SDL_GetNumAudioDevices(1), best_len = 4;
    for (int i = 0; i < n; i++) {
      const char *dev = SDL_GetAudioDeviceName(i, 1);
      if (!dev) continue;
      int k = 0;
      while (dev[k] && c->video_name[k] && dev[k] == c->video_name[k]) k++;
      if (k > best_len) {
        best_len = k;
        snprintf(name, sizeof name, "%s", dev);
        pick = name;
      }
    }
  } else {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
  int n = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < n; i++) {
      const char *dev = SDL_GetAudioDeviceName(i, 1);
      if (dev && strcasestr(dev, want)) {
        snprintf(name, sizeof name, "%s", dev);
        pick = name;
        break;
      }
    }
    if (!pick) fprintf(stderr, "audio: no capture device matches '%s'; using the default\n", want);
  }

  SDL_AudioSpec want_spec = {0}, have;
  want_spec.freq = AUDIO_RATE;
  want_spec.format = AUDIO_S16LSB;
  want_spec.channels = 2;
  want_spec.samples = 256;
  want_spec.callback = NULL;  // pull with SDL_DequeueAudio
  c->in = SDL_OpenAudioDevice(pick, 1, &want_spec, &have, 0);
  if (!c->in) {
    fprintf(stderr, "audio: SDL capture: %s\n", SDL_GetError());
    return -1;
  }

  SDL_AudioSpec out_want = {0}, out_have;
  out_want.freq = have.freq;
  out_want.format = AUDIO_S16LSB;
  out_want.channels = have.channels;
  out_want.samples = 128;
  c->out = SDL_OpenAudioDevice(NULL, 0, &out_want, &out_have, 0);
  if (!c->out) {
    fprintf(stderr, "audio: SDL output: %s\n", SDL_GetError());
    SDL_CloseAudioDevice(c->in);
    return -1;
  }
  c->max_bytes = (unsigned)have.freq * 4 * (unsigned)o->audio_max_ms / 1000;
  fprintf(stderr, "audio: %s %d Hz, %d ch, output buffer %d samples\n",
          pick ? pick : "(default input)", have.freq, have.channels, out_have.samples);
  SDL_PauseAudioDevice(c->out, 0);
  SDL_PauseAudioDevice(c->in, 0);
  atomic_store(&c->audio_running, 1);
  if (pthread_create(&c->ath, NULL, audio_thread, c) != 0) {
    atomic_store(&c->audio_running, 0);
    SDL_CloseAudioDevice(c->out);
    SDL_CloseAudioDevice(c->in);
    return -1;
  }
  return 0;
}

/* ---------------- video ---------------- */

// Picks the format closest to what was asked for; size first, frame rate as
// the tie-breaker.
static AVCaptureDeviceFormat *pick_format(AVCaptureDevice *dev, int want_w, int want_h,
                                          int want_fps, double *out_fps)
{
  AVCaptureDeviceFormat *best = nil;
  long best_score = LONG_MAX;
  for (AVCaptureDeviceFormat *f in dev.formats) {
    CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
    double max_fps = 0;
    for (AVFrameRateRange *r in f.videoSupportedFrameRateRanges)
      if (r.maxFrameRate > max_fps) max_fps = r.maxFrameRate;
    long score = labs((long)d.width - want_w) + labs((long)d.height - want_h);
    score = score * 1000 + (max_fps + 0.5 >= want_fps ? 0 : 100);
    if (score < best_score) { best_score = score; best = f; }
  }
  if (!best) return nil;

  double max_fps = 0, min_fps = 0;
  for (AVFrameRateRange *r in best.videoSupportedFrameRateRanges) {
    if (r.maxFrameRate > max_fps) max_fps = r.maxFrameRate;
    if (min_fps == 0 || r.minFrameRate < min_fps) min_fps = r.minFrameRate;
  }
  double fps = want_fps;
  if (fps > max_fps) fps = max_fps;
  if (fps < min_fps) fps = min_fps;
  if (fps <= 0) fps = 30;
  *out_fps = fps;
  return best;
}

// Must run *after* the session has started: on macOS adding the input (and
// starting) re-applies the session preset, which would undo the format.
// A capture card exposes discrete rates (60.00 - 60.00, ...) whose frame
// durations live in the device's own timebase, so reuse the range's CMTime.
static void apply_format(AVCaptureDevice *dev, AVCaptureDeviceFormat *fmt, double fps)
{
  NSError *err = nil;
  if (![dev lockForConfiguration:&err]) {
    fprintf(stderr, "video: cannot configure device: %s\n", err.localizedDescription.UTF8String);
    return;
  }
  dev.activeFormat = fmt;
  CMTime dur = kCMTimeInvalid;
  for (AVFrameRateRange *r in fmt.videoSupportedFrameRateRanges) {
    if (fps + 0.01 < r.minFrameRate || fps - 0.01 > r.maxFrameRate) continue;
    dur = CMTimeCompare(r.minFrameDuration, r.maxFrameDuration) == 0
              ? r.minFrameDuration
              : CMTimeMakeWithSeconds(1.0 / fps, 1000000);
    break;
  }
  if (CMTIME_IS_VALID(dur)) {
    @try {
      dev.activeVideoMinFrameDuration = dur;
      dev.activeVideoMaxFrameDuration = dur;
    } @catch (NSException *e) {
      fprintf(stderr, "video: device kept its own frame rate (%s)\n", e.reason.UTF8String);
    }
  }
  [dev unlockForConfiguration];

  CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
  fprintf(stderr, "video: %s %dx%d @ %.0f fps\n", dev.localizedName.UTF8String, (int)d.width,
          (int)d.height, CMTimeGetSeconds(dev.activeVideoMinFrameDuration) > 0
                              ? 1.0 / CMTimeGetSeconds(dev.activeVideoMinFrameDuration)
                              : fps);
}

static int video_open(struct lcap *c, const struct capture_opts *o, SDL_Renderer *ren)
{
  AVCaptureDevice *dev = pick_device(o->video_dev);
  if (!dev) return -1;
  snprintf(c->video_name, sizeof c->video_name, "%s", dev.localizedName.UTF8String);

  if (o->format == SV_PIX_YUYV)
    fprintf(stderr, "video: macOS capture is always NV12; ignoring -p yuyv\n");

  // No session preset: on macOS the device's activeFormat is what counts.
  c->session = [[AVCaptureSession alloc] init];
  double fps = 30;
  AVCaptureDeviceFormat *fmt = pick_format(dev, o->width, o->height, o->fps, &fps);
  CMVideoDimensions dim = fmt ? CMVideoFormatDescriptionGetDimensions(fmt.formatDescription)
                              : CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription);
  c->width = (unsigned)dim.width;
  c->height = (unsigned)dim.height;

  NSError *err = nil;
  AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
  if (!input) {
    fprintf(stderr, "video: cannot open %s: %s\n", dev.localizedName.UTF8String,
            err.localizedDescription.UTF8String);
    return -1;
  }
  if (![c->session canAddInput:input]) {
    fprintf(stderr, "video: device cannot be used as an input\n");
    return -1;
  }
  [c->session addInput:input];

  c->output = [[AVCaptureVideoDataOutput alloc] init];
  c->output.alwaysDiscardsLateVideoFrames = YES;  // newest frame wins
  c->output.videoSettings = @{
    (NSString *)kCVPixelBufferPixelFormatTypeKey :
        @( kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ),
    (NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{}
  };
  if (![c->session canAddOutput:c->output]) {
    fprintf(stderr, "video: device cannot deliver frames\n");
    return -1;
  }
  [c->session addOutput:c->output];

  c->sink = [[SVFrameSink alloc] init];
  c->sink.cap = c;
  c->queue = dispatch_queue_create("switchview.capture", DISPATCH_QUEUE_SERIAL);
  [c->output setSampleBufferDelegate:c->sink queue:c->queue];

  c->tex_format = SDL_PIXELFORMAT_NV12;
  c->tex = SDL_CreateTexture(ren, c->tex_format, SDL_TEXTUREACCESS_STREAMING,
                             (int)c->width, (int)c->height);
  if (!c->tex) {
    // Some SDL builds (e.g. sdl2-compat on an old SDL3) cannot take NV12;
    // ask AVFoundation for BGRA and hand SDL a plain RGB texture instead.
    fprintf(stderr, "video: no NV12 texture (%s); falling back to BGRA\n", SDL_GetError());
    c->output.videoSettings = @{
      (NSString *)kCVPixelBufferPixelFormatTypeKey : @( kCVPixelFormatType_32BGRA )
    };
    c->tex_format = SDL_PIXELFORMAT_ARGB8888;
    c->tex = SDL_CreateTexture(ren, c->tex_format, SDL_TEXTUREACCESS_STREAMING,
                               (int)c->width, (int)c->height);
    if (!c->tex) {
      fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
      return -1;
    }
  }

  [c->session startRunning];
  if (fmt) apply_format(dev, fmt, fps);
  return 0;
}

/* ---------------- frame upload ---------------- */

static void snapshot_luma(struct lcap *c, const uint8_t *y, int stride)
{
  pthread_mutex_lock(&c->luma_lock);
  if (!c->luma) c->luma = malloc((size_t)c->width * c->height);
  for (unsigned row = 0; row < c->height; row++)
    memcpy(c->luma + (size_t)row * c->width, y + (size_t)row * stride, c->width);
  c->luma_seq++;
  pthread_mutex_unlock(&c->luma_lock);
}

static void upload_frame(struct lcap *c, CVPixelBufferRef pb)
{
  CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  if (c->tex_format == SDL_PIXELFORMAT_NV12) {
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    int ys = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    int us = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    if (y && uv) {
      if (atomic_exchange(&c->luma_wanted, 0)) snapshot_luma(c, y, ys);
      SDL_UpdateNVTexture(c->tex, NULL, y, ys, uv, us);
    }
  } else {
    const uint8_t *bgra = CVPixelBufferGetBaseAddress(pb);
    int stride = (int)CVPixelBufferGetBytesPerRow(pb);
    if (bgra) {
      SDL_UpdateTexture(c->tex, NULL, bgra, stride);
      if (atomic_exchange(&c->luma_wanted, 0)) {
        pthread_mutex_lock(&c->luma_lock);
        if (!c->luma) c->luma = malloc((size_t)c->width * c->height);
        for (unsigned row = 0; row < c->height; row++) {
          const uint8_t *s = bgra + (size_t)row * stride;
          uint8_t *d = c->luma + (size_t)row * c->width;
          for (unsigned x = 0; x < c->width; x++, s += 4)
            d[x] = (uint8_t)((s[1] * 150 + s[2] * 29 + s[3] * 77) >> 8);  // BGRA luma
        }
        c->luma_seq++;
        pthread_mutex_unlock(&c->luma_lock);
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

/* ---------------- public ---------------- */

static void lcap_close(struct lcap *c);

static struct lcap *lcap_open(const struct capture_opts *o, SDL_Renderer *ren)
{
  struct lcap *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  pthread_mutex_init(&c->frame_lock, NULL);
  pthread_cond_init(&c->frame_cond, NULL);
  pthread_mutex_init(&c->luma_lock, NULL);

  if (video_open(c, o, ren) < 0) { lcap_close(c); return NULL; }
  SDL_SetYUVConversionMode(c->height > 576 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601);
  c->have_audio = !o->no_audio && audio_start(c, o) == 0;
  return c;
}

static void lcap_close(struct lcap *c)
{
  if (!c) return;
  if (c->have_audio) {
    atomic_store(&c->audio_running, 0);
    pthread_join(c->ath, NULL);
    SDL_CloseAudioDevice(c->out);
    SDL_CloseAudioDevice(c->in);
  }
  if (c->session) {
    [c->session stopRunning];
    [c->output setSampleBufferDelegate:nil queue:nil];
    [c->session release];
    c->session = nil;
  }
  if (c->output) { [c->output release]; c->output = nil; }
  if (c->sink) { [c->sink release]; c->sink = nil; }
  if (c->queue) { dispatch_release(c->queue); c->queue = NULL; }
  if (c->latest) { CVPixelBufferRelease(c->latest); c->latest = NULL; }
  if (c->tex) SDL_DestroyTexture(c->tex);
  free(c->luma);
  pthread_cond_destroy(&c->frame_cond);
  pthread_mutex_destroy(&c->frame_lock);
  pthread_mutex_destroy(&c->luma_lock);
  free(c);
}

static int lcap_poll(struct lcap *c, int timeout_ms)
{
  pthread_mutex_lock(&c->frame_lock);
  if (c->seq == c->taken && timeout_ms > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&c->frame_cond, &c->frame_lock, &ts);
  }
  if (c->seq == c->taken) { pthread_mutex_unlock(&c->frame_lock); return 0; }

  // Hold our own reference: the delegate may replace (and release) the
  // newest frame as soon as the lock is dropped.
  CVPixelBufferRef pb = c->latest;
  CVPixelBufferRetain(pb);
  c->taken = c->seq;
  unsigned w = (unsigned)CVPixelBufferGetWidth(pb), h = (unsigned)CVPixelBufferGetHeight(pb);
  if (w != c->width || h != c->height) { c->width = w; c->height = h; }
  pthread_mutex_unlock(&c->frame_lock);

  upload_frame(c, pb);
  CVPixelBufferRelease(pb);
  return 1;
}

static SDL_Texture *lcap_texture(struct lcap *c) { return c->tex; }
static unsigned lcap_width(struct lcap *c) { return c->width; }
static unsigned lcap_height(struct lcap *c) { return c->height; }
static int lcap_has_audio(struct lcap *c) { return c->have_audio; }

static double lcap_audio_queued_ms(struct lcap *c)
{
  return c->have_audio ? SDL_GetQueuedAudioSize(c->out) / (double)AUDIO_BYTES_PER_MS : 0.0;
}

static void lcap_set_muted(struct lcap *c, int muted)
{
  atomic_store(&c->muted, muted);
  if (muted && c->have_audio) SDL_ClearQueuedAudio(c->out);
}

static int lcap_muted(struct lcap *c) { return atomic_load(&c->muted); }

static void lcap_want_luma(struct lcap *c) { atomic_store(&c->luma_wanted, 1); }

static unsigned lcap_copy_luma(struct lcap *c, uint8_t *dst)
{
  pthread_mutex_lock(&c->luma_lock);
  unsigned seq = c->luma_seq;
  if (seq) memcpy(dst, c->luma, (size_t)c->width * c->height);
  pthread_mutex_unlock(&c->luma_lock);
  return seq;
}

static unsigned lcap_luma_seq(struct lcap *c)
{
  pthread_mutex_lock(&c->luma_lock);
  unsigned seq = c->luma_seq;
  pthread_mutex_unlock(&c->luma_lock);
  return seq;
}

CAPTURE_DEFINE_OPS(struct lcap, lcap);

struct capture *local_capture_open(const struct capture_opts *o, SDL_Renderer *ren)
{
  struct lcap *c = lcap_open(o, ren);
  if (!c) return NULL;
  c->base.ops = &lcap_ops;
  return &c->base;
}
