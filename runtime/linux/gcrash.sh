#!/bin/sh
# run bsi_linux under gdb (from the test game dir), print host bt, faulting insn and guest registers
B=${B:-/home/claude/bsi/recomp/bsi_linux}
C=$(printf "0x%x" $((0x555555554000 + 0x$(nm $B | grep " cpu.0$" | cut -d' ' -f1))))
cd /home/claude/bsigame/Binaries/Win32
timeout ${T:-600} gdb -q -batch -ex "handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint" -ex "set environment BSI_NOTRACE=${NOTRACE:-1}" -ex run -ex "bt ${N:-12}" -ex "x/2i \$pc" \
  -ex "printf \"guest(main) eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\\n\", *(unsigned*)$C,*(unsigned*)($C+4),*(unsigned*)($C+8),*(unsigned*)($C+12),*(unsigned*)($C+16),*(unsigned*)($C+20),*(unsigned*)($C+24),*(unsigned*)($C+28)" \
  -ex "info registers rax rbx rcx rdx rsi rdi" $B 2>&1 | grep -E "^#|^=>|^guest|^r[a-z]x|^r[sd]i|signal"
