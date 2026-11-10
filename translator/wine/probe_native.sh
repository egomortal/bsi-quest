#!/bin/sh
T=${1:-120}
export WINEPREFIX=/root/.wine DISPLAY=:99 PULSE_SERVER=$(pactl info 2>/dev/null | sed -n "s/^Server String: //p")
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &) ; sleep 1
C="/root/.wine/drive_c/users/root/Documents/My Games/BioShock Infinite"
cd /home/claude/bsigame/Binaries/Win32; wineserver -k 2>/dev/null; rm -rf "$C"
BSI_NATIVE=1 WINEDEBUG=+relay timeout $T wine BioShockInfinite_recomp.exe >/dev/null 2>/tmp/claude-0/relay_native.txt
wineserver -k 2>/dev/null
python3 - <<'PY'
import re
lines=open('/tmp/claude-0/relay_native.txt',errors='replace').read().split('\n')
pend={}; miss=[]
for l in lines:
    m=re.match(r'^(\w{4}):Call KERNEL32\.(CreateFileW|GetFileAttributesW|FindFirstFileW)\([0-9a-f]+ L"([^"]*)"',l)
    if m: pend[m.group(1)]=m.group(3); continue
    m=re.match(r'^(\w{4}):Ret  KERNEL32\.(CreateFileW|GetFileAttributesW|FindFirstFileW)\(\) retval=(\w+)',l)
    if m and m.group(3)=='ffffffff' and m.group(1) in pend: miss.append(pend.pop(m.group(1)))
seen=[]
for x in miss:
    if 'bsigame' in x and x not in seen: seen.append(x)
print('missing files:'); [print('  ',x) for x in seen[-25:]]
fat=re.findall(r'L"([^"]*(?:failure|Fatal|Assertion|Error)[^"]{0,200})',open('/tmp/claude-0/relay_native.txt',errors='replace').read())
print('fatal:',[f[:250] for f in dict.fromkeys(fat)][-3:])
PY
