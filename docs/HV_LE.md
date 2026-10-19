# HV expansion endian probe

`build/probe_hv_le.xex` implements the separate experiment authorized on 2026-10-18. The first hardware run
installed the expansion and confirmed HV=1 with a clean title return. Its BE data operation was rejected before
the test window. The v2 control run measured the HV stack's eight-byte alignment, passed BE loads/stores and
exact return checks, and exited with status zero. The subsequent `retail le` run passed: MSR[LE] read back as one,
instruction fetch stayed BE, and aligned integer loads/stores used LE data order inside HV. Both windows restored
the exact entry MSR, all title returns remained BE, and the title exited successfully. Title-side LE and the
remaining port prerequisites are still unmeasured.
The earlier title-side probe measured HV=0 and stopped in its BE exception test before requesting LE
(`MSR_LE.md`). This probe neither injects that fault nor installs an exception handler.

## Run in two stages

Cold-boot the console before each run. Expansions may remain installed until reboot; the source used here provides
no uninstall operation. The generated expansion ID is the first 32 bits of the payload SHA-256, logged at startup,
so changed payloads do not intentionally reuse the same ID. Do not fill expansion slots by repeatedly installing
different builds without rebooting.

Upload `build/probe_hv_le.xex`. Its adjacent `probe_hv_le.txt` accepts exactly two words:

| Configuration | Behavior |
|---|---|
| `retail control` | Default when the file is absent. Install, verify payload identity, read HV MSR, run BE data controls. |
| `retail le` | Repeat every control, then test LE fetch, then aligned LE data. Each stage gates the next. |
| `devkit control` | Same control stage, using the devkit service numbers. |
| `devkit le` | Same LE stage, using the devkit service numbers. |

The default matches this repository's RGH retail-kernel target. Choose the profile for the running firmware, not
for the presence of an XDK on the PC. There is no automatic service-number fallback or read from a hardcoded
kernel address. Collect `probe_hv_le.log` after the control run. Proceed with `le` only after the control log ends
with `PASS: control stage complete`. The LE mode also rechecks all controls itself.

The firmware must already accept unsigned, plaintext HXPR expansions. Public xeBuild research describes the
patches that permit this; the probe does not install those patches or contain keys/signatures/encrypted code.
A stock firmware's acceptance is not established. An installation error is logged in full and prevents any
expansion call. The service profile itself must be correct before the first call.

## What runs

1. Allocate one physical page, copy the generated expansion, flush its data cache lines, install it, and free the
   staging page. HV-Companion uses this allocation/install/free lifetime. Installation takes service `0x72` on its
   retail path or `0x70` on its devkit path; calls take `0x73` or `0x71` respectively. `sc` receives the service in
   r0 and arguments in r3 onward.
2. Read a 64-bit payload identity, then run a leaf callback containing `mfmsr` and `blr`. Require HV=1 and LE=0.
   Every title-side call verifies exact MSR restoration, 64-bit canaries in r14-r30, r31/the stack, r2/r13, LR,
   and CR2-4 before the next stage. Assembly never writes r2 or r13. A second leaf callback returns the HV stack
   pointer without using memory; require eight-byte alignment for the payload's `std`/`ld` saves. Check that the
   report getter rejects an index just past its fixed buffer before running any window.
3. Run the BE bank against aligned bytes `11 22 33 44 55 66 77 88` inside the expansion. Check `lwz`, `lhz`, `ld`
   and `stw`, including all eight scratch bytes read individually with `lbz` after restoring BE.
4. In `le` mode, clear EE for the bounded window, request only LE in addition to that change, then execute a
   fetch discriminator before any endian-sensitive data access. A BE never-trap instruction becomes a branch
   into separately assembled little-endian words if fetch changes. Each bank records its identity, the MSR and
   a register-built sentinel. Restore the complete entry MSR before returning.
5. Only after successful LE readback, fetch execution and restoration, repeat the window with aligned integer
   data accesses. Expected results are `44332211`, `2211`, `8877665544332211`, and scratch bytes
   `44 33 22 11 00 00 00 00` after storing `11223344`. Fetch order must agree with the preceding fetch test.

The callback saves its nonvolatile registers on its HV stack in BE. The LE window performs no stack access and
calls no C or kernel routine. Its only memory accesses are the three input loads and one scratch store. Both
buffers and the fixed-size report live in the expansion itself. A bounds-checked report getter returns one
64-bit field per call; it accepts no addresses and provides no arbitrary memory access. The title remains BE.

