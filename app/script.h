// Lua script runner. A script runs on its own thread; it drives the same
// controller/keyboard as the panel and reads the screen through OCR.
#ifndef SV_SCRIPT_H
#define SV_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

#include "capture.h"
#include "frame.h"
#include "link.h"
#include "ocr.h"
#include "power.h"

#define SCRIPT_LOG_LINES 64
#define SCRIPT_LOG_COLS 120

struct script;

struct script *script_create(struct ocr *ocr, const char *dir, struct capture *cap);
// Optional collaborators for the Lua API (power(), match()/learn()).
void script_set_power(struct script *s, struct power *p);
void script_set_frames(struct script *s, struct frame_lib *lib);
// Keyboard busy flag from the link, so type() returns when typing is done.
void script_set_typing(struct script *s, int typing);
void script_destroy(struct script *s);

// Scripts found in the directory (*.lua, sorted). Rescanned on demand.
int script_count(struct script *s);
const char *script_name(struct script *s, int i);
void script_rescan(struct script *s);

int script_start(struct script *s, int index);
// Start by file name ("goto_smilebasic.lua"); stops a running script first.
int script_start_name(struct script *s, const char *name);
// File name of the running (or last) script.
void script_current(struct script *s, char *buf, size_t len);
// Append a line to the script log from outside a script (power, control API).
void script_note(struct script *s, const char *line);
void script_stop(struct script *s);
int script_running(struct script *s);

// Controller input the running script wants; *active is 0 when idle.
void script_get_input(struct script *s, struct link_state *out, int *active);
// Host keyboard LED byte from the link (-1 = unknown), for capslock().
void script_set_leds(struct script *s, int leds);

// Text the script typed since the last call (for link_send_text).
int script_take_text(struct script *s, char *buf, size_t len);

// Last screen label the script classified ("" if none) and log lines,
// oldest first; returns the number of lines copied.
void script_screen(struct script *s, char *buf, size_t len);
int script_log(struct script *s, char lines[][SCRIPT_LOG_COLS], int max);

// Words the script is currently looking for (matched rule / find target),
// space-free and lower-case, for highlighting in the OCR overlay.
int script_highlights(struct script *s, char words[][64], int max);

#endif
