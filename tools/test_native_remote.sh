#!/bin/sh
# Runs `make test-native` on an arm64 machine over ssh (the emulated test VM,
# or any AArch64 Linux with 32-bit compat and arm-linux-gnueabihf-gcc): syncs
# this tree to ~/gport2x there (no build/, .git/ or dist/) and runs the target
# after sourcing ~/.gport2x-test-env there if it exists, which exports that
# machine's GPORT2X_FIRMWARE_DIR, GPORT2X_CARD_IMAGE and GPORT2X_GAME_ELF
# (the user's own data never leaves it).
#   tools/test_native_remote.sh [SSH OPTIONS...] USER@HOST
# e.g. tools/test_native_remote.sh -i ~/vm/vm_key -p 2222 ubuntu@localhost
set -eu
[ $# -ge 1 ] || { echo "usage: $0 [ssh options] user@host"; exit 2; }
here=$(cd "$(dirname "$0")/.." && pwd)
opts=""
while [ $# -gt 1 ]; do opts="$opts $1"; shift; done
host=$1
rsync -az --delete --exclude build --exclude .git --exclude dist -e "ssh$opts" "$here/" "$host:gport2x/"
# shellcheck disable=SC2086
ssh $opts "$host" 'cd gport2x && if [ -f ~/.gport2x-test-env ]; then . ~/.gport2x-test-env; fi && make test-native'
