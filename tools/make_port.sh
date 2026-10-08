#!/bin/sh
# Builds the PortMaster package: dist/gport2x.zip with the two launch
# scripts and ports/gport2x/ (scripts, port.json, README, the binary).
#   tools/make_port.sh BINARY_AARCH64 [BINARY_ARMHF_NATIVE]
# BINARY_AARCH64 is an aarch64 build with the SDL2 front end (built on an
# arm64 system with libsdl2-dev: make BUILD=build/sdl build/sdl/gport2x);
# BINARY_ARMHF_NATIVE the native engine with SDL2 (make armhf-native-sdl).
# No game or firmware data is ever packaged (LEGAL.md).
set -eu
bin=${1:?usage: tools/make_port.sh BINARY_AARCH64 [BINARY_ARMHF_NATIVE]}
native=${2:-}
here=$(cd "$(dirname "$0")/.." && pwd)
file "$bin" | grep -q "ARM aarch64" || { echo "$bin is not an aarch64 binary"; exit 1; }
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp "$here/port/GPort2X - Payback.sh" "$here/port/GPort2X - GP2X Menu.sh" "$stage/"
if [ -n "$native" ]; then
  file "$native" | grep -q "ARM, EABI5" || { echo "$native is not an armhf binary"; exit 1; }
  cp "$here/port/GPort2X Native - Payback.sh" "$here/port/GPort2X Native - GP2X Menu.sh" "$stage/"
fi
mkdir -p "$stage/gport2x/firmware" "$stage/gport2x/card"
cp "$here/port/gport2x/gport2x.sh" "$here/port/gport2x/port.json" "$here/port/gport2x/README.md" "$stage/gport2x/"
cp "$bin" "$stage/gport2x/gport2x.aarch64"
[ -n "$native" ] && cp "$native" "$stage/gport2x/gport2x.armhf"
chmod +x "$stage"/gport2x/gport2x.* "$stage"/*.sh
echo "put your GP2X firmware dump here (bin, lib, usr/gp2x, ...)" > "$stage/gport2x/firmware/PUT_FIRMWARE_HERE.txt"
echo "put your Payback SD card image (.img) here" > "$stage/gport2x/card/PUT_CARD_IMAGE_HERE.txt"
mkdir -p "$here/dist"
rm -f "$here/dist/gport2x.zip"
(cd "$stage" && zip -qr "$here/dist/gport2x.zip" .)
echo "dist/gport2x.zip:"
unzip -l "$here/dist/gport2x.zip"
