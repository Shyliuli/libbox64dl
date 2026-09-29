/* Runtime-generated host trampoline template for x86_64. */
  .text
  .p2align 3
  .globl box64dl_thunk_code
  .globl box64dl_thunk_code_end
  .type box64dl_thunk_code, @function
box64dl_thunk_code:
  mov 0x18(%rip), %r11
  mov 0x20(%rip), %r10
  mov 0x28(%rip), %rax
  jmp *%rax
  nop
  nop
  nop
box64dl_thunk_code_end:
  .size box64dl_thunk_code, . - box64dl_thunk_code
  .section .note.GNU-stack,"",@progbits
