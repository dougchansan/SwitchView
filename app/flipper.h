// Flipper Zero typing over the ESP32's raw RPC bridge (TCP :8080 <-> the
// Flipper's expansion UART), the same path ~/flipper2jb drives:
//   storage write  /ext/badusb/switchview.txt   (key-by-key Bad KB script)
//   app start      "Bad KB" <path>, then an OK press to start the script
//   wait for the script's own duration, then Back to leave Bad KB
// The keystrokes come out of the Flipper's own USB, plugged into the console.
// Requirements (as for flipper2jb): Momentum firmware Bad KB in USB mode with
// the en-UK layout, the Flipper at its desktop, console Caps Lock off.
#ifndef SV_FLIPPER_H
#define SV_FLIPPER_H

#include <stddef.h>

// Types text (ASCII; \n Enter, \b Backspace, \t Tab, 27 Esc, and the
// SV_KEY_* specials). Blocks until Bad KB has finished and closed.
// Returns the number of input bytes typed (== len on success), or -1 with a
// reason in err. Long text is split by the caller.
long flipper_type(const char *host, int port, const char *text, size_t len,
                  char *err, size_t errlen);

// Estimated seconds a job of this text takes (start delay + keys), so callers
// can report progress.
double flipper_estimate(const char *text, size_t len);

#endif
