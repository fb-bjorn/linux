# Kernel SIMD Vectorization PoC

## Background

In July 2011, Andy Lutomirski posted an RFC patch to remove redundant
`clts`/`stts` instructions from the x86 context switch path. During the
review, Ingo Molnar made a broader observation: with XSAVE/XRSTOR being
so cheap on modern CPUs (~24ns save, ~40ns restore on Sandy Bridge),
the kernel could abandon the `-mno-sse` compilation restriction
entirely. If the compiler were allowed to use SSE/AVX registers via
`-ftree-vectorize`, GCC could auto-vectorize loops and use wider
registers for data movement throughout the kernel — at the cost of
saving/restoring FPU state at every kernel entry point.

Thread: https://lore.kernel.org/all/CAObL_7GTfH58gfgppg=KzKf8ztShA4LRFoZ0HyDXZ9M5Dii2BA@mail.gmail.com/T/

Andy noted that "the kernel doesn't really use floating-point math" and
that enabling `-ftree-vectorize` was "a little scary." Ingo agreed but
suggested it was worth trying: "as long as it boots it would allow some
baseline figures to be estimated."

This was never attempted. The eager FPU switching work eventually landed
(default in v4.9, lazy FPU fully removed in v4.18), but the kernel
continued to be compiled with `-mno-sse -mno-mmx -mno-sse2` and
explicit `kernel_fpu_begin()`/`end()` bracketing for the few places
(crypto, RAID, checksums) that need SIMD.

This PoC implements what Ingo proposed: compile the kernel with
`-march=native -ftree-vectorize`, and add FPU state save/restore at all
kernel entry points so that compiler-generated SIMD instructions don't
corrupt userspace (or interrupted kernel) FPU state.

## What the kernel does today

1. **Compilation**: The kernel is compiled with `-mno-sse -mno-mmx
   -mno-sse2 -mno-3dnow -mno-avx -mno-sse4a` (arch/x86/Makefile:80),
   forbidding GCC from generating any vector instructions.

2. **Explicit opt-in**: Code that needs SIMD (crypto, RAID XOR, etc.)
   is compiled from hand-written assembly or with per-file
   `CC_FLAGS_FPU` overrides, and must bracket execution with
   `kernel_fpu_begin()`/`kernel_fpu_end()`.

3. **`kernel_fpu_begin()`** saves the current userspace FPU state,
   disables preemption, sets `TIF_NEED_FPU_LOAD`, and initializes
   MXCSR.

4. **Context switch**: `switch_fpu()` eagerly saves user FPU state via
   XSAVE on every context switch. Restore is deferred (lazy) until
   return to userspace, checked via `TIF_NEED_FPU_LOAD`.

5. **No FPU save at syscall/interrupt entry**: Since kernel code never
   touches vector registers (by compilation constraint), there is no
   need to save FPU state when entering the kernel.

## What this PoC changes

### Part 1: Compiler flags

Remove `-mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx -mno-sse4a`
from `arch/x86/Makefile` and replace with `-march=native
-ftree-vectorize`. This lets GCC use SSE/AVX registers in any kernel
function.

### Part 2: FPU save/restore at all entry points

Since any kernel function may now clobber vector registers, FPU state
must be saved at every boundary where kernel code begins executing:

| Entry type                     | Save what?           | Where?                              |
|--------------------------------|----------------------|-------------------------------------|
| Syscall from user              | User FPU state       | `arch_enter_from_user_mode()`       |
| Interrupt/exception from user  | User FPU state       | `arch_enter_from_user_mode()`       |
| Interrupt/exception from kernel| Kernel FPU regs      | `DEFINE_IDTENTRY*` macros           |
| NMI                            | Whatever is in regs  | `exc_nmi()` in nmi.c                |

For the kernel-mode interrupt case, per-CPU nesting save areas are
used (3 levels: task → hardirq → NMI), each containing a full
`union fpregs_state` (PAGE_SIZE).

### Part 3: Simplify `kernel_fpu_begin()`/`end()`

With FPU state managed at entry/exit boundaries, `kernel_fpu_begin()`
becomes a simple MXCSR initialization (no preemption disable, no
state save needed), and `kernel_fpu_end()` becomes a no-op.

### Part 4: Early SSE enable in `head_64.S`

GCC emits SSE instructions in the very early boot code
(`__pi___startup_64` — page table setup), before the FPU subsystem is
initialized. The PoC adds CR4.OSFXSR, CR4.OSXMMEXCPT setup and
CR0.TS/CR0.EM clearing at the top of `startup_64`, before any C code
runs.

## Files modified

| File | Lines | Change |
|------|-------|--------|
| `arch/x86/Makefile` | +3/-3 | Replace `-mno-sse` flags with `-march=native -ftree-vectorize` |
| `include/linux/fpu.h` | +3/-3 | Disable `_LINUX_FPU_COMPILATION_UNIT` error guard |
| `arch/x86/include/asm/fpu/api.h` | +5 | Declare new entry/exit FPU helpers |
| `arch/x86/kernel/fpu/core.c` | +125/-18 | Per-CPU save areas, entry/exit helpers, simplified `kernel_fpu_begin`/`end` |
| `arch/x86/include/asm/entry-common.h` | +8 | Save user FPU in `arch_enter_from_user_mode()` |
| `arch/x86/include/asm/idtentry.h` | +26 | FPU save/restore in `DEFINE_IDTENTRY`, `DEFINE_IDTENTRY_ERRORCODE`, `DEFINE_IDTENTRY_IRQ`, `DEFINE_IDTENTRY_SYSVEC`, `DEFINE_IDTENTRY_SYSVEC_SIMPLE` |
| `arch/x86/kernel/nmi.c` | +10 | FPU save/restore around NMI handler |
| `arch/x86/kernel/head_64.S` | +25 | Enable SSE in CR4/CR0 at very early boot |

