# MSR[LE] console experiment

Status: **the first console run stopped at the BE exception control, before any LE switch**. The user reported a
console crash. `mfmsr` returned `0xB030` and the BE harness passed; LE behavior and the cause of the crash remain
unresolved. Evidence is in `results/2026-10-18-msr-le-handler-crash/` and `docs/HARDWARE.md`. This is the port feasibility
probe requested on 2026-10-18, not an SDK endian mode or a change to the game/toolchain.

## Run

Build from PowerShell:

```powershell
wsl.exe -d Debian --exec bash -c 'cd /mnt/z/xsdk-msr-le/Xbox-360-SDK && make clean && make -j32'
```

Copy `build/probe_msr_le.xex` to the console and launch it. It writes `probe_msr_le.log` beside the title. With no
configuration file, it now runs `handler`, stopping before any LE switch. The startup line includes
`handler diagnostics v2` to distinguish this build. An optional `probe_msr_le.txt` beside the title selects:

| Text | Tests |
|---|---|
| `handler` (default) | BE execution, deliberate illegal instruction and recovery, BE load; then stop |
| `all` | BE controls, switch/fetch, data accesses, persistence, timing, direct query, direct wait |
| `core` | BE controls, switch/fetch, data accesses, persistence, timing |
| `query` | BE controls, switch/fetch, direct `KeQuerySystemTime` |
| `wait` | BE controls, switch/fetch, direct `NtWaitForSingleObjectEx` |

For the next run, leave the configuration absent or set it to `handler`. This still deliberately raises the same
exception that preceded the reported crash; recovery has not yet been verified with this revision. A completed run
must report all three BE controls passing and `stopping before any LE switch`. Use an explicit LE mode only after
this prerequisite works. The mode does not weaken or bypass the exception control.

Each potentially faulting experiment logs `BEGIN` before entry and `END` after recovery. If the console stops, retain
the log before relaunching; the next launch overwrites it. `core`, `query`, and `wait` let later experiments run
independently. A fault in a kernel call may stop the title or console. The handler does not unwind kernel frames or
guess how to repair them. A missing `END` identifies an incomplete experiment, not a negative hardware finding.

The user performs the hardware upload/launch and returns the log, as described in `CLAUDE.md`. Keep returned evidence
under `results/<run-date>-msr-le/` and only then add settled findings to `docs/HARDWARE.md`.

## Measurements

1. **Switch and fetch prerequisite.** Read the original MSR in the title, logging PR, EE, and HV. Verify BE code and
   a known illegal instruction through the existing console-proven vectored-handler ABI. Try `mtmsrd ...,0` and
   `SRR0/SRR1` plus `rfid` independently, preserving the original MSR except for LE. `rfid` disables EE only while
   installing the return registers and restores it on entry. No HV/HVX calls or hypervisor registers are used.

   The first word at the destination is `08 00 00 48`: a never-taken trap in BE, a branch by eight bytes in LE.
   BE fetch branches to BE instructions; LE fetch enters an independently assembled LE instruction bank. Both
   execute `isync`, read MSR, compute `0x13579BDF`, and report which bank ran. The restore gate likewise supplies
   both byte orders. Thus the probe tests swapped instructions when fetch flips. The build checks the actual
   linked gates and embedded instruction bytes, including the absence of relocations in those embedded banks.

   This discriminates ordinary BE fetch from per-word byte-reversed fetch. A different legacy address-swizzling
   implementation or synchronization failure can fault; that remains an unresolved fetch experiment. Merely
   observing LE=1 is not evidence of usable LE data accesses. The first viable method is selected for later tests;
   timing covers both methods when both work.

2. **Data.** Source bytes begin `11 22 33 44 55 66 77 88`. Separate cases check `lwz`, `lhz`, `ld`, `stw`, `lfs`, and
   `stfs`. The floating cases load LE bytes for 1.0 and store a register loaded with 1.0 while still BE. Store buffers
   are read byte by byte after returning to BE, with guards checked. Cached `lwz` at offsets 1, 2, and 3 reports
   either the exact LE value or the alignment exception; this classification check does not imply unaligned loads
   work. `lmw r30` and `stmw r30` test two registers at aligned addresses, expecting an alignment exception in LE.
   Their raw D-form encodings are used because GAS refuses those mnemonics when assembling LE code.

3. **Persistence.** After the basic aligned data checks pass, execute LE loads on hardware thread 0 for two seconds,
   checking each value and MSR[LE] on every iteration. EE must be set. Two BE observers run while the LE loop's byte
   flag is set: one sharing hardware thread 0, one on its sibling hardware thread 1. Each counts passes and checks
   its own MSR and BE loads. Positive counts on hardware thread 0 show actual scheduling during the LE window;
   counts on thread 1 test isolation on the same
   core. The loop uses no kernel calls. Elapsed time and scheduling support tick/interrupt persistence; there is no
   instrumented interrupt-vector counter, so the log explicitly identifies that inference.

