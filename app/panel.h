// Clickable Pro Controller + keyboard text entry, drawn below the capture.
#ifndef SV_PANEL_H
#define SV_PANEL_H

#include <SDL2/SDL.h>
#include "link.h"
#include "ocr.h"

struct script;
struct power;
struct companion;

// Design-space size the layout is authored in; scaled to the window.
#define PANEL_DESIGN_W 1280
#define PANEL_DESIGN_H 422

struct panel;

struct panel *panel_create(SDL_Renderer *ren);
void panel_destroy(struct panel *p);

// Feed every SDL event. Returns 1 if the panel consumed it.
int panel_event(struct panel *p, const SDL_Event *e, const SDL_Rect *area);

// Combined state from mouse, latches, keyboard and gamepad.
void panel_get_state(struct panel *p, struct link_state *out);

// Text the user submitted with Enter / the Enter button; returns 1 and fills
// buf (newline-terminated when sent with Enter) if there is something to send.
int panel_take_text(struct panel *p, char *buf, size_t len);

void panel_render(struct panel *p, const SDL_Rect *area, const char *status);

// 1 = Pico answering, 0 = not; drawn as a small dot in the panel corner.
void panel_set_link(struct panel *p, int up);

// Translucent round mute button drawn over the video (top-left); alpha 0..1 fades it.
void panel_draw_mute(struct panel *p, SDL_FRect rc, int muted, int hover, float alpha);
int panel_text_focused(struct panel *p);

// Script panel (right half) drives this runner.
void panel_set_script(struct panel *p, struct script *s);
// Fingerbot power buttons (hidden when NULL).
void panel_set_power(struct panel *p, struct power *pw);
// Companion watcher shown in the log card's second tab (hidden when NULL).
void panel_set_companion(struct panel *p, struct companion *c);

#endif
