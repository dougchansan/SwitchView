#!/usr/bin/env python3
"""SwitchView Wi-Fi stream relay (runs on the Raspberry Pi).

Captures the capture card with ffmpeg (hardware H.264 + Opus in MPEG-TS) and
sends it over UDP to every SwitchView app that has said hello recently.

Protocol (UDP, default port 39384):
  app -> relay   b"SVSTREAM1 HELLO <session>"   every 500 ms while watching
  app -> relay   b"SVSTREAM1 BYE"               when the app closes
  relay -> app   raw MPEG-TS, 188-byte aligned, <= 1316 bytes per datagram

ffmpeg starts on the first hello and stops after IDLE_STOP seconds without
one, so the encoder only runs while someone is watching. A hello with a new
<session> value restarts ffmpeg, so the decoder that just opened gets SPS/PPS
and a keyframe at once instead of waiting for the next GOP.

Configuration comes from the environment (see switchview-stream.conf).
`--test` streams a synthetic pattern + tone with libx264 instead, for trying
the pipeline without a capture card (also works on a Mac).
"""

import collections
import os
import select
import shlex
import signal
import socket
import subprocess
import sys
import time

TS = 188
CHUNK = TS * 7          # 1316: fits a 1500-byte MTU with IP/UDP headers
CLIENT_TIMEOUT = 3.0    # forget a client this long after its last hello
IDLE_STOP = 15.0        # stop ffmpeg this long after the last client left
MAGIC = b"SVSTREAM1 "
# Pacing. The Pi's keyframes reach ~350 KB; sent back to back at gigabit speed
# they overflow the router's queue toward a slower Wi-Fi client, and the same
# part of every keyframe is lost (a glitch band mid-screen until the next one).
# Sending at a steady rate with a small burst allowance avoids that.


def env(name, default):
    v = os.environ.get(name, "").strip()
    return v if v else default


def log(*a):
    print("relay:", *a, file=sys.stderr, flush=True)


def alsa_card_present(dev):
    """True if an ALSA device like hw:CARD=S,DEV=0 / hw:1,0 names an existing card."""
    if not dev.startswith(("hw:", "plughw:")):
        return True  # 'default', dsnoop, ... let ffmpeg decide
    spec = dev.split(":", 1)[1].split(",")[0]
    card = spec.split("=", 1)[1] if spec.startswith("CARD=") else spec
    if card.isdigit():
        return os.path.exists(f"/proc/asound/card{card}")
    return os.path.exists(f"/proc/asound/{card}")


V4L_SYS = "/sys/class/video4linux"
# Capture formats in order of preference: NV12 goes to the encoder as is,
# YUYV needs no decoding, MJPEG costs a software decode on the Pi.
FORMATS = [("NV12", "nv12"), ("YUYV", "yuyv422"), ("MJPG", "mjpeg")]


def usb_device_of(path):
    """The USB device directory (e.g. .../usb2/2-2) a sysfs device belongs to."""
    p = os.path.realpath(path)
    while p != "/":
        if os.path.exists(os.path.join(p, "idVendor")):
            return p
        p = os.path.dirname(p)
    return None


def find_video_dev():
    """First USB video capture node (a UVC card's index-0 node; index 1 is metadata)."""
    try:
        names = sorted(os.listdir(V4L_SYS), key=lambda n: int(n[5:]) if n[5:].isdigit() else 999)
    except OSError:
        return None
    for n in names:
        if not n.startswith("video"):
            continue
        base = os.path.join(V4L_SYS, n)
        try:
            index = open(os.path.join(base, "index")).read().strip()
        except OSError:
            index = "0"
        if index == "0" and usb_device_of(os.path.join(base, "device")):
            return "/dev/" + n
    return None


def find_audio_dev(vdev):
    """The ALSA capture card on the same USB device as the video node."""
    usb = usb_device_of(os.path.join(V4L_SYS, os.path.basename(vdev), "device"))
    if not usb:
        return None
    try:
        cards = os.listdir("/sys/class/sound")
    except OSError:
        return None
    for c in sorted(cards):
        if not (c.startswith("card") and c[4:].isdigit()):
            continue
        if usb_device_of(os.path.join("/sys/class/sound", c, "device")) == usb:
            try:
                cid = open(f"/proc/asound/{c}/id").read().strip()
                return f"hw:CARD={cid},DEV=0"
            except OSError:
                return f"hw:{c[4:]},0"
    return None


