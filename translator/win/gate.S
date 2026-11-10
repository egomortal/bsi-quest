/* BSI-Quest Windows runtime: transitions between recompiled C and original native x86 code.
   Per-thread context (TCtx) pointer lives in TLS slot g_tls_index (TEB TlsSlots at fs:0xE10).
   TCtx layout (asserted in rt_win.c): CPU at 0 (eax0 ecx4 edx8 ebx12 esp16 ebp20 esi24 edi28, xmm40..167),
   host_esp 244, gate_target 248, gate_ret 252, gate_eflags 256, fpu_n 260, fcw 264, fsw_before 266, fsw_after 268,
   fpu_in 272 (8 doubles), fpu_out 336 (8 doubles) */
    .text
#define CTX(r) movl _g_tls_index, r; movl %fs:0xe10(,r,4), r

    .globl _gate_call
/* void gate_call(TCtx *t, uint32_t target) */
_gate_call:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%esp), %eax
    movl 24(%esp), %edx
    movl %esp, 248(%eax)
    movl %edx, 252(%eax)
    movups 40(%eax), %xmm0
    movups 56(%eax), %xmm1
    movups 72(%eax), %xmm2
    movups 88(%eax), %xmm3
    movups 104(%eax), %xmm4
    movups 120(%eax), %xmm5
    movups 136(%eax), %xmm6
    movups 152(%eax), %xmm7
    fldcw 268(%eax)
    movl 264(%eax), %ecx
1:  testl %ecx, %ecx
    jz 2f
    decl %ecx
    fldl 280(%eax,%ecx,8)
    jmp 1b
2:  fnstsw 270(%eax)
    movl 16(%eax), %ecx
    movl (%ecx), %ebx
    movl %ebx, 256(%eax)
    movl $gate_back, (%ecx)
    pushl 260(%eax)
    popfl
    movl 12(%eax), %ebx
    movl 20(%eax), %ebp
    movl 24(%eax), %esi
    movl 28(%eax), %edi
    movl 4(%eax), %ecx
    movl 8(%eax), %edx
    movl 16(%eax), %esp
    pushl 252(%eax)            /* target on guest stack below esp... no: use ret trick */
    movl 0(%eax), %eax
    ret                        /* jumps to target, esp back to guest esp */
gate_back:
    pushl %eax
    pushl %ecx
    CTX(%ecx)
    movl %ecx, %eax
    popl 4(%eax)
    popl 0(%eax)
    movl %edx, 8(%eax)
    movl %ebx, 12(%eax)
    movl %esp, 16(%eax)
    movl %ebp, 20(%eax)
    movl %esi, 24(%eax)
    movl %edi, 28(%eax)
    pushfl
    popl 260(%eax)
    movups %xmm0, 40(%eax)
    movups %xmm1, 56(%eax)
    movups %xmm2, 72(%eax)
    movups %xmm3, 88(%eax)
    movups %xmm4, 104(%eax)
    movups %xmm5, 120(%eax)
    movups %xmm6, 136(%eax)
    movups %xmm7, 152(%eax)
    fnstcw 268(%eax)
    fnstsw 272(%eax)
    movl 248(%eax), %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

    .globl _bail_native
/* void bail_native(TCtx *t, uint32_t addr) -- one-way */
_bail_native:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    movl %edx, 252(%eax)
    movups 40(%eax), %xmm0
    movups 56(%eax), %xmm1
    movups 72(%eax), %xmm2
    movups 88(%eax), %xmm3
    movups 104(%eax), %xmm4
    movups 120(%eax), %xmm5
    movups 136(%eax), %xmm6
    movups 152(%eax), %xmm7
    fldcw 268(%eax)
    movl 264(%eax), %ecx
1:  testl %ecx, %ecx
    jz 2f
    decl %ecx
    fldl 280(%eax,%ecx,8)
    jmp 1b
2:  movl 16(%eax), %esp
    pushl 260(%eax)
    popfl
    movl 12(%eax), %ebx
    movl 20(%eax), %ebp
    movl 24(%eax), %esi
    movl 28(%eax), %edi
    movl 4(%eax), %ecx
    movl 8(%eax), %edx
    pushl 252(%eax)
    movl 0(%eax), %eax
    ret

    .globl _fpu_store_n
/* void fpu_store_n(TCtx *t, int n): pop n values from the real FPU into t->fpu_out */
_fpu_store_n:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    xorl %edx, %edx
1:  cmpl %ecx, %edx
    jge 2f
    fstpl 344(%eax,%edx,8)
    incl %edx
    jmp 1b
2:  ret
