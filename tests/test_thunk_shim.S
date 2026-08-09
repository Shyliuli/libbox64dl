/* Test shim: prepend $t0 (the thunk's data0) to the argument list. */
  .text
  .align 3
  .globl test_shim
  .type test_shim, @function
test_shim:
  move    $a1, $a0
  move    $a0, $t0
  b       test_dispatch
  .size test_shim, . - test_shim
