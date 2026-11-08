# SwitchView ESP32-S2 firmware

ESP-IDF port of `firmware/` (Pico 2 W): the board sits in the Switch dock and
presents a USB Pro Controller, driven by the same 33-byte UDP protocol the
desktop app already speaks. Point the app at it with `-H <board-ip>`.

## What is the same as the Pico build

- `proto/switchview_proto.h` wire format: state, text, mouse, ping.
- Pro Controller / keyboard / mouse descriptors from `include/SwitchConsts.h`.
- Report mapping, subcommand replies, dead-man release (500 ms without state).
- `USB_LAYOUT` 0 (controller), 1 (controller + keyboard + mouse, default),
  2 (keyboard only).

## Differences

| Pico | ESP32-S2 |
|---|---|
| CYW43 Wi-Fi, lwIP raw API | `esp_wifi` station + lwIP sockets |
| Static IP set on the netif after join | `esp_netif_set_ip_info` with DHCP client stopped |
| `tusb_init()` from the app | `esp_tinyusb` installs the stack and runs `tud_task()` in its own task |
| Single superloop | one host task: HID reports at 60 Hz + 1 ms UDP poll (`CONFIG_FREERTOS_HZ=1000`) |
| `secrets.cmake` | `idf.py menuconfig` → *SwitchView (ESP32-S2)* |

## Build

```sh
. ~/esp/esp-idf/export.sh
cd firmware-esp32
idf.py set-target esp32s2
idf.py menuconfig        # Wi-Fi SSID/password, static address
idf.py build
```

Note: the machine this was developed on keeps the component-manager cache in
the workspace, so builds ran with
`HOME=$PWD/../.idf-home IDF_TOOLS_PATH=$HOME/.espressif idf.py build`
(the toolchain itself stays in `~/.espressif`).

## Flash

Hold **BOOTSEL** (or the board's boot button) and plug the board in:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The board must enumerate as a *device* on the console's USB port, with the
same USB-C port used for flashing. It is not a hub: keyboard/mouse stay on a
separate device.

## Status

- Compiles against ESP-IDF v5.4.2 (`esp_tinyusb` 1.7.6, 3 HID interfaces).
- **Not yet run on hardware.** First bring-up steps: confirm the console sees a
  wired Pro Controller, then `ping` the static address and send a state packet
  from `tools/pico_ctl.py` or the desktop app.
- Keyboard/mouse on the Flipper is a separate, not-yet-written FAP; until then
  layout 1's keyboard/mouse interfaces are unused.
