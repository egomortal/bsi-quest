/* BSI-Quest Windows runtime: transitions between recompiled C and original native x86 code.
   CPU layout (asserted in rt_win.c): eax0 ecx4 edx8 ebx12 esp16 ebp20 esi24 edi28 flags32.. xmm40..167 */
    .text
    .globl _gate_call
/* void gate_call(CPU *c, uint32_t target)  -- guest has already pushed its return address at [c->esp] */
_gate_call:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%esp), %eax
    movl 24(%esp), %edx
    movl %esp, _g_host_esp
    movl %eax, _g_gate_cpu
    movl %edx, _g_gate_target
    movups 40(%eax), %xmm0
    movups 56(%eax), %xmm1
    movups 72(%eax), %xmm2
    movups 88(%eax), %xmm3
    movups 104(%eax), %xmm4
    movups 120(%eax), %xmm5
    movups 136(%eax), %xmm6
    movups 152(%eax), %xmm7
    /* x87: push g_fpu_n doubles from g_fpu_in[n-1] .. g_fpu_in[0] so g_fpu_in[0] ends up in st(0) */
    fldcw _g_gate_fcw
    movl _g_fpu_n, %ecx
1:  testl %ecx, %ecx
    jz 2f
    decl %ecx
    fldl _g_fpu_in(,%ecx,8)
    jmp 1b
2:  fnstsw _g_fsw_before
    /* hijack the guest return address */
    movl 16(%eax), %ecx
    movl (%ecx), %ebx
    movl %ebx, _g_gate_ret
    movl $gate_back, (%ecx)
    pushl _g_gate_eflags
    popfl
    movl 12(%eax), %ebx
    movl 20(%eax), %ebp
    movl 24(%eax), %esi
    movl 28(%eax), %edi
    movl 4(%eax), %ecx
    movl 8(%eax), %edx
    movl 16(%eax), %esp
    movl 0(%eax), %eax
    jmp *_g_gate_target
gate_back:
    pushl %eax
    movl _g_gate_cpu, %eax
    popl 0(%eax)
    movl %ecx, 4(%eax)
    movl %edx, 8(%eax)
    movl %ebx, 12(%eax)
    movl %esp, 16(%eax)
    movl %ebp, 20(%eax)
    movl %esi, 24(%eax)
    movl %edi, 28(%eax)
    pushfl
    popl _g_gate_eflags
    movups %xmm0, 40(%eax)
    movups %xmm1, 56(%eax)
    movups %xmm2, 72(%eax)
    movups %xmm3, 88(%eax)
    movups %xmm4, 104(%eax)
    movups %xmm5, 120(%eax)
    movups %xmm6, 136(%eax)
    movups %xmm7, 152(%eax)
    fnstcw _g_gate_fcw
    fnstsw _g_fsw_after
    movl _g_host_esp, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

    .globl _bail_native
/* void bail_native(CPU *c, uint32_t addr) -- one-way: continue executing original code at addr with guest state */
_bail_native:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    movl %edx, _g_gate_target
    movups 40(%eax), %xmm0
    movups 56(%eax), %xmm1
    movups 72(%eax), %xmm2
    movups 88(%eax), %xmm3
    movups 104(%eax), %xmm4
    movups 120(%eax), %xmm5
    movups 136(%eax), %xmm6
    movups 152(%eax), %xmm7
    fldcw _g_gate_fcw
    movl _g_fpu_n, %ecx
1:  testl %ecx, %ecx
    jz 2f
    decl %ecx
    fldl _g_fpu_in(,%ecx,8)
    jmp 1b
2:  movl 16(%eax), %esp
    pushl _g_gate_eflags
    popfl
    movl 12(%eax), %ebx
    movl 20(%eax), %ebp
    movl 24(%eax), %esi
    movl 28(%eax), %edi
    movl 4(%eax), %ecx
    movl 8(%eax), %edx
    movl 0(%eax), %eax
    jmp *_g_gate_target

    .globl _fpu_store_n
/* store n values from real FPU stack (st0 first) into g_fpu_out, popping them */
_fpu_store_n:
    movl 4(%esp), %ecx
    xorl %edx, %edx
1:  cmpl %ecx, %edx
    jge 2f
    fstpl _g_fpu_out(,%edx,8)
    incl %edx
    jmp 1b
2:  ret