Rejections return signed 64-bit values: -1 for an invalid operation or report index, -2 for HV clear, -3 for LE
already set, and -4 for a stack not aligned to eight bytes. These return before changing MSR or touching the
test buffers. The separate stack diagnostic logs both the eight-byte and sixteen-byte remainders.

If restoration leaves instruction fetch LE, the return gate parks in a little-endian self-branch. If fetch is BE
but the LE bit remains set, the BE continuation parks before any stack or report access. An unexpected stack
change at the title boundary also parks. Such a failure can require a console reboot; the last flushed `BEGIN`
line identifies the attempted stage. There is no assumed recovery path for an HV fault. FP, unaligned and
multiple-register operations are deliberately absent from this first expansion.

## Build and checks

The ordinary `make` builds this probe using the existing GCC/binutils toolchain and Python standard library.
No XDK compiler or library participates. `tests/hv_le_payload.S` is position-independent; its BE and LE windows
are independently assembled from `tests/hv_le_window.S`. `tools/build_hv_le.py` rejects relocations, checks bank
equivalence except the fetch marker, checks local branches and the exact allowed LE memory instructions, checks
the embedded fetch/restore gates, and limits the complete image to one page. All payload code and packaging
source are reviewable; the generated image is build output.

Before making the XEX, `tools/test_hv_le.py` checks the actual assembled gate and windows under explicit BE-fetch
and LE-fetch models, including ignored LE requests and failed restoration. It checks that the register-only fetch
case makes no data access, that data stays within the two buffers, that corrupt banks are rejected, and that the
linked title embeds the exact generated unsigned page. The same tests run on Windows or WSL with
`python tools/test_hv_le.py build`. These small instruction models do not emulate Xenon's HV or prove its behavior.

The packer writes the expansion header, section descriptors, payload and SHA-1, with zero IV and signature.
It does not modify or reuse HV-Companion's signed payload. The payload SHA-256 and expansion ID in the log identify
the precise build tested. Host checks establish the construction of the test; they cannot establish hardware
LE behavior or firmware acceptance.

This probe does not determine whether a title can return from HV with LE set, whether interrupts or thread
switches preserve title-side LE, kernel-call compatibility, FP behavior, or switch cost. A successful bounded
HV test is evidence only for the observed instructions in that context. Do not infer a game-port decision yet.

## First hardware control (2026-10-18)

The user's transcript is `results/2026-10-18-hv-le-control/probe_hv_le-v1.log`. The tested XEX SHA-256 was
`b595034917f099400a34792daf5db7f5866ac1a7df97fac449cb3ace539b8bf2`; its payload ID was `2BA3FB50`.
Installation returned zero, the identity returned `48564C4500000001`, and the leaf `mfmsr` returned
`9000000000001000`: HV=1, LE=0, EE=0, SF=1. Every call returned to title MSR `9030` with no detected register damage.

The BE data call returned -1, then the title exited with status 1. That v1 code used the same rejection for an
invalid operation, HV clear, LE set, or a stack not aligned to sixteen bytes. The stack guard is the likely cause,
given the successful preceding controls, but the log does not identify which check rejected this call. V2 uses
the eight-byte alignment required by its integer saves and distinguishes the rejections. The completed v2
control run is recorded below. Neither the initial rejection nor the corrected build establishes LE support.

The v2 delivery passed a clean full `make -j32` build with zero compiler/linker warnings and all five host tests
in WSL and Windows. Its payload ID is `82F7BBC4`, SHA-256
`82f7bbc43a647fa67ec2f1d8182e4572d94c180190aebb2d184a12f2747dcacc`; the XEX SHA-256 is
`db26852551fc651c47fc0f04d0e080d261e3feec31f1ba2d4ad9f0c73110c006`.

## Completed v2 control (2026-10-18)

The user's complete transcript is `results/2026-10-18-hv-le-control/probe_hv_le-v2.log`. It supersedes the earlier
file fragment that ended partway through the stack line; that fragment did not establish a failure.

The stack callback returned `0000006A01F61EF8`, with `mod8=0 mod16=8`. V1's sixteen-byte guard would reject this
stack; v2's eight-byte guard accepted it and the BE data window completed. The out-of-range report control returned
-1 as intended, and all twelve valid report reads returned cleanly.

