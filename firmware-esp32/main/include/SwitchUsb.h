// USB device side of SwitchView on the ESP32-S2: installs TinyUSB with the
// Switch Pro Controller descriptors and emits one report per 60 Hz interval.
#ifndef SwitchUsb_h
#define SwitchUsb_h

#include "SwitchCommon.h"
#include "SwitchConsts.h"

class SwitchUsb : public SwitchCommon {
 public:
  // Installs the USB device stack. Call once from app_main.
  void init(Controller *controller) override;
  // Pumps TinyUSB and sends the periodic controller report.
  void task();

 private:
  uint8_t *generate_usb_report();
  uint64_t _next_report = 0;
};

#endif
