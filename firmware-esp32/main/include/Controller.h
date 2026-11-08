// Minimal controller interface for the ESP32 port. The Pico firmware's
// Controller also drove PIO-based adapters; here the state always arrives
// over Wi-Fi, so only the report mapping is needed.
#ifndef Controller_h
#define Controller_h

#include "SwitchConsts.h"

class Controller {
 public:
  virtual ~Controller() {}
  virtual void init() = 0;
  virtual void getSwitchReport(SwitchReport *report) = 0;
  virtual void setRumble(bool rumble) = 0;
};

#endif