The BE window recorded HV MSR `9000000000001000` before, requested, inside and after, BE instruction words and
sentinel `13579BDF`. It loaded `11223344`, `1122` and `1122334455667788`; storing `11223344` produced bytes
`11 22 33 44 00 00 00 00`, verified individually. Every title return kept MSR `9030` and the checked registers
unchanged. The log ends with `PASS: control stage complete` and `exit: status 0`.

This satisfies the prerequisite for the LE stage. Cold-boot, put `retail le` in `probe_hv_le.txt` beside the same
v2 XEX, and run it. The mode repeats all controls, tests register-only LE fetch and exact restoration, then tests
aligned integer LE data only if fetch/readback/restoration passed. No LE result follows from the BE control.

## Completed HV LE fetch and data run (2026-10-18)

The user's pasted transcript is preserved verbatim in `results/2026-10-18-hv-le-aligned/probe_hv_le.log`. It used
the same v2 payload `82F7BBC4`, selected `retail le`, repeated all controls successfully and exited with status 0.

Both the register-only fetch window and the aligned data window recorded:

| Observation | Value |
|---|---|
| Entry HV MSR | `9000000000001000` |
| Requested and observed window MSR | `9000000000001001` |
| Restored HV MSR | `9000000000001000` |
| Executed instruction bank | BE words |
| Register-built fetch sentinel | `13579BDF` |
| Title MSR before and after every call | `9030` |

This measures BE instruction execution with LE set, including `isync` and `mfmsr` inside the selected bank. The
little-endian instruction bank was not taken. The data window's ordinary instructions read the fixed bytes
`11 22 33 44 55 66 77 88` as follows:

| Operation | Observed result |
|---|---|
| `lwz` | `44332211` |
| `lhz` | `2211` |
| `ld` | `8877665544332211` |
| `stw` of `11223344`, inspected with `lbz` after BE restoration | `44 33 22 11 00 00 00 00` |

All report reads returned cleanly. Every call preserved the checked title nonvolatile GPRs, CR2-4, LR, r2/r13 and
stack. The expansion restored BE before returning; the title itself never ran LE.

The bounded HV experiment is complete. These are HV=1, EE=0, SF=1 results. They establish aligned integer LE
accesses and BE fetch in that context, not an LE game runtime. Remaining measurements are `lfs`/`stfs`, unaligned
and multiple-register operations, LE execution in a title thread, interrupt/decrementer/thread-switch persistence,
another hardware thread's independence, kernel/xsdk crossings and round-trip switch cost. The next decisive
question is whether an HV-mediated switch can leave a title thread running LE and later restore it to BE; this
probe deliberately restores BE inside HV and does not answer that question.

## Sources

- HV-Companion commit `425ad45ff59169f4223f1d02c27c8e5a13dd5b04`,
  [Main.cpp](https://github.com/proferabg/HV-Companion/blob/425ad45ff59169f4223f1d02c27c8e5a13dd5b04/XEX/hvcompanion/Main.cpp),
  lines 25-61 and 137-145: service selection, physical staging lifetime and register ABI.
  [Main.h](https://github.com/proferabg/HV-Companion/blob/425ad45ff59169f4223f1d02c27c8e5a13dd5b04/XEX/hvcompanion/Main.h),
  lines 32-55: decoded callback uses r4 as its operation and returns through LR. Interface/format facts only;
  no embedded binary copied.
- [Xbox-360-Crypto exp_signer.py](https://github.com/GoobyCorp/Xbox-360-Crypto/blob/ad62bee90e0342cbfe482a4dddf60f9a0f456660/exp_signer.py):
  HXPR header at 0, expansion information at 0x130, section descriptor at 0x160, code at 0x170, and SHA-1 over
  information/section/payload through the declared padded end. Only format facts are used; signing/encryption
  code, dependencies and keys are not used.
- [InvoxiPlayGames xeBuild research](https://tangled.org/invoxiplaygames.uk/x360-research/blob/af9edc08d65850f5c5cc0f952c61d747ae20488e/homebrew/xebuild/freeboot-like_patches.md),
  "HvxExpansionInstall signature/encryption patches": unsigned expansions skip decryption on the described
  patched firmware. This is a prerequisite, not a modification made by the probe.
- Linux `arch/powerpc/include/asm/ppc_asm.h`, `FIXUP_ENDIAN`: dual-endian `08 00 00 48` fetch discriminator,
  also used by the existing title-side probe. PowerPC Book III defines the MSR bit positions; Xenon behavior
  remains a hardware measurement.
