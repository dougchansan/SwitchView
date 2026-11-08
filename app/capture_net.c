// Network capture backend: receives the capture card's picture and sound from
// the Raspberry Pi relay (pi/sv_stream_relay.py) as MPEG-TS over UDP, and
// decodes it with FFmpeg's libraries. H.264 frames go to an IYUV texture
// (newest frame wins, GPU colour conversion); Opus audio goes to SDL's output
// with a bounded queue, like the local backends.
//
// The app speaks first: a hello every HELLO_MS from the same socket the stream
// arrives on, so nothing needs to know the Mac's address and no inbound port
// has to be opened. A stall longer than STALL_MS reopens the session, which
// also asks the relay for a fresh keyframe.
#include "capture_impl.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HELLO_MS 500
#define STALL_MS 2000     // no data this long during a session: reconnect
#define CONNECT_MS 5000   // no data this long after a hello: re-resolve and retry
#define AUDIO_RATE 48000
#define NET_AUDIO_MIN_MS 80  // Wi-Fi delivers in bursts; a tighter cap just drops sound

struct ncap {
  struct capture base;  // first: see capture_impl.h
  SDL_Renderer *ren;
  char host[256];
  int port;
  // video, handed from the network thread to capture_poll
  SDL_Texture *tex;
  unsigned width, height, tex_w, tex_h;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  AVFrame *latest, *shown;
  unsigned long seq, taken;
  int warned_fmt;
  // network thread
  pthread_t th;
  atomic_int running;
  int sock;
  struct sockaddr_storage peer;
  socklen_t peer_len;
  unsigned session;
  uint8_t dgram[65536];
  size_t dlen, doff;
  uint64_t last_hello, last_data, session_start;
  int got_data;
  // audio
  SDL_AudioDeviceID out;
  unsigned max_bytes;
  atomic_int muted;
  SwrContext *swr;
  uint8_t *abuf;
  int abuf_samples;
  // OCR snapshot
  atomic_int luma_wanted;
  pthread_mutex_t luma_lock;
  uint8_t *luma;
  unsigned luma_w, luma_h, luma_seq;
};

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---------------- socket ---------------- */

static int same_peer(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
  if (a->ss_family != b->ss_family) return 0;
  if (a->ss_family == AF_INET) {
    const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
  }
  if (a->ss_family == AF_INET6) {
    const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
    return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, 16);
  }
  return 0;
}

// Resolves the Pi (mDNS names like raspberrypi.local work) and opens a socket.
static int net_connect(struct ncap *c)
{
  char port[16];
  snprintf(port, sizeof port, "%d", c->port);
  struct addrinfo hints = {.ai_socktype = SOCK_DGRAM, .ai_flags = AI_ADDRCONFIG}, *res = NULL;
  int err = getaddrinfo(c->host, port, &hints, &res);
  if (err) {
    fprintf(stderr, "stream: cannot resolve %s: %s\n", c->host, gai_strerror(err));
    return -1;
  }
  struct addrinfo *pick = res;  // prefer IPv4: simplest path on a home LAN
  for (struct addrinfo *a = res; a; a = a->ai_next)
    if (a->ai_family == AF_INET) { pick = a; break; }
  c->sock = socket(pick->ai_family, SOCK_DGRAM, 0);
  if (c->sock < 0) { perror("stream: socket"); freeaddrinfo(res); return -1; }
  int rcv = 4 << 20;  // ride out a Wi-Fi burst without kernel drops
  setsockopt(c->sock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
  memcpy(&c->peer, pick->ai_addr, pick->ai_addrlen);
  c->peer_len = pick->ai_addrlen;
  freeaddrinfo(res);
  return 0;
}

static void send_msg(struct ncap *c, const char *msg)
{
  if (c->sock >= 0)
    sendto(c->sock, msg, strlen(msg), 0, (struct sockaddr *)&c->peer, c->peer_len);
}

static void send_hello(struct ncap *c)
{
  char msg[64];
  snprintf(msg, sizeof msg, "SVSTREAM1 HELLO %d-%u", (int)getpid(), c->session);
  send_msg(c, msg);
  c->last_hello = now_ms();
}

// AVIOContext read callback: one datagram at a time, hellos in between.
static int read_cb(void *opaque, uint8_t *buf, int size)
{
  struct ncap *c = opaque;
  for (;;) {
    if (!atomic_load(&c->running)) return AVERROR_EXIT;
    if (c->doff < c->dlen) {
      int n = (int)(c->dlen - c->doff) < size ? (int)(c->dlen - c->doff) : size;
      memcpy(buf, c->dgram + c->doff, (size_t)n);
      c->doff += (size_t)n;
      return n;
    }
    uint64_t now = now_ms();
    if (now - c->last_hello >= HELLO_MS) send_hello(c);
    if (c->got_data ? now - c->last_data > STALL_MS : now - c->session_start > CONNECT_MS)
      return AVERROR(ETIMEDOUT);
    struct pollfd pf = {.fd = c->sock, .events = POLLIN};
    if (poll(&pf, 1, 100) <= 0) continue;
    struct sockaddr_storage from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(c->sock, c->dgram, sizeof c->dgram, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0 || !same_peer(&from, &c->peer)) continue;
    if (!c->got_data) fprintf(stderr, "stream: receiving from %s\n", c->host);
    c->got_data = 1;
    c->last_data = now_ms();
    c->dlen = (size_t)n;
    c->doff = 0;
  }
}

static int interrupt_cb(void *opaque)
{
  struct ncap *c = opaque;
  return !atomic_load(&c->running);
}

/* ---------------- decoding ---------------- */

static AVCodecContext *open_decoder(AVStream *st)
{
  const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) return NULL;
  AVCodecContext *ctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(ctx, st->codecpar);
  ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  ctx->thread_type = FF_THREAD_SLICE;  // frame threads would add a frame of delay each
  ctx->thread_count = 0;
  if (avcodec_open2(ctx, dec, NULL) < 0) avcodec_free_context(&ctx);
  return ctx;
}

