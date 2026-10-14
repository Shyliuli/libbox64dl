/*
 * Runtime-generated host trampolines (AArch64).
 *
 * The template is assembled here rather than hand-encoded, and copied verbatim
 * into an executable page at runtime; only the three trailing data words are
 * patched.  It addresses its own data with a PC-relative `adr`, so a copy works
 * at any address and needs no relocation.
 *
 * Layout of one thunk (48 bytes, 8-byte aligned):
 *   +0   adr  x12, .Ldata     -> x12 = &data0
 *   +4   ldr  x9,  [x12, #0]     x9  = data0
 *   +8   ldr  x10, [x12, #8]     x10 = data1
 *   +12  ldr  x11, [x12, #16]    x11 = target
 *   +16  br   x11
 *   +20  nop                  (pad the code to 24 bytes for the data words)
 *   +24  .quad data0
 *   +32  .quad data1
 *   +40  .quad target
 *
 * Register roles mirror the LoongArch template: the tail call carries data0 in
 * x9 and data1 in x10 (the $t0/$t1 analogues) and jumps through x11 (the $t2
 * analogue).  x9-x12 are caller-saved temporaries (IP0/IP1 are avoided so the
 * linker's call veneers stay free), so clobbering them across the tail call is
 * safe; the argument registers x0-x7 / v0-v7 are left untouched.
 *
 * The template is exactly THUNK_CODE_SZ (24) bytes long: five 4-byte
 * instructions plus the padding nop.
 */

  .text
  .p2align 3

  .globl box64dl_thunk_code
  .globl box64dl_thunk_code_end
  .type box64dl_thunk_code, @function
box64dl_thunk_code:
  adr     x12, .Lbox64dl_thunk_data
  ldr     x9,  [x12, #0]
  ldr     x10, [x12, #8]
  ldr     x11, [x12, #16]
  br      x11
  nop
.Lbox64dl_thunk_data:
box64dl_thunk_code_end:
  .size box64dl_thunk_code, . - box64dl_thunk_code
  .section .note.GNU-stack,"",@progbits
