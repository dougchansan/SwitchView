# SwitchView: capture viewer + Pro Controller/keyboard command panel (app/),
# Pico 2 W firmware (firmware/), shared wire protocol (proto/).

CFLAGS ?= -O2 -Wall -Wextra
PICO_SDK_PATH ?= $(HOME)/Development/pico-sdk
USB_LAYOUT ?= 1        # 0 controller only, 1 controller + keyboard, 2 keyboard only

UNAME_S := $(shell uname -s)
# Wi-Fi stream from the Raspberry Pi (-S) is decoded with FFmpeg's libraries.
FFMPEG_PKGS = libavformat libavcodec libavutil libswresample
CAPTURE_OBJS = app/capture_src.o app/capture_net.o
APP = app/switchview
COMMON = app/http.o app/frame.o app/power.o app/control.o app/flipper.o app/companion.o

ifeq ($(UNAME_S),Darwin)
  # Video comes from AVFoundation, audio from SDL's CoreAudio backend.
  # OCR is Apple Vision in-process (app/ocr_vision.m); no Tesseract needed.
  PKGS = sdl2 SDL2_ttf lua5.4 $(FFMPEG_PKGS)
  OBJS = app/main.o $(CAPTURE_OBJS) app/capture_macos.o app/panel.o app/link.o app/ocr_vision.o app/script.o $(COMMON)
  LIBS = $(shell pkg-config --libs $(PKGS)) -framework AVFoundation -framework CoreMedia \
         -framework CoreVideo -framework Foundation -framework Vision -framework CoreGraphics -lpthread
  PLATFORM_CFLAGS = -mmacosx-version-min=14.0
else
  PKGS = sdl2 SDL2_ttf alsa tesseract lua5.4 $(FFMPEG_PKGS)
  OBJS = app/main.o $(CAPTURE_OBJS) app/capture.o app/panel.o app/link.o app/ocr.o app/script.o $(COMMON)
  LIBS = $(shell pkg-config --libs $(PKGS)) -lpthread -lm
  PLATFORM_CFLAGS =
endif

PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))

all: $(APP)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

app/%.o: app/%.c app/*.h proto/switchview_proto.h
	$(CC) $(CFLAGS) $(PLATFORM_CFLAGS) -Iproto $(PKG_CFLAGS) -c $< -o $@

# The macOS backend owns its Objective-C objects explicitly, so no ARC.
app/%.o: app/%.m app/*.h proto/switchview_proto.h
	$(CC) $(CFLAGS) $(PLATFORM_CFLAGS) -fno-objc-arc -Iproto $(PKG_CFLAGS) -c $< -o $@

run: $(APP)
	./$(APP)

deps:
ifeq ($(UNAME_S),Darwin)
	brew install sdl2 sdl2_ttf lua ffmpeg
else
	sudo apt install build-essential pkg-config libsdl2-dev libsdl2-ttf-dev \
	                 libasound2-dev libtesseract-dev tesseract-ocr-eng liblua5.4-dev \
	                 libavformat-dev libavcodec-dev libswresample-dev
endif

firmware:
	PICO_SDK_PATH=$(PICO_SDK_PATH) cmake -S firmware -B firmware/build -G Ninja \
	  -DUSB_LAYOUT=$(USB_LAYOUT) -DFETCHCONTENT_FULLY_DISCONNECTED=ON
	PICO_SDK_PATH=$(PICO_SDK_PATH) cmake --build firmware/build
	@echo "UF2: firmware/build/pico_pro_controller.uf2"

# Hold BOOTSEL while plugging the Pico into the PC, then:
flash: firmware
	@vol=$$(ls -d /media/$$USER/RP2350 /run/media/$$USER/RP2350 /Volumes/RP2350 2>/dev/null | head -1); \
	if [ -z "$$vol" ]; then echo "RP2350 boot volume not mounted (hold BOOTSEL while plugging in)"; exit 1; fi; \
	cp firmware/build/pico_pro_controller.uf2 "$$vol/" && echo "flashed to $$vol"

clean:
	rm -f app/*.o $(APP)
	rm -rf firmware/build

.PHONY: all run deps firmware flash clean