static void publish_video(struct ncap *c, AVFrame *f)
{
  pthread_mutex_lock(&c->lock);
  av_frame_unref(c->latest);
  av_frame_move_ref(c->latest, f);
  c->seq++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->lock);
}

static void play_audio(struct ncap *c, AVFrame *f)
{
  if (!c->out) return;
  if (!c->swr) {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&c->swr, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE, &f->ch_layout,
                            (enum AVSampleFormat)f->format, f->sample_rate, 0, NULL) < 0 ||
        swr_init(c->swr) < 0) {
      fprintf(stderr, "stream: cannot convert the audio format\n");
      swr_free(&c->swr);
      return;
    }
  }
  int want = swr_get_out_samples(c->swr, f->nb_samples);
  if (want > c->abuf_samples) {
    free(c->abuf);
    c->abuf = malloc((size_t)want * 4);
    c->abuf_samples = c->abuf ? want : 0;
  }
  if (!c->abuf) return;
  int n = swr_convert(c->swr, &c->abuf, want, (const uint8_t **)f->extended_data, f->nb_samples);
  if (n <= 0 || atomic_load(&c->muted)) return;
  // Drop rather than let the output queue (and latency) grow.
  if (SDL_GetQueuedAudioSize(c->out) > c->max_bytes) return;
  SDL_QueueAudio(c->out, c->abuf, (Uint32)n * 4);
}