4. **Kernel boundary.** If fetch remains BE, call the actual imported `KeQuerySystemTime` and
   `NtWaitForSingleObjectEx` with LE still set. Query output is checked as both byte orders against BE timestamps
   taken before and after. The wait uses an initially unsignaled event, a BE timeout, and a signaler on hardware
   thread 2, which waits for the LE entry flag and then delays 50 ms before signaling. Assembly timestamps bracket
   the call itself, excluding logging. Check status, blocking duration, signal timing, MSR after the call, and an LE
   load after return. These test only these exports and these argument layouts; two successes do not certify every kernel,
   XAM, or xsdk function. If fetch flips, direct entry into BE import/kernel instructions is skipped and reported.

   This is an exported-kernel-call test. It does **not** issue a raw `sc`: no non-HV service ABI has been established
   here, and the agreed guidelines exclude HV/HVX calls. Raw system-call-vector persistence remains untested.

5. **Cost.** Warm the code, then measure 17 pairs of 256 BE-control and LE round-trip calls. The control traverses
   the same C/assembly harness without switching. Report raw time-base ticks, gross time, and paired differences,
   with minimum, median, and maximum. Convert ticks to estimated CPU cycles at 64 cycles/tick, the existing console
   timing ratio. Interrupts remain enabled except for each short SRR update. This measures the complete probe
   round trip, including dispatch and synchronization; baseline subtraction estimates its incremental switching
   cost. It is not a direct hardware cycle-counter reading or a ready-made port boundary trampoline.

## Recovery and limits

The original handler could reject pointers before printing anything, and read the saved frame with a 64-bit load
before its first diagnostic. Thus the missing `fault:` line in the first run does not establish whether the kernel
entered the callback, a check rejected it, or the callback itself faulted. No specific cause is asserted from that log.
The revised handler follows the earlier `probe_fault` scaffold: a recursion guard before logging, entry arguments
logged before reading exception pointers, current callback stack bounds, and volatile 32-bit context accesses.
It validates only the aligned words it actually reads and logs each rejection, the IAR redirect, and its return.
The main-thread stack bounds and code addresses are also logged before the test. These are diagnostic changes,
not a console-verified crash fix.

The handler accepts only known probe instruction addresses on the main thread's exact saved frame. It records the
exception and redirects IAR to the matching-endian restore gate. It does not edit the context's word at +4, whose
interpretation as MSR remains unverified in the earlier hardware notes. A second fault during recovery is passed
on. Initial `mfmsr` or switching-privilege failures get distinct exits and cannot count as successful LE execution.
The return path checks that LE is clear before touching the BE stack or result structure; a failed restore faults.

The assembly saves r14-r31 in full, reserves the kernel argument home area, never writes r2/r13, and keeps only code
addresses in LR. It does not touch the BE stack or call C while LE, apart from the two explicit boundary experiments.
If the kernel fails to restore LE on an interrupt, a fetch-flipping implementation may fault before a counter can
be updated; the log then shows a fault/incomplete run rather than a successful persistence result.

Xenia is not a validator for this probe: its `mfmsr` reports the emulator's lock state and its full `mtmsrd` is
unimplemented (`xenia:src/xenia/cpu/ppc/ppc_emit_control.cc:727-795`). No emulator output is recorded as a console fact.
Even a successful run does not establish a complete LE game ABI: atomics, VMX, callbacks, shared layouts, and library
code generation still need consideration. Do not select the port route from a build or from the aggregate PASS count;
read the actual observations, particularly unaligned accesses, fetch, persistence, and the two boundary calls.

## Public references

- [PowerPC architecture v2.02, Book III](https://powerpc.dev/general/PPC_Vers202_Book3_public.pdf): LE is optional;
  MSR and privileged switching semantics are in chapters 2-3, alignment exceptions in section 5.5.6, synchronization
  in chapter 7. These are architecture expectations, not proof that Xenon implements the optional facility.
- [Linux PowerPC assembly helpers](https://github.com/torvalds/linux/blob/master/arch/powerpc/include/asm/ppc_asm.h):
  `FIXUP_ENDIAN` explains the never-trap / branch-by-eight fetch discriminator. The surrounding probe is original.
- Existing console evidence: `docs/HARDWARE.md`, "How the kernel hands a title its faults", "The time base runs at
  49.875 MHz", and "The console keeps only LR's low word across an interrupt"; sources in `tests/probe_fault.c`,
  `tests/probe_fault_sites.S`, and `src/thread.c`.
