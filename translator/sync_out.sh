#!/bin/sh
# copy recompiler/runtime sources into the PC mirror folder (outputs/BSI-Quest/recomp); prints files changed
S=/home/claude/bsi/recomp; D=/mnt/user-data/outputs/BSI-Quest/recomp
cd $S
for f in *.py *.sh *.h *.c linux/*.py linux/*.c linux/*.h linux/*.sh win/*.c win/*.S win/*.py wine/*.sh wine/*.py eh/*.py eh/*.c eh/*.json README.md boot_imports.json; do
  [ -f "$f" ] || continue
  case "$f" in *_v1*|*.pkl) continue;; esac
  mkdir -p "$D/$(dirname $f)"
  if ! cmp -s "$f" "$D/$f"; then cp "$f" "$D/$f"; echo "$f"; fi
done
