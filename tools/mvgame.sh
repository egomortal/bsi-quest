#!/system/bin/sh
# Move the game from /sdcard/BSI (FUSE) into the app's own folder, one file at a time (copy, verify size, delete original).
# Safe to re-run: files already moved are skipped. Progress: /sdcard/BSI/mvgame.log
SRC="/sdcard/BSI/BioShock Infinite"
DST=/sdcard/Android/data/com.bsiquest.game/files/game
LOG=/sdcard/BSI/mvgame.log
cd "$SRC" || { echo "no source" >> $LOG; exit 1; }
echo "start $(date)" >> $LOG
find . -type d | while read d; do mkdir -p "$DST/$d"; done
find . -type f | while read f; do
  s1=$(stat -c %s "$f")
  if [ -f "$DST/$f" ] && [ "$(stat -c %s "$DST/$f")" = "$s1" ]; then rm "$f"; continue; fi
  rm -f "$DST/$f.part"
  if cp "$f" "$DST/$f.part" && [ "$(stat -c %s "$DST/$f.part")" = "$s1" ] && mv "$DST/$f.part" "$DST/$f"; then rm "$f"; echo "ok $s1 $f" >> $LOG
  else echo "FAIL $f" >> $LOG; rm -f "$DST/$f.part"; exit 1; fi
done
echo "ALLDONE $(date)" >> $LOG
