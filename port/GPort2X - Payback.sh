#!/bin/bash
# GPort2X: runs Payback from your own GP2X SD card image, through your own
# GP2X firmware's shell (the card's Payback.gpe chain), with the PortMaster
# environment. See gport2x/README.md for the files you must supply.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -f "/storage/.config/PortMaster/control.txt" ]; then
  controlfolder="/storage/.config/PortMaster"   # ROCKNIX before PortMaster itself is installed
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/gport2x"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

GPORT2X_ENGINE=jit
source "$GAMEDIR/gport2x.sh"
gport2x_check_files || { pm_finish; exit 1; }

gport2x_hotkeys
pm_platform_helper "$GAMEDIR/${GPORT2X_BIN#./}"
$GPORT2X_BIN "${GPORT2X_ARGS[@]}" \
    --cwd /mnt/sd/Payback /bin/sh /mnt/sd/Payback/Payback.gpe

pm_finish