def pick_input_format(vdev, size, fps):
    """Best capture format offering size@fps (falls back to any that offers the size)."""
    try:
        out = subprocess.run(["v4l2-ctl", "-d", vdev, "--list-formats-ext"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    modes, fmt, cur = {}, None, None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("[") and "'" in line:
            fmt = line.split("'")[1]
            modes.setdefault(fmt, {})
        elif line.startswith("Size:") and fmt:
            cur = line.split()[-1]
            modes[fmt].setdefault(cur, set())
        elif line.startswith("Interval:") and fmt and cur and "(" in line:
            modes[fmt][cur].add(round(float(line.split("(")[1].split()[0])))
    for need_fps in (True, False):
        for fourcc, name in FORMATS:
            rates = modes.get(fourcc, {}).get(size)
            if rates is not None and (not need_fps or round(float(fps)) in rates):
                return name
    return None


def ffmpeg_cmd(test):
    fps = env("FPS", "60")
    size = env("SIZE", "1280x720")
    bitrate = env("BITRATE", "6M")
    gop = env("GOP", fps)  # one keyframe a second bounds artefacts after packet loss
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", env("LOGLEVEL", "warning"), "-nostdin"]

    if test:
        cmd += ["-re", "-f", "lavfi", "-i", f"testsrc2=size={size}:rate={fps}",
                "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
        have_audio = True
        vcodec = ["-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                  "-pix_fmt", "yuv420p"]
    else:
        vdev = env("VIDEO_DEV", "auto")
        if vdev == "auto":
            vdev = find_video_dev() or "/dev/video0"
        adev = env("AUDIO_DEV", "auto")
        if adev == "auto":
            adev = find_audio_dev(vdev) or "none"
        ifmt = env("INPUT_FORMAT", "auto")
        if ifmt == "auto":
            ifmt = pick_input_format(vdev, size, fps)
        log(f"capture {vdev} {size}@{fps} {ifmt or 'device default'}, audio {adev}")
        cmd += ["-f", "v4l2", "-thread_queue_size", "8"]
        if ifmt:
            cmd += ["-input_format", ifmt]
        cmd += ["-video_size", size, "-framerate", fps, "-i", vdev]
        have_audio = adev != "none" and alsa_card_present(adev)
        if adev != "none" and not have_audio:
            log(f"audio device {adev} not found; streaming video only "
                "(check `arecord -l` and set AUDIO_DEV)")
        if have_audio:
            cmd += ["-f", "alsa", "-thread_queue_size", "64", "-sample_rate", "48000",
                    "-channels", "2", "-i", adev]
        vcodec = ["-c:v", env("ENCODER", "h264_v4l2m2m")]
        if env("PIX_FMT", ""):
            vcodec += ["-pix_fmt", env("PIX_FMT", "")]

    cmd += ["-map", "0:v"] + (["-map", "1:a"] if have_audio else [])
    cmd += vcodec + ["-b:v", bitrate, "-g", gop, "-bf", "0",
                     "-fps_mode", "passthrough"]
    cmd += shlex.split(env("EXTRA_VIDEO_ARGS", ""))
    if have_audio:
        cmd += ["-c:a", "libopus", "-application", "lowdelay", "-frame_duration", "10",
                "-b:a", env("AUDIO_BITRATE", "128k")]
    cmd += ["-f", "mpegts", "-muxdelay", "0", "-muxpreload", "0", "-flush_packets", "1",
            "pipe:1"]
    return cmd


class Relay:
    def __init__(self, port, test):
        self.test = test
        try:  # dual-stack if the Pi has IPv6, plain IPv4 otherwise
            self.sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            self.sock.bind(("::", port))
        except OSError:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind(("0.0.0.0", port))
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        self.clients = {}       # addr -> (last_seen, session)
        self.proc = None
        self.pending = b""
        self.last_client = 0.0
        self.next_start = 0.0
        self.queue = collections.deque()
        self.pace = float(env("PACE_MBPS", "80")) * 1e6 / 8  # bytes per second
        self.burst = max(CHUNK, int(env("PACE_BURST", "4")) * CHUNK)  # in datagrams
        self.tokens = self.burst
        # Debugging: DUMP_TS=/path writes exactly what is sent, for comparing
        # with what a client received.
        self.dump = open(env("DUMP_TS", ""), "wb") if env("DUMP_TS", "") else None
        self.last_refill = time.monotonic()
        log(f"listening on UDP {port}{' (test pattern)' if test else ''}")

    def start(self):
        cmd = ffmpeg_cmd(self.test)
        log("starting:", " ".join(shlex.quote(c) for c in cmd))
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0)
        self.pending = b""
        self.queue.clear()
        self.started = time.monotonic()

    def stop(self, why):
        if not self.proc:
            return
        log("stopping ffmpeg:", why)
        self.proc.terminate()
        try:
            self.proc.wait(2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def on_datagram(self, data, addr, now):
        if not data.startswith(MAGIC):
            return
        words = data[len(MAGIC):].split()
        if not words:
            return
        if words[0] == b"HELLO":
            session = words[1] if len(words) > 1 else b""
            prev = self.clients.get(addr)
            if prev is None:
                log(f"client {addr[0]} port {addr[1]} joined")
            self.clients[addr] = (now, session)
            if self.proc and (prev is None or prev[1] != session):
                # A fresh decoder needs SPS/PPS + IDR: restart so it gets them now.
                self.stop("new viewer session")
                self.next_start = 0.0
        elif words[0] == b"BYE" and addr in self.clients:
            log(f"client {addr[0]} left")
            del self.clients[addr]

    def forward(self, data):
        buf = self.pending + data
        n = len(buf) - len(buf) % TS
        self.pending = buf[n:]
        if self.dump and self.clients:
            self.dump.write(buf[:n])
            self.dump.flush()
        # Stop at n: the partial packet after it stays in pending for next time.
        for off in range(0, n, CHUNK):
            self.queue.append(buf[off:min(off + CHUNK, n)])
        self.flush()

    def flush(self):
        """Sends queued chunks as the pacing budget allows; returns seconds until
        the next one may go (None when the queue is empty)."""
        now = time.monotonic()
        if self.pace <= 0:
            self.tokens = float("inf")
        else:
            self.tokens = min(self.burst, self.tokens + (now - self.last_refill) * self.pace)
        self.last_refill = now
        while self.queue and self.tokens >= len(self.queue[0]):
            chunk = self.queue.popleft()
            self.tokens -= len(chunk)
            for addr in list(self.clients):
                try:
                    self.sock.sendto(chunk, addr)
                except OSError as e:  # e.g. EHOSTUNREACH while Wi-Fi roams
                    if e.errno not in (11, 35, 55, 64, 65, 105):
                        log(f"send to {addr[0]}: {e}")
        if not self.queue:
            return None
        return (len(self.queue[0]) - self.tokens) / self.pace

    def run(self):
        while True:
            now = time.monotonic()
            for addr, (seen, _) in list(self.clients.items()):
                if now - seen > CLIENT_TIMEOUT:
                    log(f"client {addr[0]} timed out")
                    del self.clients[addr]
            if self.clients:
                self.last_client = now
                if not self.proc and now >= self.next_start:
                    self.start()
            elif self.proc and now - self.last_client > IDLE_STOP:
                self.stop("no viewers")

            wait = self.flush()
            fds = [self.sock] + ([self.proc.stdout] if self.proc else [])
            ready, _, _ = select.select(fds, [], [], 0.25 if wait is None else max(wait, 0.0005))
            for f in ready:
                if f is self.sock:
                    try:
                        data, addr = self.sock.recvfrom(256)
                    except OSError:
                        continue
                    self.on_datagram(data, addr[:2], time.monotonic())
                else:
                    data = os.read(self.proc.stdout.fileno(), 65536)
                    if data:
                        self.forward(data)
                    else:
                        code = self.proc.wait()
                        quick = time.monotonic() - self.started < 5
                        self.proc = None
                        log(f"ffmpeg exited ({code}); retrying in {3 if quick else 1} s")
                        self.next_start = time.monotonic() + (3 if quick else 1)


def main():
    test = "--test" in sys.argv[1:]
    relay = Relay(int(env("PORT", "39384")), test)

    def bye(*_):
        relay.stop("shutting down")
        sys.exit(0)

    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    relay.run()


if __name__ == "__main__":
    main()
