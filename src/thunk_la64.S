/*
 * Runtime-generated host trampolines (LoongArch64).
 *
 * The template is assembled here rather than hand-encoded, and copied verbatim
 * into an executable page at runtime; only the three trailing data words are
 * patched. It addresses its own data with pcaddi, so a copy works at any
 * address and needs no relocation.
 *
 * Layout of one thunk (48 bytes, 8-byte aligned):
 *   +0   pcaddi $t2, 6      -> t2 = &data0
 *   +4   ld.d   $t0, $t2, 0    t0 = data0
 *   +8   ld.d   $t1, $t2, 8    t1 = data1
 *   +12  ld.d   $t2, $t2, 16   t2 = target
 *   +16  jr     $t2
 *   +20  nop                (pad to 8-byte alignment for the data words)
 *   +24  .quad data0
 *   +32  .quad data1
 *   +40  .quad target
 *
 * $t0/$t1/$t2 are temporaries in the LP64D ABI, so clobbering them across the
 * tail call is safe; argument registers are untouched.
 */

  .text
  .align 3

  .globl box64dl_thunk_code
  .globl box64dl_thunk_code_end
  .type box64dl_thunk_code, @function
box64dl_thunk_code:
  pcaddi  $t2, 6
  ld.d    $t0, $t2, 0
  ld.d    $t1, $t2, 8
  ld.d    $t2, $t2, 16
  jr      $t2
  nop
box64dl_thunk_code_end:
  .size box64dl_thunk_code, . - box64dl_thunk_code
