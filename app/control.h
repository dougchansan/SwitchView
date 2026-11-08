// Local control API (HTTP on 127.0.0.1 only, default port 39390), so tools
// and AI agents can drive SwitchView with one call instead of a
// screenshot -> think -> press loop:
//
//   GET  /state                     screen label, learned-screen match, OCR text, link, power
//   GET  /goto?target=smilebasic[&wait=1]   run scripts/goto_<target>.lua
//   GET  /run?script=NAME.lua[&wait=1]      GET /stop     GET /log[?n=40]
//   GET  /tap?btn=A[,B][&ms=120]    GET /dpad?dir=down[&ms=100]
//   POST /type  (body = text)  or GET /type?text=...  [&wait=1]
//   GET  /power?action=toggle|boot|off|restart[&wait=1]
//   GET  /learn?label=topmenu       GET /forget?label=topmenu
//   GET  /frame.pgm                 current frame, 8-bit grey
//   GET  /companion[?n=40]          companion watcher lines (plain text)
#ifndef SV_CONTROL_H
#define SV_CONTROL_H

#include <stddef.h>

#include "capture.h"
#include "companion.h"
#include "frame.h"
#include "link.h"
#include "ocr.h"
#include "power.h"
#include "script.h"

struct control_ctx {
  struct script *script;
  struct ocr *ocr;
  struct capture *cap;
  struct frame_lib *frames;
  struct power *power;
  struct companion *companion;  // optional watcher feed (/companion)
};

struct control;

struct control *control_start(int port, const struct control_ctx *ctx);
void control_stop(struct control *c);

// Main loop: merge the API's pad input and hand its text to the link.
void control_get_input(struct control *c, struct link_state *out, int *active);
int control_take_text(struct control *c, char *buf, size_t len);
// Main loop: publish link health for /state and type?wait=1.
void control_set_link(struct control *c, const struct link_stats *st, const char *desc);

#endif
