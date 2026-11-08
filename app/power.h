// Console power through the Tuya Fingerbot that presses the Switch 2's power
// button. SwitchView talks to the fingerbot HTTP server (ns2jb-rpc
// fingerbot.py serve, default http://127.0.0.1:8080), which owns the BLE link:
//   GET /press?seconds=N   arm down, wait N s on the host, arm up
//   GET /up                lift the arm   GET /travel?pct=N  press depth
//
// Actions run on a worker thread, one at a time:
//   toggle   0.5 s press: sleep <-> wake
//   boot     3 s press (measured: what reliably boots it from off)
//   off      16 s hold (force off), verified by the picture going away
//   restart  off, then boot once the console is really off, then wait for HDMI
#ifndef SV_POWER_H
#define SV_POWER_H

#include <stddef.h>

#include "capture.h"

struct power;

typedef void (*power_log_fn)(void *ud, const char *line);

struct power *power_create(const char *url, struct capture *cap, power_log_fn log, void *ud);
void power_destroy(struct power *p);

// Starts an action ("toggle", "boot", "off", "restart"). Returns 0 if
// started, -1 if unknown or another action is still running.
int power_start(struct power *p, const char *action);
int power_busy(struct power *p);
// Short human status ("idle", "restart: waiting for power-off", ...).
void power_status(struct power *p, char *buf, size_t len);
// 1 = server answered recently, 0 = not, -1 = never asked yet.
int power_server_up(struct power *p);
const char *power_url(struct power *p);

#endif
