#!/bin/sh
# Run the game natively and recompiled under Wine with +relay, compare main-thread DLL call sequences.
# usage: compare_boot.sh [seconds]
T=${1:-90}
export PULSE_SERVER=$(pactl info 2>/dev/null | sed -n "s/^Server String: //p")
export WINEPREFIX=/root/.wine DISPLAY=:99
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &) ; sleep 1
C="/root/.wine/drive_c/users/root/Documents/My Games/BioShock Infinite"
G=/home/claude/bsigame/Binaries/Win32
cp /home/claude/bsi/recomp/bsirecomp.dll $G/
cd $G
wineserver -k 2>/dev/null
rm -rf "$C"; BSI_NATIVE=1 WINEDEBUG=+relay timeout $T wine BioShockInfinite_recomp.exe >/dev/null 2>/tmp/claude-0/relay_native.txt
wineserver -k 2>/dev/null
rm -rf "$C"; WINEDEBUG=+relay timeout $T wine BioShockInfinite_recomp.exe >/dev/null 2>/tmp/claude-0/relay_recomp.txt
wineserver -k 2>/dev/null
tail -3 $G/bsirecomp.log
cd /tmp/claude-0 && python3 /home/claude/bsi/recomp/wine/relaycmp3.py
