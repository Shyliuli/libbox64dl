/* AArch64 test shim: prepend x9 (the thunk's data0) to the argument list.
 *
 * The generated thunk leaves data0 in x9 and data1 in x10 (see
 * src/thunk_aarch64.S); this shim turns a one-argument call into
 * test_dispatch(data0, arg), exactly like the LoongArch test shim does with
 * $t0/$a0/$a1.
 */
  .text
  .p2align 3
  .globl test_shim
  .type test_shim, @function
test_shim:
  mov     x1, x0          /* arg      -> a1 */
  mov     x0, x9          /* data0    -> a0 */
  b       test_dispatch
  .size test_shim, . - test_shim
  .section .note.GNU-stack,"",@progbits
