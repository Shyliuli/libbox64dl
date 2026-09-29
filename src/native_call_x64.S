/* Direct x86_64 CPU call for the debug backend.  The descriptor uses the
 * SysV register-file order rdi,rsi,rdx,rcx,r8,r9 and xmm0..xmm7. */
  .text
  .globl box64_native_call_x64
  .type box64_native_call_x64,@function
box64_native_call_x64:
  push %r12
  push %r13
  push %r14
  mov %rdi,%r12
  mov %rsi,%r13
  mov %rdx,%r14
  mov 12(%r13),%eax
  add $47,%eax
  and $-16,%eax
  sub %rax,%rsp
  mov %rsp,%rdi
  mov 40(%r13),%rsi
  mov 12(%r13),%ecx
  test %ecx,%ecx
  jz .Lnative_gpr
  mov %ecx,%edx
  rep movsb
  mov %edx,%ecx
  add $15,%ecx
  and $-16,%ecx
  sub %edx,%ecx
  jz .Lnative_gpr
  xor %eax,%eax
  rep stosb
.Lnative_gpr:
  testw $0x3f,4(%r13)
  jz .Lnative_fpr
  mov 24(%r13),%r10
  mov 0(%r10),%rdi
  mov 8(%r10),%rsi
  mov 16(%r10),%rdx
  mov 24(%r10),%rcx
  mov 32(%r10),%r8
  mov 40(%r10),%r9
.Lnative_fpr:
  testw $0xff,6(%r13)
  jz .Lnative_call
  mov 32(%r13),%r11
  movdqu 0(%r11),%xmm0
  movdqu 16(%r11),%xmm1
  movdqu 32(%r11),%xmm2
  movdqu 48(%r11),%xmm3
  movdqu 64(%r11),%xmm4
  movdqu 80(%r11),%xmm5
  movdqu 96(%r11),%xmm6
  movdqu 112(%r11),%xmm7
.Lnative_call:
  call *%r12
  mov %rax,0(%r14)
  mov %rdx,8(%r14)
  movdqu %xmm0,16(%r14)
  movdqu %xmm1,32(%r14)
  mov 12(%r13),%eax
  add $47,%eax
  and $-16,%eax
  add %rax,%rsp
  pop %r14
  pop %r13
  pop %r12
  xor %eax,%eax
  ret
  .size box64_native_call_x64,.-box64_native_call_x64
  .section .note.GNU-stack,"",@progbits
