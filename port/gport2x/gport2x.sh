# Shared by the GPort2X launch scripts (sourced with GAMEDIR set).
#
# Your files (never shipped with the port):
#   $GAMEDIR/firmware/   your GP2X firmware dump: the root of the GP2X's
#                        filesystem (bin, lib, usr/gp2x, ...)
#   $GAMEDIR/card/*.img  your Payback SD card image (read-only: GPort2X
#                        never writes to it; saves go to $GAMEDIR/saves)

# Work with a full PortMaster install or only a firmware's control.txt stub
# (ROCKNIX before PortMaster is installed): fill in what may be missing.
DEVICE_ARCH="${DEVICE_ARCH:-$(uname -m)}"
type pm_platform_helper >/dev/null 2>&1 || pm_platform_helper() { :; }
type pm_finish >/dev/null 2>&1 || pm_finish() { :; }
gport2x_hotkeys() {
  # PortMaster's gptokeyb gives the usual exit hotkey; without it GPort2X's
  # own SELECT+START (held a second) still quits
  [ -n "$GPTOKEYB" ] && [ -x "$controlfolder/gptokeyb" ] && $GPTOKEYB "${GPORT2X_BIN#./}" &
}

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# The engine: native (the game's own code on the CPU; 32-bit ARM support in
# the kernel) or jit (the interpreter with its AArch64 JIT). Set by the launcher.
GPORT2X_ENGINE="${GPORT2X_ENGINE:-jit}"
if [ "$GPORT2X_ENGINE" = native ]; then
  GPORT2X_BIN="./gport2x.armhf"
  GPORT2X_ENGINE_ARGS=(--engine native)
else
  GPORT2X_BIN="./gport2x.${DEVICE_ARCH}"
  GPORT2X_ENGINE_ARGS=()
fi

GPORT2X_FIRMWARE="$GAMEDIR/firmware"
GPORT2X_CARD=$(ls "$GAMEDIR"/card/*.img "$GAMEDIR"/card/*.IMG 2>/dev/null | head -n 1)
# scratch (the GP2X's /tmp and the stub's tmpfs) lives in RAM, not on the SD card
GPORT2X_SCRATCH="/tmp/gport2x"
rm -rf "$GPORT2X_SCRATCH" "$GAMEDIR/tmp"
mkdir -p "$GAMEDIR/saves" "$GPORT2X_SCRATCH"

gport2x_check_files() {
  echo "GPort2X ($GPORT2X_ENGINE engine) on ${CFW_NAME:-unknown} (${DEVICE_ARCH}), $(ldd --version 2>&1 | head -n 1)"
  if [ ! -x "$GAMEDIR/${GPORT2X_BIN#./}" ]; then
    echo "GPort2X: no binary ${GPORT2X_BIN#./} for this engine"
    return 1
  fi
  if [ ! -d "$GPORT2X_FIRMWARE/usr/gp2x" ] || [ ! -d "$GPORT2X_FIRMWARE/lib" ]; then
    echo "GPort2X: put your GP2X firmware dump in $GPORT2X_FIRMWARE (it must contain usr/gp2x and lib)"
    return 1
  fi
  if [ -z "$GPORT2X_CARD" ]; then
    echo "GPort2X: put your Payback SD card image (.img) in $GAMEDIR/card/"
    return 1
  fi
  echo "firmware: $GPORT2X_FIRMWARE"
  echo "card:     $GPORT2X_CARD"
  return 0
}

# Fullscreen SDL front end; the card read-only, writes to saves/; real time.
# An array: the card's file name may contain spaces.
GPORT2X_ARGS=("${GPORT2X_ENGINE_ARGS[@]}" --fullscreen --firmware "$GPORT2X_FIRMWARE" --card "$GPORT2X_CARD"
              --saves "$GAMEDIR/saves" --scratch "$GPORT2X_SCRATCH" --log info)
