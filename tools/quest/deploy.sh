#!/usr/bin/env bash
# Install OpenXvT VR on the connected Quest/PICO, copy the original game data
# once, grant all-files access and launch. Usage:
#   tools/quest/deploy.sh                 # install + launch
#   PUSH_DATA=1 tools/quest/deploy.sh     # also (re)copy the game data
#   RESET_CONFIG=1 tools/quest/deploy.sh  # forget saved OpenXvT settings
#
# Device layout (matches OpenXvTActivity):
#   /sdcard/OpenXvT  <- the Steam/GOG "STAR WARS X-Wing vs TIE Fighter" folder
#                       (the one containing BalanceOfPower)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
PKG=com.bilko.openxvtvr
APK="${APK:-$ROOT/android/app/build/outputs/apk/debug/app-debug.apk}"
GAME_DIR="${XVT_GAME_DIR:-$HOME/.steam/debian-installation/steamapps/common/STAR WARS X-Wing vs TIE Fighter}"
DEVICE_DATA=/sdcard/OpenXvT
MARKER=.openxvt-copied

"$ADB" install -r "$APK"

if [[ "${RESET_CONFIG:-0}" == 1 ]]; then
    echo "== resetting saved OpenXvT settings"
    "$ADB" shell am force-stop "$PKG"
    "$ADB" shell run-as "$PKG" rm -f files/config.yaml
fi

if [[ "${PUSH_DATA:-0}" == 1 ]] || ! "$ADB" shell "test -f $DEVICE_DATA/$MARKER"; then
    echo "== copying game data to $DEVICE_DATA (one time, ~400 MB)"
    "$ADB" shell mkdir -p "$DEVICE_DATA"
    # Windows-only installer/launcher files are not game data.
    for entry in "$GAME_DIR"/*; do
        case "$(basename "$entry")" in
            *.exe|*.EXE|*.dll|*.bat|*.sdb|*.vdf|*.DOC|AUTORUN.INF) continue ;;
        esac
        "$ADB" push "$entry" "$DEVICE_DATA/" >/dev/null
    done
    "$ADB" shell "touch $DEVICE_DATA/$MARKER"
fi

"$ADB" shell appops set --uid "$PKG" MANAGE_EXTERNAL_STORAGE allow
"$ADB" shell am force-stop "$PKG"
"$ADB" logcat -c
[[ "${NO_LAUNCH:-0}" == 1 ]] || "$ADB" shell am start -n "$PKG/.OpenXvTActivity"
