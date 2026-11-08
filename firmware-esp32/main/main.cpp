// SwitchView ESP32-S2 firmware: joins Wi-Fi, listens for the app's 33-byte
// UDP packets, and presents the console with a USB Pro Controller (plus
// keyboard and mouse in the composite layout).
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "tusb.h"

#include "SwitchConsts.h"
#include "SwitchUsb.h"
#include "switchview_proto.h"

static const char *TAG = "switchview";

SwitchUsb usb;

// Wire protocol shared with the PC app (proto/switchview_proto.h).
static_assert(sizeof(sv_packet) == SV_PACKET_SIZE, "packet layout");
enum {
  PAD_A = SV_BTN_A, PAD_B = SV_BTN_B, PAD_X = SV_BTN_X, PAD_Y = SV_BTN_Y,
  PAD_L = SV_BTN_L, PAD_R = SV_BTN_R, PAD_ZL = SV_BTN_ZL, PAD_ZR = SV_BTN_ZR,
  PAD_PLUS = SV_BTN_PLUS, PAD_MINUS = SV_BTN_MINUS, PAD_HOME = SV_BTN_HOME,
  PAD_CAPTURE = SV_BTN_CAPTURE, PAD_L3 = SV_BTN_L3, PAD_R3 = SV_BTN_R3,
};
typedef sv_packet InputPacket;

// Dead-man release: if the PC stops sending state (app crash, Wi-Fi drop),
// everything is released rather than left held. The app refreshes at 50 Hz.
static constexpr uint32_t STATE_TIMEOUT_MS = 500;
static volatile uint32_t g_last_state_ms;

static constexpr unsigned KEYBOARD_ITF = USB_LAYOUT == 2 ? 0 : 1;
static constexpr unsigned MOUSE_ITF = 2;  // layout 1 only

// Pending relative mouse motion from the PC, drained by host_poll.
static volatile int32_t g_mdx, g_mdy, g_mwheel;
static volatile uint8_t g_mbtn, g_mbtn_sent;
static volatile uint32_t g_buttons;
static volatile int16_t g_lx, g_ly, g_rx, g_ry;
static volatile uint8_t g_hat = SV_HAT_CENTER;
static char g_text[256];
static volatile unsigned g_text_head, g_text_tail;

// Typing state machine: the console applies Shift as a mode toggle after any
// key in the same report, so shifted characters go out as separate steps.
static uint8_t g_key_modifier, g_key_code;
static volatile uint8_t g_kbd_leds, g_kbd_leds_valid;
static uint8_t g_key_phase;  // 0 idle; shifted: 1..3, plain: 6
static uint64_t g_phase_at, g_next_key_at;

static int g_sock = -1;
static InputPacket g_echo;
static struct sockaddr_in g_echo_addr;
static volatile bool g_echo_pending;

static void enqueue_text(const uint8_t *text, unsigned length) {
  for (unsigned i = 0; i < length && text[i]; ++i) {
    unsigned next = (g_text_head + 1) % sizeof(g_text);
    if (next == g_text_tail) break;
    g_text[g_text_head] = (char)text[i];
    g_text_head = next;
  }
}

static void handle_packet(const InputPacket &packet, const struct sockaddr_in &from) {
  if (packet.magic[0] != SV_MAGIC0 || packet.magic[1] != SV_MAGIC1) return;
  if (packet.type == SV_PKT_STATE) {
    g_last_state_ms = (uint32_t)(esp_timer_get_time() / 1000);
    g_buttons = packet.buttons;
    g_lx = packet.lx; g_ly = packet.ly; g_rx = packet.rx; g_ry = packet.ry;
    g_hat = packet.hat > 8 ? 8 : packet.hat;
  } else if (packet.type == SV_PKT_TEXT) {
    enqueue_text(packet.text, packet.length > 16 ? 16 : packet.length);
  } else if (packet.type == SV_PKT_MOUSE) {
    g_mdx += packet.lx; g_mdy += packet.ly; g_mwheel += packet.rx;
    g_mbtn = (uint8_t)(packet.buttons & 7);
  } else if (packet.type == SV_PKT_PING) {
    // Answered from the main loop, so the LED byte is fresh.
    g_echo = packet;
    g_echo_addr = from;
    g_echo_pending = true;
  }
}

/* ---------------- Wi-Fi ---------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base; (void)data;
  if (id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "Wi-Fi dropped, reconnecting");
    esp_wifi_connect();
  }
}

static void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();

  // Static address: the app targets a fixed -H, so DHCP would move it.
  esp_netif_dhcpc_stop(netif);
  esp_netif_ip_info_t ip = {};
  ip.ip.addr = esp_ip4addr_aton(CONFIG_SV_STATIC_IP);
  ip.netmask.addr = esp_ip4addr_aton(CONFIG_SV_NETMASK);
  ip.gw.addr = esp_ip4addr_aton(CONFIG_SV_GATEWAY);
  ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  wifi_config_t wc = {};
  strncpy((char *)wc.sta.ssid, CONFIG_SV_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, CONFIG_SV_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_connect();
  ESP_LOGI(TAG, "joining %s, static %s", CONFIG_SV_WIFI_SSID, CONFIG_SV_STATIC_IP);
}

/* ---------------- input ---------------- */

