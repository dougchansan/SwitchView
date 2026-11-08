// Derived from retro-pico-switch by David Pagels (MIT License),
// https://github.com/DavidPagels/retro-pico-switch - see THIRD_PARTY_NOTICES.md.
#include "SwitchUsb.h"

#include <string.h>

#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tusb.h"

extern "C" void host_poll(void);

static tusb_desc_device_t sv_device_descriptor;

static tinyusb_config_t sv_usb_config = {};

uint8_t *SwitchUsb::generate_usb_report() {
  set_empty_report();

  if (_switchRequestReport[0] == 0x80) {
    _report[0] = 0x81;
    _report[1] = _switchRequestReport[1];
    switch (_switchRequestReport[1]) {
      case 0x01:
        _report[3] = 0x03;
        for (int i = 0; i < 6; i++) {
          _report[4 + i] = _addr[5 - i];
        }
        break;
      case 0x02:
      case 0x03:
        break;
      default:
        _report[0] = 0x30;
        _controller->getSwitchReport(&_switchReport);
        memcpy(_report + 2, (uint8_t *)&_switchReport, sizeof(SwitchReport));
        break;
    }
    if (_switchRequestReport[0] > 0x00) {
      set_empty_switch_request_report();
    }
    return _report;
  } else {
    generate_report();
    if (_switchRequestReport[0] > 0x00) {
      set_empty_switch_request_report();
    }
    // _report is a bluetooth report starting with 0xA1, which usb skips
    return _report + 1;
  }
}

void SwitchUsb::init(Controller *controller) {
  _controller = controller;
  _switchReport.batteryConnection = 0x81;
  _switchRequestReport[0] = 0x80;
  _switchRequestReport[1] = 0x01;
  uint8_t newAddr[] = {0x7c,
                       0xbb,
                       0x8a,
                       (uint8_t)(esp_random() % 0xff),
                       (uint8_t)(esp_random() % 0xff),
                       (uint8_t)(esp_random() % 0xff)};
  memcpy(_addr, newAddr, 6);

  // ESP-IDF takes the descriptors as data rather than through tud_descriptor_*
  // callbacks; copy the packed controller descriptor into its struct form.
  static_assert(sizeof(switch_usb_device_descriptor) == sizeof(tusb_desc_device_t),
                "device descriptor size");
  memcpy(&sv_device_descriptor, switch_usb_device_descriptor, sizeof(sv_device_descriptor));
#if USB_LAYOUT == 2
  sv_device_descriptor.idVendor = 0xcafe;
  sv_device_descriptor.idProduct = 0x4002;
#endif

  // Index 0 is the language ID (as the stack expects), then the strings the
  // device descriptor points at.
  sv_usb_config.device_descriptor = &sv_device_descriptor;
  sv_usb_config.string_descriptor = (const char **)switch_usb_string_descriptors;
  sv_usb_config.string_descriptor_count =
      sizeof(switch_usb_string_descriptors) / sizeof(switch_usb_string_descriptors[0]);
  sv_usb_config.configuration_descriptor = switch_usb_configuration_descriptor;
  sv_usb_config.external_phy = false;
  ESP_ERROR_CHECK(tinyusb_driver_install(&sv_usb_config));
}

// esp_tinyusb runs tud_task() in its own task, so this only emits reports.
void SwitchUsb::task() {
  uint64_t now = esp_timer_get_time();
  if (USB_LAYOUT != 2 && tud_mounted() && tud_hid_ready() && now >= _next_report) {
    _next_report = now + 16667;
    uint8_t *report = generate_usb_report();
    tud_hid_report(0, report, 64);
  }
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
#if USB_LAYOUT == 2
  return itf == 0 ? keyboard_usb_report_descriptor : nullptr;
#elif USB_LAYOUT == 1
  return itf == 1 ? keyboard_usb_report_descriptor
       : itf == 2 ? mouse_usb_report_descriptor : switch_usb_report_descriptor;
#else
  return itf == 0 ? switch_usb_report_descriptor : nullptr;
#endif
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)itf; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
  return 0;  // Unsupported GET_REPORT: never return uninitialized bytes.
}
