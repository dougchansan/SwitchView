// SwitchView wire protocol: PC app  --UDP-->  Pico 2 W firmware.
// Shared by app/ and firmware/. Byte layout must stay stable: the Pico may be
// running an older build than the app.
#ifndef SWITCHVIEW_PROTO_H
#define SWITCHVIEW_PROTO_H

#include <stdint.h>

#define SV_PORT 39383
// Raspberry Pi stream relay (pi/sv_stream_relay.py): MPEG-TS over UDP.
#define SV_STREAM_PORT 39384
#define SV_MAGIC0 'S'
#define SV_MAGIC1 'W'

// Packet types. Unknown types are ignored by the firmware.
enum {
  SV_PKT_STATE = 1,  // absolute controller state
  SV_PKT_TEXT = 2,   // up to 16 ASCII chars to type on the keyboard interface
  SV_PKT_PING = 3,   // echoed back unchanged by the firmware (link health / RTT)
  SV_PKT_MOUSE = 4,  // relative mouse: buttons=SV_MOUSE_*, lx=dx, ly=dy, rx=wheel
};

// Mouse button bits in sv_packet.buttons for SV_PKT_MOUSE.
enum { SV_MOUSE_LEFT = 1u << 0, SV_MOUSE_RIGHT = 1u << 1, SV_MOUSE_MIDDLE = 1u << 2 };

// Button bits in sv_packet.buttons.
enum {
  SV_BTN_A = 1u << 0, SV_BTN_B = 1u << 1, SV_BTN_X = 1u << 2, SV_BTN_Y = 1u << 3,
  SV_BTN_L = 1u << 4, SV_BTN_R = 1u << 5, SV_BTN_ZL = 1u << 6, SV_BTN_ZR = 1u << 7,
  SV_BTN_PLUS = 1u << 8, SV_BTN_MINUS = 1u << 9, SV_BTN_HOME = 1u << 10,
  SV_BTN_CAPTURE = 1u << 11, SV_BTN_L3 = 1u << 12, SV_BTN_R3 = 1u << 13,
};

// Hat (d-pad) values in sv_packet.hat.
enum {
  SV_HAT_UP = 0, SV_HAT_UPRIGHT, SV_HAT_RIGHT, SV_HAT_DOWNRIGHT, SV_HAT_DOWN,
  SV_HAT_DOWNLEFT, SV_HAT_LEFT, SV_HAT_UPLEFT, SV_HAT_CENTER = 8,
};

#define SV_TEXT_MAX 16

// Non-printing keys, sent inside SV_PKT_TEXT like characters. ASCII \b, \t,
// \n and ESC (27) are also understood.
#define SV_KEY_CAPSLOCK 0x11
#define SV_KEY_UP 0x12
#define SV_KEY_DOWN 0x13
#define SV_KEY_LEFT 0x14
#define SV_KEY_RIGHT 0x15
// Flipper keyboard only: a 250 ms pause inside the Bad KB job (repeat for longer),
// so several commands can go out as one job. Other keyboards ignore it.
#define SV_KEY_PAUSE 0x16

// SV_PKT_PING echo: the firmware fills text[0] with the host's keyboard LED
// byte (bit 1 = Caps Lock) and text[1] = 1 once the host has sent one.
#define SV_LED_CAPSLOCK 0x02

// Sticks are int16 centred on 0; -32767..32767. ly/ry positive = up.
typedef struct __attribute__((packed)) {
  uint8_t magic[2];
  uint8_t type;
  uint8_t length;    // text bytes used (SV_PKT_TEXT)
  uint32_t buttons;  // SV_BTN_*
  int16_t lx, ly, rx, ry;
  uint8_t hat;       // SV_HAT_*
  uint8_t text[SV_TEXT_MAX];
} sv_packet;

#define SV_PACKET_SIZE 33

// Firmware types one character per ~100 ms from a 255-byte queue; senders
// should not exceed this rate for long strings.
#define SV_TEXT_CHARS_PER_SEC 10

#endif
