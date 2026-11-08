# Driving the Switch 2 through SwitchView (for agents)

You can see and control a Nintendo Switch 2 through **SwitchView**, a local app that already
has the capture card, the controller, the keyboard, OCR and the console's power button
wired up. Do NOT take screenshots with ffmpeg, run the ns2-savekit harness, curl the ESP32 /
fingerbot directly, or drive the console with your own screenshot -> think -> press loop.
Use SwitchView's control API: one call does what used to take many.

## Is it running?

```sh
curl -s http://127.0.0.1:39390/state
```

If that fails, start it (it must run under Terminal, which has the camera permission):

```sh
open -a Terminal /Users/douglaswhittingham/Desktop/switchview/.claude/worktrees/switch-view-integrations-2548b8/tools/switchview.command
```

Wait ~6 s and check `/state` again. The CLI wrapper below is in the same `tools/` folder:
`SV=/Users/douglaswhittingham/Desktop/switchview/.claude/worktrees/switch-view-integrations-2548b8/tools/sv`

## Commands

| Need | Command | Notes |
|---|---|---|
| What's on screen | `$SV state` | JSON: `picture` (false = dark/asleep/no signal), `text` (OCR, lower-case), `match` (learned screen), `link.up`, `power.status`. ~0.1 s |
| Go to SmileBASIC Direct mode | `$SV goto smilebasic` | Blocks until done; returns the step log. 0.1 s if already there, ~8–17 s otherwise |
| Go to Seedsow Lullaby (save launcher menu) | `$SV goto seedsow` | |
| Go to the HTTP console (Seedsow launcher -> Y: HTTP server) | `$SV goto console` | Done when "http control channel ready" is on screen |
| Type text | `$SV type 'PRINT 7'` | Goes through the Flipper (Bad KB). ~10–15 s per job; a newline is added. Returns when typing finished |
| Press a button | `$SV tap A` / `$SV tap HOME 150` | A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE L3 R3 |
| D-pad | `$SV dpad down` | up down left right (+ diagonals) |
| Seedsow attempt log (watch-pretty view) | `$SV companion 40` | Alias steps, canary hit/miss, endpoint phases |
| Script log | `$SV log` | |
| Current frame as an image | `$SV frame /tmp/now.pgm` | Only when OCR text isn't enough; convert with `sips -s format png` |
| Power | `$SV power toggle` (sleep/wake) / `boot` (3 s press) | `off` and `restart` force the console off: **ask the user first** |
| Teach a screen (instant recognition later) | `$SV learn <state-name>` | e.g. `seedsow`, `topmenu` |

Raw HTTP equivalents: `GET /state`, `/goto?target=X&wait=1`, `POST /type?wait=1` (body = text),
`/tap?btn=A&ms=120`, `/dpad?dir=down`, `/power?action=boot&wait=1`, `/companion?n=40`,
`/log`, `/frame.pgm`, `/learn?label=X`.

## Rules

1. **Look before you type.** Typing goes to whatever has focus. Run `$SV goto smilebasic`
   (or check `state.text` for "bytes free ... ok") before typing BASIC.
2. **Prefer `goto` over manual presses.** It handles sleep/dark screens, the start screen,
   the account picker, close-software dialogs, splash screens and a wedged program. Its log
   says what it saw at each step; read it if it fails.
3. **Switching targets closes the other software** (e.g. going to SmileBASIC closes Seedsow
   and stops the HTTP server). Make sure that is what the user wants.
4. **Never `power off` / `power restart` without the user's OK.** `boot` and `toggle` are fine
   for waking a dark console. If `picture` is false, try `$SV goto <target>` first: it wakes
   the console itself (HOME, then the fingerbot).
5. `tap`/`dpad` work while a script (`goto`) runs and add to what it presses, like the on-screen pad;
   pressing during a `goto` can derail it, so prefer waiting for it or `$SV stop` first.
6. Don't run other tools that open the capture card, the ESP32 bridge (port 8080 on
   192.168.50.215) or the fingerbot while SwitchView is using them.

## Known limits

- The ESP32 pad has no analog sticks (buttons and d-pad only).
- Flipper typing needs the Flipper at its desktop with its USB-C in the console; a job that
  fails is logged in `$SV log` and SwitchView's Terminal window.
- OCR can split words ("control ler"); match on distinctive fragments.