// One connection to the relay: open the demuxer, decode until it stalls.
static void run_session(struct ncap *c)
{
  c->session++;
  c->dlen = c->doff = 0;
  c->got_data = 0;
  c->session_start = now_ms();
  send_hello(c);

  int bufsz = 1 << 15;
  AVIOContext *pb = avio_alloc_context(av_malloc((size_t)bufsz), bufsz, 0, c, read_cb, NULL, NULL);
  AVFormatContext *fmt = avformat_alloc_context();
  fmt->pb = pb;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOBUFFER;
  fmt->interrupt_callback = (AVIOInterruptCB){interrupt_cb, c};
  fmt->probesize = 1 << 18;
  fmt->max_analyze_duration = 500000;
  AVCodecContext *vctx = NULL, *actx = NULL;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  int vi = -1, ai = -1;

  if (avformat_open_input(&fmt, NULL, av_find_input_format("mpegts"), NULL) < 0) {
    fmt = NULL;  // freed by avformat_open_input on failure
    goto done;
  }
  if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
  vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (vi < 0 || !(vctx = open_decoder(fmt->streams[vi]))) {
    fprintf(stderr, "stream: no decodable video in the stream\n");
    goto done;
  }
  if (ai >= 0 && c->out) actx = open_decoder(fmt->streams[ai]);
  fprintf(stderr, "stream: %dx%d %s%s%s\n", vctx->width, vctx->height,
          avcodec_get_name(vctx->codec_id), actx ? " + " : ", no audio",
          actx ? avcodec_get_name(actx->codec_id) : "");

  while (atomic_load(&c->running) && av_read_frame(fmt, pkt) >= 0) {
    AVCodecContext *ctx = pkt->stream_index == vi ? vctx : pkt->stream_index == ai ? actx : NULL;
    if (ctx && avcodec_send_packet(ctx, pkt) >= 0) {
      while (avcodec_receive_frame(ctx, frame) >= 0) {
        if (ctx == vctx) publish_video(c, frame);
        else { play_audio(c, frame); av_frame_unref(frame); }
      }
    }
    av_packet_unref(pkt);
  }
  if (atomic_load(&c->running)) fprintf(stderr, "stream: lost %s, reconnecting\n", c->host);

done:
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&vctx);
  avcodec_free_context(&actx);
  swr_free(&c->swr);  // the next session may carry a different audio format
  avformat_close_input(&fmt);
  if (pb) av_freep(&pb->buffer);
  avio_context_free(&pb);
}

static void *net_thread(void *p)
{
  struct ncap *c = p;
  fprintf(stderr, "stream: waiting for %s port %d\n", c->host, c->port);
  while (atomic_load(&c->running)) {
    if (net_connect(c) == 0) {
      run_session(c);
      if (!atomic_load(&c->running)) break;  // keep the socket for the goodbye
      close(c->sock);
      c->sock = -1;
    }
    for (int i = 0; i < 10 && atomic_load(&c->running); i++) usleep(50000);
  }
  return NULL;
}

/* ---------------- capture API ---------------- */

static void ncap_close(struct ncap *c)
{
  if (atomic_exchange(&c->running, 0)) pthread_join(c->th, NULL);
  if (c->sock >= 0) {
    send_msg(c, "SVSTREAM1 BYE");
    close(c->sock);
  }
  if (c->out) SDL_CloseAudioDevice(c->out);
  av_frame_free(&c->latest);
  av_frame_free(&c->shown);
  if (c->tex) SDL_DestroyTexture(c->tex);
  free(c->abuf);
  free(c->luma);
  pthread_cond_destroy(&c->cond);
  pthread_mutex_destroy(&c->lock);
  pthread_mutex_destroy(&c->luma_lock);
  free(c);
}

// A black frame until the first picture arrives.
static int make_texture(struct ncap *c, unsigned w, unsigned h)
{
  if (c->tex) SDL_DestroyTexture(c->tex);
  c->tex = SDL_CreateTexture(c->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
  if (!c->tex) { fprintf(stderr, "SDL texture: %s\n", SDL_GetError()); return -1; }
  c->tex_w = c->width = w;
  c->tex_h = c->height = h;
  SDL_SetYUVConversionMode(h > 576 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601);
  return 0;
}

static int ncap_poll(struct ncap *c, int timeout_ms)
{
  pthread_mutex_lock(&c->lock);
  if (c->seq == c->taken && timeout_ms > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&c->cond, &c->lock, &ts);
  }
  if (c->seq == c->taken) { pthread_mutex_unlock(&c->lock); return 0; }
  av_frame_unref(c->shown);
  av_frame_move_ref(c->shown, c->latest);
  c->taken = c->seq;
  pthread_mutex_unlock(&c->lock);

  AVFrame *f = c->shown;
  if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
    if (!c->warned_fmt++)
      fprintf(stderr, "stream: unsupported picture format %s (want yuv420p)\n",
              av_get_pix_fmt_name((enum AVPixelFormat)f->format));
    return 0;
  }
  unsigned w = (unsigned)f->width, h = (unsigned)f->height;
  if ((w != c->tex_w || h != c->tex_h) && make_texture(c, w, h) < 0) return 0;
  SDL_UpdateYUVTexture(c->tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1],
                       f->data[2], f->linesize[2]);
  if (atomic_exchange(&c->luma_wanted, 0)) {
    pthread_mutex_lock(&c->luma_lock);
    if (c->luma_w != w || c->luma_h != h) {
      free(c->luma);
      c->luma = malloc((size_t)w * h);
      c->luma_w = w;
      c->luma_h = h;
    }
    if (c->luma) {
      for (unsigned y = 0; y < h; y++)
        memcpy(c->luma + (size_t)y * w, f->data[0] + (size_t)y * (size_t)f->linesize[0], w);
      c->luma_seq++;
    }
    pthread_mutex_unlock(&c->luma_lock);
  }
  return 1;
}

