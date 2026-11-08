// Link to the board in the Switch dock. Two pad backends and two keyboards:
//
//   pad  UDP   Pico 2 W / ESP-IDF firmware speaking proto/switchview_proto.h
//        HTTP  the ESP32-S2 "pro-2" Arduino firmware (ns2-savekit esp32_pro):
//              GET /pro?mask=..&ms=..  (buttons + d-pad; no analog sticks)
//   kbd  UDP   the Pico's USB keyboard interface (SV_PKT_TEXT)
//        Flipper  Bad KB through the ESP32's raw RPC bridge (TCP :8080 to the
//              Flipper's UART), see app/flipper.h
//
// The pad host picks the pad backend: "http://IP" = ESP32, a bare IP = UDP.
// The keyboard follows the pad unless kb_url names a Flipper bridge.
#ifndef SV_LINK_H
#define SV_LINK_H

#include <stdint.h>

struct link;

struct link_state {
  uint32_t buttons;  // SV_BTN_*
  int16_t lx, ly, rx, ry;
  uint8_t hat;       // SV_HAT_*
};

struct link_stats {
  unsigned long packets;     // state + text packets / requests sent
  unsigned long send_errors;
  unsigned text_queued;      // chars waiting to go out
  uint32_t last_send_ms;
  int up;                    // board answered within the last couple of seconds
  float rtt_ms;              // last measured round trip
  int leds;                  // host keyboard LEDs (SV_LED_*), -1 = not reported yet
  int typing;                // keyboard still busy (queued text or a Bad KB job running)
  int kb_flipper;            // 1 when text goes through the Flipper
};

// host: "192.168.4.60" (UDP pad) or "http://192.168.50.215" (ESP32 pro-2 pad).
// kb_url: NULL = keyboard follows the pad (Flipper on an http pad, UDP otherwise);
// "http://..." = Flipper bridge on that board; "none" = no keyboard.
struct link *link_open(const char *host, int port, const char *kb_url);
void link_close(struct link *l);

// Latest desired state. Sent immediately if changed, and re-sent as a
// keepalive so a lost packet costs milliseconds, not a stuck button.
// Every distinct state is delivered in order, so a short tap is never lost.
void link_set_state(struct link *l, const struct link_state *s);

// Relative mouse motion / buttons for the Switch's USB mouse (UDP pad only).
void link_mouse(struct link *l, int dx, int dy, int wheel, unsigned buttons);

// Queue text for the keyboard. UDP: paced to the firmware's typing rate.
// Flipper: batched into Bad KB jobs (a line, or after a short pause).
void link_send_text(struct link *l, const char *text);

// Call often (every loop iteration). now_ms = SDL_GetTicks().
void link_poll(struct link *l, uint32_t now_ms);

// 1 if the board takes USB mouse input (Pico/UDP); the ESP32 pro-2 does not.
int link_has_mouse(struct link *l);

void link_get_stats(struct link *l, struct link_stats *out);
const char *link_describe(struct link *l);

#endif
