// Public capture API: picks the backend at open time and dispatches to it.
#include "capture_impl.h"

struct capture *capture_open(const struct capture_opts *o, SDL_Renderer *ren)
{
  return o->stream_host ? net_capture_open(o, ren) : local_capture_open(o, ren);
}

void capture_close(struct capture *c) { if (c) c->ops->close(c); }
int capture_poll(struct capture *c, int timeout_ms) { return c->ops->poll(c, timeout_ms); }
SDL_Texture *capture_texture(struct capture *c) { return c->ops->texture(c); }
unsigned capture_width(struct capture *c) { return c->ops->width(c); }
unsigned capture_height(struct capture *c) { return c->ops->height(c); }
int capture_has_audio(struct capture *c) { return c->ops->has_audio(c); }
double capture_audio_queued_ms(struct capture *c) { return c->ops->audio_queued_ms(c); }
void capture_want_luma(struct capture *c) { c->ops->want_luma(c); }
unsigned capture_copy_luma(struct capture *c, uint8_t *dst) { return c->ops->copy_luma(c, dst); }
unsigned capture_luma_seq(struct capture *c) { return c->ops->luma_seq(c); }
void capture_set_muted(struct capture *c, int muted) { c->ops->set_muted(c, muted); }
int capture_muted(struct capture *c) { return c->ops->muted(c); }
