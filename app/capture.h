// Capture: video (newest-frame-wins) + audio, platform specific.
//   Linux: V4L2 (mmap) + ALSA
//   macOS: AVFoundation (AVCaptureVideoDataOutput) + CoreAudio via SDL
//   -S:    MPEG-TS over UDP from a Raspberry Pi relay, decoded with FFmpeg
#ifndef SV_CAPTURE_H
#define SV_CAPTURE_H

#include <SDL2/SDL.h>
#include <stdint.h>

// Requested pixel format. NV12 is the fast path everywhere; YUYV is a
// Linux-only fallback for devices that cannot produce NV12.
enum {
  SV_PIX_NV12 = 1,
  SV_PIX_YUYV = 2,
};

struct capture_opts {
  const char *video_dev, *audio_dev;
  int width, height, fps;
  int format;  // SV_PIX_*
  int no_audio;
  int audio_max_ms;  // drop input beyond this much queued output
  // Set to receive from the Raspberry Pi relay over Wi-Fi instead of a local
  // card (pi/sv_stream_relay.py); width/height then only size the placeholder.
  const char *stream_host;
  int stream_port;
};

struct capture;

struct capture *capture_open(const struct capture_opts *o, SDL_Renderer *ren);
void capture_close(struct capture *c);

// Waits up to timeout_ms for a frame; uploads the newest one into the texture.
// Returns 1 if the texture was updated.
int capture_poll(struct capture *c, int timeout_ms);

SDL_Texture *capture_texture(struct capture *c);
unsigned capture_width(struct capture *c);
unsigned capture_height(struct capture *c);
int capture_has_audio(struct capture *c);
double capture_audio_queued_ms(struct capture *c);

// Grayscale (luma) snapshots for OCR. Any thread may call capture_want_luma;
// the next frame capture_poll uploads is also copied out. capture_copy_luma
// copies the latest snapshot (width*height bytes) and returns its sequence
// number, 0 if none yet.
void capture_want_luma(struct capture *c);
unsigned capture_copy_luma(struct capture *c, uint8_t *dst);

// Muting keeps capture running (so unmute is instant) but drops the audio.
void capture_set_muted(struct capture *c, int muted);
int capture_muted(struct capture *c);

// Prints the capture devices this platform offers (for -d / -a).
void capture_list_devices(void);

#endif
