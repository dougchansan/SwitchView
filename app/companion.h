// Companion log: runs a watcher command (default: kalua-sow's watch-pretty.py
// on /tmp/ns2-direct-attempts/attempt-1.log) and keeps its last lines, with
// the ANSI colours it prints, for the panel's "Seedsow log" tab.
//   SV_COMPANION   shell command to run instead (e.g. "tail -n 50 -F file")
//   SV_COMPANION=none  disables it
#ifndef SV_COMPANION_H
#define SV_COMPANION_H

#include <stddef.h>
#include <stdint.h>

#define COMP_LINES 200
#define COMP_COLS 220
#define COMP_SEGS 12

enum { CC_DEFAULT, CC_GREY, CC_RED, CC_GREEN, CC_YELLOW, CC_BLUE, CC_MAGENTA, CC_CYAN };

struct comp_seg { uint8_t start, len, color, bold; };
struct comp_line {
  char text[COMP_COLS];
  struct comp_seg seg[COMP_SEGS];
  int nseg;
};

struct companion;

struct companion *companion_start(const char *cmd, int columns);
void companion_stop(struct companion *c);
// Copies the last n lines (oldest first); returns how many.
int companion_lines(struct companion *c, struct comp_line *out, int max);
// Bumps on every new line (to redraw only on change).
unsigned companion_seq(struct companion *c);
const char *companion_command(struct companion *c);

#endif
