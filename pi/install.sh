#!/bin/sh
# Installs the SwitchView stream relay on a Raspberry Pi (Raspberry Pi OS
# Bookworm or newer). Run from this folder on the Pi:  sudo ./install.sh
set -e
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then echo "run with sudo"; exit 1; fi

apt-get update
apt-get install -y ffmpeg python3 v4l-utils alsa-utils iw

install -d /usr/local/lib/switchview
install -m 755 sv_stream_relay.py /usr/local/lib/switchview/
if [ -e /etc/default/switchview-stream ]; then
  echo "keeping existing /etc/default/switchview-stream"
else
  install -m 644 switchview-stream.conf /etc/default/switchview-stream
fi
install -m 644 switchview-stream.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable switchview-stream
systemctl restart switchview-stream

echo
echo "== capture devices =="
v4l2-ctl --list-devices 2>/dev/null || true
echo "== audio inputs =="
arecord -l 2>/dev/null || true
echo
echo "Installed. Settings: /etc/default/switchview-stream (devices are auto-detected)."
echo "On the Mac:  ./app/switchview -S $(hostname).local"
echo "Logs:  journalctl -u switchview-stream -f"