static SDL_Texture *ncap_texture(struct ncap *c) { return c->tex; }
static unsigned ncap_width(struct ncap *c) { return c->width; }
static unsigned ncap_height(struct ncap *c) { return c->height; }
static int ncap_has_audio(struct ncap *c) { return c->out != 0; }
static double ncap_audio_queued_ms(struct ncap *c)
{
  return c->out ? SDL_GetQueuedAudioSize(c->out) / (AUDIO_RATE * 4 / 1000.0) : 0.0;
}

static void ncap_set_muted(struct ncap *c, int muted)
{
  atomic_store(&c->muted, muted);
  if (muted && c->out) SDL_ClearQueuedAudio(c->out);
}

static int ncap_muted(struct ncap *c) { return atomic_load(&c->muted); }
static void ncap_want_luma(struct ncap *c) { atomic_store(&c->luma_wanted, 1); }

// OCR reads width*height bytes: only hand out a snapshot of the current size.
static unsigned ncap_copy_luma(struct ncap *c, uint8_t *dst)
{
  pthread_mutex_lock(&c->luma_lock);
  unsigned seq = c->luma_w == c->width && c->luma_h == c->height ? c->luma_seq : 0;
  if (seq) memcpy(dst, c->luma, (size_t)c->luma_w * c->luma_h);
  pthread_mutex_unlock(&c->luma_lock);
  return seq;
}

static unsigned ncap_luma_seq(struct ncap *c)
{
  pthread_mutex_lock(&c->luma_lock);
  unsigned seq = c->luma_w == c->width && c->luma_h == c->height ? c->luma_seq : 0;
  pthread_mutex_unlock(&c->luma_lock);
  return seq;
}

CAPTURE_DEFINE_OPS(struct ncap, ncap);

struct capture *net_capture_open(const struct capture_opts *o, SDL_Renderer *ren)
{
  struct ncap *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->base.ops = &ncap_ops;
  c->ren = ren;
  c->sock = -1;
  c->port = o->stream_port;
  snprintf(c->host, sizeof c->host, "%s", o->stream_host);
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->cond, NULL);
  pthread_mutex_init(&c->luma_lock, NULL);
  c->latest = av_frame_alloc();
  c->shown = av_frame_alloc();
  av_log_set_level(AV_LOG_ERROR);

  if (make_texture(c, (unsigned)o->width, (unsigned)o->height) < 0) { ncap_close(c); return NULL; }
  {
    // Paint the placeholder black (limited-range Y=16, neutral chroma).
    size_t ys = (size_t)c->width * c->height, cs = ys / 4;
    uint8_t *px = malloc(ys + 2 * cs);
    if (px) {
      memset(px, 16, ys);
      memset(px + ys, 128, 2 * cs);
      SDL_UpdateYUVTexture(c->tex, NULL, px, (int)c->width, px + ys, (int)c->width / 2,
                           px + ys + cs, (int)c->width / 2);
      free(px);
    }
  }

  if (!o->no_audio) {
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 256;
    c->out = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!c->out) fprintf(stderr, "audio: SDL output: %s\n", SDL_GetError());
    else {
      int ms = o->audio_max_ms > NET_AUDIO_MIN_MS ? o->audio_max_ms : NET_AUDIO_MIN_MS;
      c->max_bytes = AUDIO_RATE * 4 * (unsigned)ms / 1000;
      SDL_PauseAudioDevice(c->out, 0);
    }
  }

  atomic_store(&c->running, 1);
  if (pthread_create(&c->th, NULL, net_thread, c) != 0) {
    atomic_store(&c->running, 0);
    ncap_close(c);
    return NULL;
  }
  return c->base.ops ? &c->base : NULL;
}
