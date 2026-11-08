// Frame fingerprints: a 64x36 luma thumbnail of the live capture, cheap
// enough (well under a millisecond) to take on every decision. Used for
//   - "did the screen change / has it stopped animating" waits, so navigation
//     reacts to the console instead of sleeping a guessed amount;
//   - black / no-signal detection without any OCR;
//   - a tiny nearest-neighbour screen classifier: learn("topmenu") stores the
//     current thumbnail, match() names the closest stored screen.
#ifndef SV_FRAME_H
#define SV_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "capture.h"

#define FT_W 64
#define FT_H 36

struct frame_shot {
  uint8_t px[FT_W * FT_H];
  float mean, sd;  // luma statistics of the whole frame
  unsigned seq;
};

// Grab a thumbnail of a frame captured after this call. Returns 0, or -1
// when no new frame arrived within timeout_ms (no signal / card stalled).
int frame_grab(struct capture *cap, struct frame_shot *out, int timeout_ms);

// Mean absolute luma difference, 0 (identical) .. 255.
float frame_dist(const struct frame_shot *a, const struct frame_shot *b);

// A uniform frame: black, a "no signal" card, or a stalled capture.
int frame_flat(const struct frame_shot *s);

struct frame_lib;

// Screens are stored as <dir>/<label>[#n].sv (raw thumbnail bytes).
struct frame_lib *frame_lib_open(const char *dir);
void frame_lib_close(struct frame_lib *lib);
int frame_lib_learn(struct frame_lib *lib, const char *label, const struct frame_shot *s);
int frame_lib_forget(struct frame_lib *lib, const char *label);
// Nearest stored screen; label is copied to buf ("" if the library is empty).
// Returns its distance (999 if none).
float frame_lib_match(struct frame_lib *lib, const struct frame_shot *s, char *buf, size_t len);
int frame_lib_count(struct frame_lib *lib);

#endif
