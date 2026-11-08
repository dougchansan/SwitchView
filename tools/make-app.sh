#!/bin/sh
# Builds SwitchView.app, a launcher for this checkout, and installs it.
#   tools/make-app.sh [destination]      (default /Applications/SwitchView.app)
# The bundle runs tools/launch.sh from this repo, so rig.env, scripts/ and the
# build stay here; re-run this only if the repo moves. Output goes to
# $TMPDIR/switchview.log. macOS asks for camera / Bluetooth / local-network
# access on behalf of SwitchView the first time each is used.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="${1:-/Applications/SwitchView.app}"
case "$DEST" in *.app) ;; *) echo "destination must end in .app" >&2; exit 1 ;; esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
APP="$TMP/SwitchView.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/app/icon/switchview.icns" "$APP/Contents/Resources/switchview.icns"

cat >"$APP/Contents/MacOS/SwitchView" <<EOF
#!/bin/sh
exec '$ROOT/tools/launch.sh' "\$@"
EOF
chmod +x "$APP/Contents/MacOS/SwitchView"

cat >"$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>SwitchView</string>
  <key>CFBundleDisplayName</key><string>SwitchView</string>
  <key>CFBundleIdentifier</key><string>local.switchview.launcher</string>
  <key>CFBundleExecutable</key><string>SwitchView</string>
  <key>CFBundleIconFile</key><string>switchview</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSCameraUsageDescription</key><string>SwitchView shows the console through the capture card.</string>
  <key>NSMicrophoneUsageDescription</key><string>SwitchView plays the console's audio from the capture card.</string>
  <key>NSBluetoothAlwaysUsageDescription</key><string>SwitchView's fingerbot server presses the console's power button over Bluetooth.</string>
  <key>NSLocalNetworkUsageDescription</key><string>SwitchView talks to the controller board, the Flipper bridge and the Raspberry Pi video relay on your network.</string>
</dict>
</plist>
EOF

rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
cp -R "$APP" "$DEST"
codesign --force --sign - "$DEST" >/dev/null 2>&1 || true   # ad-hoc: keeps TCC grants stable
touch "$DEST"   # refresh Finder / Dock icon caches
echo "installed $DEST -> $ROOT"
