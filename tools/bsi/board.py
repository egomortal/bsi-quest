#!/usr/bin/env python3
"""Post one line to chats\\BOARD.md with the time stamp (token-saver: no hand-built printf).

  python board.py "chat 4" "b120 RESULTS: ..."        appends "- 2026-11-02 11:20 EST [chat 4] b120 RESULTS: ..."
  python board.py --tail 5                            prints the last 5 entries, each cut to 300 characters
"""
import argparse, datetime, os, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BOARD = os.path.join(ROOT, 'chats', 'BOARD.md')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('who', nargs='?'); ap.add_argument('text', nargs='?'); ap.add_argument('--tail', type=int)
    a = ap.parse_args()
    if a.tail:
        lines = [l.rstrip('\n') for l in open(BOARD, encoding='utf-8') if l.startswith('- ')]
        for l in lines[-a.tail:]:
            print(l[:300])
        return 0
    if not a.who or not a.text:
        ap.error('who and text needed')
    try:   # the board uses New York time; fall back to local time when tz data is missing (Windows Python without tzdata)
        from zoneinfo import ZoneInfo
        now = datetime.datetime.now(ZoneInfo('America/New_York')); tz = now.strftime('%Z')
    except Exception:
        now = datetime.datetime.now(); tz = time.strftime('%Z') or ''
    with open(BOARD, 'a', encoding='utf-8', newline='\n') as f:
        f.write('- %s %s [%s] %s\n' % (now.strftime('%Y-%m-%d %H:%M'), tz, a.who, a.text))
    print('posted')
    return 0


if __name__ == '__main__':
    sys.exit(main())