// ASCII to USB HID keyboard usage. Returns false for unsupported characters.
static bool ascii_key(char c, uint8_t *modifier, uint8_t *code) {
  *modifier = 0;
  if (c >= 'a' && c <= 'z') { *code = 4 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { *modifier = 0x02; *code = 4 + (c - 'A'); return true; }
  if (c >= '1' && c <= '9') { *code = 30 + (c - '1'); return true; }
  if (c == '0') { *code = 39; return true; }
  const char *plain = " -=\\[];',./`";
  const uint8_t plain_codes[] = {44, 45, 46, 49, 47, 48, 51, 52, 54, 55, 56, 53};
  for (unsigned i = 0; plain[i]; ++i)
    if (c == plain[i]) { *code = plain_codes[i]; return true; }
  const char *shifted = "!@#$%^&*()_+{}|:\"<>?~";
  const uint8_t shifted_codes[] = {30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
                                   45, 46, 47, 48, 49, 51, 52, 54, 55, 56, 53};
  for (unsigned i = 0; shifted[i]; ++i)
    if (c == shifted[i]) { *modifier = 0x02; *code = shifted_codes[i]; return true; }
  switch (c) {
    case '\n': case '\r': *code = 40; return true;
    case '\t': *code = 43; return true;
    case 27: *code = 41; return true;
    case 8: *code = 42; return true;
    case SV_KEY_CAPSLOCK: *code = 57; return true;
    case SV_KEY_RIGHT: *code = 79; return true;
    case SV_KEY_LEFT: *code = 80; return true;
    case SV_KEY_DOWN: *code = 81; return true;
    case SV_KEY_UP: *code = 82; return true;
  }
  return false;
}

static int8_t take_delta(volatile int32_t *v) {
  int32_t d = *v;
  if (d > 127) d = 127;
  if (d < -127) d = -127;
  *v -= d;
  return (int8_t)d;
}

static void mouse_poll() {
  if (!tud_hid_n_ready(MOUSE_ITF)) return;
  if (!g_mdx && !g_mdy && !g_mwheel && g_mbtn == g_mbtn_sent) return;
  uint8_t report[4] = {g_mbtn, (uint8_t)take_delta(&g_mdx), (uint8_t)take_delta(&g_mdy),
                       (uint8_t)take_delta(&g_mwheel)};
  g_mbtn_sent = g_mbtn;
  tud_hid_n_report(MOUSE_ITF, 0, report, sizeof(report));
}

static void send_keyboard_report(uint8_t modifier, uint8_t code) {
  uint8_t report[8] = {modifier, 0, code, 0, 0, 0, 0, 0};
  if (tud_hid_n_ready(KEYBOARD_ITF)) tud_hid_n_report(KEYBOARD_ITF, 0, report, sizeof(report));
}

static void echo_poll() {
  if (!g_echo_pending || g_sock < 0) return;
  g_echo.text[0] = g_kbd_leds;
  g_echo.text[1] = g_kbd_leds_valid;
  sendto(g_sock, &g_echo, sizeof(g_echo), 0, (struct sockaddr *)&g_echo_addr,
         sizeof(g_echo_addr));
  g_echo_pending = false;
}

// Keyboard/mouse state machine and the dead-man release. Runs once per loop
// iteration, in the same task that pumps TinyUSB.
static void host_poll(uint64_t now) {
  echo_poll();
  if (g_last_state_ms && now - g_last_state_ms > STATE_TIMEOUT_MS) {
    g_buttons = 0; g_lx = g_ly = g_rx = g_ry = 0; g_hat = SV_HAT_CENTER;
    g_mbtn = 0; g_mdx = g_mdy = g_mwheel = 0;
    g_last_state_ms = 0;
  }
  if (USB_LAYOUT == 0) return;
  if (USB_LAYOUT == 1) mouse_poll();
  if (!tud_hid_n_ready(KEYBOARD_ITF)) return;

  if (g_key_phase) {
    if (now < g_phase_at) return;
    switch (g_key_phase) {
      case 1: send_keyboard_report(g_key_modifier, g_key_code); g_phase_at = now + 40; break;
      case 2: send_keyboard_report(g_key_modifier, 0); g_phase_at = now + 25; break;
      case 3: send_keyboard_report(0, 0); g_phase_at = now + 25; break;
      case 6: send_keyboard_report(0, 0); g_phase_at = now + 25; break;
      default: break;
    }
    g_key_phase = (g_key_phase == 3 || g_key_phase == 6) ? 0 : g_key_phase + 1;
    if (!g_key_phase) g_next_key_at = g_phase_at;
    return;
  }

  if (now >= g_next_key_at && g_text_head != g_text_tail) {
    char c = g_text[g_text_tail];
    g_text_tail = (g_text_tail + 1) % sizeof(g_text);
    uint8_t modifier = 0, code = 0;
    if (!ascii_key(c, &modifier, &code)) return;
    g_key_modifier = modifier; g_key_code = code;
    if (modifier) {
      send_keyboard_report(modifier, 0);  // shift down first, on its own
      g_key_phase = 1; g_phase_at = now + 25;
    } else {
      send_keyboard_report(0, code);
      g_key_phase = 6; g_phase_at = now + 40;
    }
  }
}

class HostController : public Controller {
 public:
  void init() override {}
  void setRumble(bool rumble) override { (void)rumble; }
  void getSwitchReport(SwitchReport *report) override {
    memset(report, 0, sizeof(*report));
    report->batteryConnection = 0x81;
    uint32_t b = g_buttons;
    report->buttons[0] = (b & PAD_A ? SWITCH_MASK_A : 0) |
                         (b & PAD_B ? SWITCH_MASK_B : 0) |
                         (b & PAD_X ? SWITCH_MASK_X : 0) |
                         (b & PAD_Y ? SWITCH_MASK_Y : 0) |
                         (b & PAD_R ? SWITCH_MASK_R : 0) |
                         (b & PAD_ZR ? SWITCH_MASK_ZR : 0);
    report->buttons[1] = (b & PAD_PLUS ? SWITCH_MASK_PLUS : 0) |
                         (b & PAD_MINUS ? SWITCH_MASK_MINUS : 0) |
                         (b & PAD_HOME ? SWITCH_MASK_HOME : 0) |
                         (b & PAD_CAPTURE ? SWITCH_MASK_CAPTURE : 0) |
                         (b & PAD_L3 ? SWITCH_MASK_L3 : 0) |
                         (b & PAD_R3 ? SWITCH_MASK_R3 : 0);
    report->buttons[2] = (b & PAD_L ? SWITCH_MASK_L : 0) |
                         (b & PAD_ZL ? SWITCH_MASK_ZL : 0);
    static const uint8_t hats[9] = {
        SWITCH_HAT_UP, SWITCH_HAT_UPRIGHT, SWITCH_HAT_RIGHT, SWITCH_HAT_DOWNRIGHT,
        SWITCH_HAT_DOWN, SWITCH_HAT_DOWNLEFT, SWITCH_HAT_LEFT, SWITCH_HAT_UPLEFT,
        SWITCH_HAT_NOTHING};
    report->buttons[2] |= hats[g_hat > 8 ? 8 : g_hat];
    uint16_t lx = (uint16_t)((g_lx + 32767) * 4095 / 65534);
    uint16_t ly = (uint16_t)((g_ly + 32767) * 4095 / 65534);
    uint16_t rx = (uint16_t)((g_rx + 32767) * 4095 / 65534);
    uint16_t ry = (uint16_t)((g_ry + 32767) * 4095 / 65534);
    report->l[0] = lx & 0xff;
    report->l[1] = ((ly & 0xf) << 4) | (lx >> 8);
    report->l[2] = ly >> 4;
    report->r[0] = rx & 0xff;
    report->r[1] = ((ry & 0xf) << 4) | (rx >> 8);
    report->r[2] = ry >> 4;
  }
};

extern "C" void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                                      hid_report_type_t type, uint8_t const *buffer,
                                      uint16_t size) {
  // Keyboard LED report (Caps Lock etc.), passed back to the PC in ping echoes.
  if (USB_LAYOUT != 0 && itf == KEYBOARD_ITF && type == HID_REPORT_TYPE_OUTPUT && size >= 1) {
    g_kbd_leds = buffer[size - 1];
    g_kbd_leds_valid = 1;
    return;
  }
  if (USB_LAYOUT == 2 || itf != 0 || type != HID_REPORT_TYPE_OUTPUT || !size) return;
  uint8_t report[64] = {};
  if (report_id) {
    if (size > 63) return;
    report[0] = report_id;
    memcpy(report + 1, buffer, size++);
  } else {
    if (size > 64) return;
    memcpy(report, buffer, size);
  }
  if ((report[0] == 0x80 && size >= 2) || (report[0] == 0x01 && size >= 11) ||
      (report[0] == 0x10 && size >= 10))
    hid_report_data_callback(&usb, report[0], report, size);
}

/* ---------------- main ---------------- */

static void host_task(void *) {
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock < 0) {
    ESP_LOGE(TAG, "udp socket: %d", errno);
  } else {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SV_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      ESP_LOGE(TAG, "udp bind: %d", errno);
      close(g_sock);
      g_sock = -1;
    } else {
      int flags = fcntl(g_sock, F_GETFL, 0);
      fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
      ESP_LOGI(TAG, "udp %d", SV_PORT);
    }
  }

  for (;;) {
    usb.task();

    if (g_sock >= 0) {
      InputPacket packet;
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      int n;
      while ((n = recvfrom(g_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&from, &fl)) ==
             (int)sizeof(packet))
        handle_packet(packet, from);
    }

    host_poll((uint64_t)(esp_timer_get_time() / 1000));
    vTaskDelay(1);  // 1 ms tick: keeps the report cadence at 60 Hz
  }
}

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  g_hat = SV_HAT_CENTER;
  wifi_init();

  static HostController controller;
  usb.init(&controller);

  xTaskCreate(host_task, "host", 4096, nullptr, 5, nullptr);
}
