/*
 * Register probe for tests/test_call_plan.c.
 *
 * void probe_guest(void* out)
 *
 * Stores the incoming integer argument registers and the first two SSE
 * registers verbatim into `out`, so the test can see exactly which registers
 * the engine seam wrote and that an SSE value is carried as a full 16 bytes.
 *
 * out layout (all 8 bytes, little-endian):
 *   [ 0] rdi   [ 8] rsi   [16] rdx   [24] rcx   [32] r8   [40] r9
 *   [48] xmm0 bytes 0..15
 *   [64] xmm1 bytes 0..15
 */

    .text
    .globl probe_guest
    .type probe_guest, @function
probe_guest:
    movq    %rdi, %r11
    movq    %rdi, 0(%r11)
    movq    %rsi, 8(%r11)
    movq    %rdx, 16(%r11)
    movq    %rcx, 24(%r11)
    movq    %r8, 32(%r11)
    movq    %r9, 40(%r11)
    movups  %xmm0, 48(%r11)
    movups  %xmm1, 64(%r11)
    ret
    .size probe_guest, .-probe_guest

/*
 * uint64_t align_probe(void)
 * Low 4 bits of RSP at function entry. The SysV (and Win64) ABI guarantees
 * (rsp + 8) % 16 == 0 when control reaches an entry point, i.e. rsp % 16 == 8;
 * callers' movaps-on-locals code assumes it. This reports the value directly.
 */
    .globl align_probe
    .type align_probe, @function
align_probe:
    movq    %rsp, %rax
    andq    $15, %rax
    ret
    .size align_probe, .-align_probe

/*
 * void align_probe_sse(uint64_t x)
 * A genuinely aligned SSE access, in the shape a compiler emits for a 16-byte
 * aligned local: at entry rsp % 16 == 8, so `sub $24` makes rsp 16-aligned and
 * `movaps %xmm0, (%rsp)` faults unless that ABI parity held. (Writing to
 * 8(%rsp) instead would target the caller's home slot, which at the outermost
 * host->guest call is above the top of the emu stack — not an alignment test.)
 */
    .globl align_probe_sse
    .type align_probe_sse, @function
align_probe_sse:
    subq    $24, %rsp
    movaps  %xmm0, (%rsp)
    addq    $24, %rsp
    ret
    .size align_probe_sse, .-align_probe_sse

/* The descriptor stack blob is copied at guest entry rsp+8.  These probes
 * record the first eight bytes of the rounded, zero-filled tail after 24 and
 * 40 byte blobs respectively. */
    .globl stack_tail24
    .type stack_tail24, @function
stack_tail24:
    movq    32(%rsp), %rax
    movq    %rax, (%rdi)
    ret
    .size stack_tail24, .-stack_tail24

    .globl stack_tail40
    .type stack_tail40, @function
stack_tail40:
    movq    48(%rsp), %rax
    movq    %rax, (%rdi)
    ret
    .size stack_tail40, .-stack_tail40

/*
 * uint64_t regs_probe(void)
 *
 * A legal SysV64 function used to pin the known-ABI call contract: it clobbers
 * every volatile register (RDI, RSI, RDX, RCX, R8-R11) with a distinct sentinel
 * and returns a marker in RAX, while preserving RBX/RBP/R12-R15 by saving and
 * restoring them itself. It must NOT be claimed to preserve the volatile set;
 * the engine's ABI mode is allowed to leave those with the callee.
 */
    .globl regs_probe
    .type regs_probe, @function
regs_probe:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movabs  $0xb0b0b0b0b0b0b0b0, %rbx
    movabs  $0xb1b1b1b1b1b1b1b1, %rbp
    movabs  $0xc2c2c2c2c2c2c2c2, %r12
    movabs  $0xd3d3d3d3d3d3d3d3, %r13
    movabs  $0xe4e4e4e4e4e4e4e4, %r14
    movabs  $0xf5f5f5f5f5f5f5f5, %r15
    movabs  $0x1111111111111111, %rdi
    movabs  $0x2222222222222222, %rsi
    movabs  $0x3333333333333333, %rdx
    movabs  $0x4444444444444444, %rcx
    movabs  $0x5555555555555555, %r8
    movabs  $0x6666666666666666, %r9
    movabs  $0x7777777777777777, %r10
    movabs  $0x8888888888888888, %r11
    movabs  $0xabcdef0123456789, %rax
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret
    .size regs_probe, .-regs_probe

/*
 * uint64_t regs_around_callback(uint64_t (*f)(uint64_t), uint64_t x)
 *
 * Keeps a sentinel in R12 across a call to the host bridge `f` (which re-enters
 * the engine through the unified scalar call entry). Returns 1 when R12 came back unchanged, i.e.
 * the nested known-ABI call left the outer call's non-volatile register alone.
 * Three pushes leave RSP 16-aligned at the call, so the bridge sees the ABI
 * parity.
 */
    .globl regs_around_callback
    .type regs_around_callback, @function
regs_around_callback:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    movq    %rdi, %rbx
    movq    %rsi, %rdi
    movabs  $0x1234567890abcdef, %r12
    call    *%rbx
    xorl    %eax, %eax
    movabs  $0x1234567890abcdef, %r10
    cmpq    %r10, %r12
    sete    %al
    movzbl  %al, %eax
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret
    .size regs_around_callback, .-regs_around_callback

/* Mark the stack non-executable (no crt files under -nostdlib emit this). */
    .section .note.GNU-stack,"",@progbits
