// Private to the capture backends: every backend's state struct starts with a
// struct capture whose ops point at that backend's functions; capture_src.c
// turns the public capture_* calls into calls through it.
#ifndef SV_CAPTURE_IMPL_H
#define SV_CAPTURE_IMPL_H

#include "capture.h"

struct capture_ops {
  void (*close)(struct capture *);
  int (*poll)(struct capture *, int timeout_ms);
  SDL_Texture *(*texture)(struct capture *);
  unsigned (*width)(struct capture *);
  unsigned (*height)(struct capture *);
  int (*has_audio)(struct capture *);
  double (*audio_queued_ms)(struct capture *);
  void (*want_luma)(struct capture *);
  unsigned (*copy_luma)(struct capture *, uint8_t *);
  unsigned (*luma_seq)(struct capture *);
  void (*set_muted)(struct capture *, int);
  int (*muted)(struct capture *);
};

struct capture {
  const struct capture_ops *ops;
};

// Local capture card (capture.c on Linux, capture_macos.m on macOS).
struct capture *local_capture_open(const struct capture_opts *o, SDL_Renderer *ren);
// MPEG-TS over UDP from the Raspberry Pi relay (capture_net.c).
struct capture *net_capture_open(const struct capture_opts *o, SDL_Renderer *ren);

// Builds a backend's ops table from functions named PFX_close(T *), PFX_poll(T *, int), ...
// where T starts with a struct capture.
#define CAPTURE_DEFINE_OPS(T, PFX)                                                        \
  static void PFX##_op_close(struct capture *c) { PFX##_close((T *)c); }                  \
  static int PFX##_op_poll(struct capture *c, int t) { return PFX##_poll((T *)c, t); }    \
  static SDL_Texture *PFX##_op_texture(struct capture *c) { return PFX##_texture((T *)c); } \
  static unsigned PFX##_op_width(struct capture *c) { return PFX##_width((T *)c); }       \
  static unsigned PFX##_op_height(struct capture *c) { return PFX##_height((T *)c); }     \
  static int PFX##_op_has_audio(struct capture *c) { return PFX##_has_audio((T *)c); }    \
  static double PFX##_op_audio_queued_ms(struct capture *c)                               \
  { return PFX##_audio_queued_ms((T *)c); }                                               \
  static void PFX##_op_want_luma(struct capture *c) { PFX##_want_luma((T *)c); }          \
  static unsigned PFX##_op_copy_luma(struct capture *c, uint8_t *d)                       \
  { return PFX##_copy_luma((T *)c, d); }                                                  \
  static unsigned PFX##_op_luma_seq(struct capture *c) { return PFX##_luma_seq((T *)c); }   \
  static void PFX##_op_set_muted(struct capture *c, int m) { PFX##_set_muted((T *)c, m); } \
  static int PFX##_op_muted(struct capture *c) { return PFX##_muted((T *)c); }            \
  static const struct capture_ops PFX##_ops = {                                           \
      PFX##_op_close,     PFX##_op_poll,      PFX##_op_texture,         PFX##_op_width,   \
      PFX##_op_height,    PFX##_op_has_audio, PFX##_op_audio_queued_ms, PFX##_op_want_luma, \
      PFX##_op_copy_luma, PFX##_op_luma_seq, PFX##_op_set_muted, PFX##_op_muted}

#endif