## Vectorization analysis

Built the kernel twice — once with default flags, once with
`-march=native -ftree-vectorize` — and compared with `objdump -d`.

| Metric | Baseline (`-mno-sse`) | Vectorized | Delta |
|--------|-----------------------|------------|-------|
| Total vector instructions | ~5,373 | ~104,262 | +98,889 |
| Functions with SIMD | ~40 (hand-written) | 12,084+ | +12,044 new |

### What GCC generates

The vast majority of new instructions are **data movement**, not loop
vectorization:

| Instruction | Count | Purpose |
|-------------|-------|---------|
| `movq` | 38,416 | 64-bit moves via SSE registers |
| `movaps`/`movups` | 37,391 | 128-bit struct/memory copies |
| `pxor` | 10,705 | Register zeroing (`memset(0)`) |
| `movdqa` | 6,181 | 128-bit aligned integer moves |

GCC uses SSE/AVX registers primarily as **wider data buses** — copying
structs, initializing memory, zeroing buffers. Genuine arithmetic
vectorization is relatively rare in kernel code.

### Top new beneficiaries

| Function | New SIMD insns | What GCC is doing |
|----------|---------------|-------------------|
| `drm_fb_xrgb8888_to_rgb565be_line` | 290 | Pixel format conversion (genuine vectorization) |
| `HUF_fillDTableX2ForWeight` (zstd) | 343 | Table initialization |
| `intel_pmu_init` | 313 | Large struct initialization |
| `check_alu_op` (BPF verifier) | 115 | Struct copies |
| `copy_process` | 53 | Task struct initialization |
| `load_elf_binary` | 71 | ELF loading struct ops |
| `fib_multipath_hash` | 88 | Hash computations |

## Testing

### Boot test (QEMU/KVM)

Booted with a busybox initramfs on QEMU with KVM acceleration:

```
qemu-system-x86_64 \
  -kernel arch/x86/boot/bzImage \
  -initrd /tmp/initramfs.cpio.gz \
  -append "console=ttyS0 nokaslr rdinit=/init" \
  -nographic -enable-kvm -m 2G -no-reboot
```

Results:
- Full boot through ACPI, SMP, PCI, USB, SCSI, networking init
- `atomic64_test: passed for x86-64 platform with CX8 and with SSE`
- Initramfs mounted, `/init` script executed
- `uname -a` reports `7.0.0-rc4+ SMP PREEMPT_DYNAMIC`
- Userspace integer math correct (`1234 * 5678 = 7006652`)
- Clean shutdown via sysrq

### SMP test (4 CPUs)

Booted with `-smp 4` to verify secondary CPU bring-up with SSE enabled:

```
qemu-system-x86_64 \
  -kernel arch/x86/boot/bzImage \
  -initrd /tmp/initramfs.cpio.gz \
  -append "console=ttyS0 nokaslr rdinit=/init" \
  -nographic -enable-kvm -m 2G -smp 4 -no-reboot
```

Results:
- `smp: Brought up 1 node, 4 CPUs`
- `smpboot: Total of 4 processors activated (21504.00 BogoMIPS)`
- All 4 CPUs booted through `common_startup_64` with SSE enabled
- `atomic64_test: passed for x86-64 platform with CX8 and with SSE`
- Init script ran successfully, clean shutdown

### Early boot crash diagnosis and fix

Initial boot attempts crashed with `#UD` (undefined opcode) at physical
address `0x04493e39` — the `__pi___startup_64` page table setup code.
GCC generated `movups` instructions for copying page table entries, but
CR4.OSFXSR was not set yet (CR4=0x20). The fix: enable SSE in CR4/CR0
at the very top of `startup_64` in `head_64.S`, before any C function
runs.

## Code

- Branch: `simd-kernel-poc`
- Base: `a0c83177734ab98623795e1ba2cf4b72c23de5e7` (v7.0-rc4)
- Tree: `/home/bjorntopel/src/linux/master`

Commits:
```
26c868bca6e6 x86/fpu: Add per-CPU kernel FPU save areas for SIMD kernel PoC
56307633de34 x86/entry: Save/restore FPU state at kernel entry boundaries
c5ec23f165f8 x86/boot: Enable SSE in CR4/CR0 at early boot for SIMD kernel
0a76f3455be3 x86/build: Enable compiler SIMD vectorization for the kernel
274df90b1c56 docs: Add SIMD kernel PoC report and vectorization analysis data
```

## Limitations and future work

1. **AVX-512 frequency throttling**: With `-march=native`, GCC may
   emit AVX-512 on CPUs that support it, causing frequency downclocking.
   A production version should cap at `-mavx2` or use
   `-mprefer-vector-width=256`.

2. **Save area size**: Each per-CPU nesting level uses a full
   `union fpregs_state` (PAGE_SIZE). With AVX-512/AMX this is
   significant memory per CPU. A production version should use the
   actual `fpu_kernel_cfg.max_size`.

3. **Performance**: Every syscall and interrupt now pays an XSAVE +
   XRSTOR cost. Whether the compiler-generated SIMD (mostly struct
   copies) compensates for this overhead is an open question that
   requires benchmarking.

4. **`noinstr` correctness**: The new FPU helpers are marked `noinstr`
   but call functions that may not be. A production version needs
   careful auditing of the instrumentation boundaries.

5. **Rust**: The Rust target spec still disables SSE
   (`KBUILD_RUSTFLAGS`). Rust code in the kernel would need similar
   treatment.
