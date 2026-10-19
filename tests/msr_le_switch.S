#include "msr_le.h"

	.section .rodata.xsdk_msr_le_floats, "a"
	.balign 8
.Lfloats:
	.double 0.0, 1.0

	.text
	.balign 4
	.globl xsdk_msr_le_run
	.type xsdk_msr_le_run, @function
xsdk_msr_le_run:
	mflr r0
	stw r0, 4(r1)
	stwu r1, -256(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, 96 + 8 * (\n - 14)(r1)
	.endr
	mr r14, r3
	stw r1, 68(r14)
	lwz r15, 0(r4)
	lwz r16, 4(r4)
	lwz r20, 8(r4)
	lwz r18, 12(r4)
	lwz r22, 16(r4)
	lwz r23, 20(r4)
	lwz r24, 24(r4)
	lwz r25, 28(r4)
	lwz r27, 32(r4)
	lwz r28, 36(r4)
	li r26, 0
	li r29, 0
	li r30, 0
	li r31, 0
	lis r19, xsdk_msr_le_return@ha
	addi r19, r19, xsdk_msr_le_return@l
	lis r21, xsdk_msr_le_recover@ha
	addi r21, r21, xsdk_msr_le_recover@l
	lis r12, .Lfloats@ha
	addi r12, r12, .Lfloats@l
	lfd f1, 0(r12)
	lfd f2, 8(r12)
	.globl xsdk_msr_le_read_site
xsdk_msr_le_read_site:
	mfmsr r17
	std r17, 0(r14)
	cmpwi r20, LE_FAULT_CONTROL
	bne .Lenter
	.globl xsdk_msr_le_fault_site
xsdk_msr_le_fault_site:
	.long 0
.Lenter:
	cmpwi r18, LE_CONTROL
	beq xsdk_msr_le_gate
	ori r11, r17, 1
	cmpwi r18, LE_RFID
	beq xsdk_msr_le_rfid_begin
	sync
	.globl xsdk_msr_le_mtmsrd_site
xsdk_msr_le_mtmsrd_site:
	mtmsrd r11, 0
	.globl xsdk_msr_le_gate
xsdk_msr_le_gate:
// This is a nop in BE and b .+8 in LE (Linux: arch/powerpc/include/asm/ppc_asm.h, FIXUP_ENDIAN).
	tdi 0, r0, 0x48
	b xsdk_msr_le_payload
	.globl xsdk_msr_le_le_payload
xsdk_msr_le_le_payload:
	.incbin "build/tests/msr_le_payload_le.bin"
	.globl xsdk_msr_le_le_payload_end
xsdk_msr_le_le_payload_end:
	.long 0
	.globl xsdk_msr_le_rfid_begin
xsdk_msr_le_rfid_begin:
// SRR0/1 must not be overwritten by a tick between these writes (PowerPC v2.02 Book III, sections 2.3 and 5.4).
	li r0, 0
	ori r0, r0, 0x8000
	andc r0, r17, r0
	mtmsrd r0, 1
	lis r12, xsdk_msr_le_gate@ha
	addi r12, r12, xsdk_msr_le_gate@l
	mtsrr0 r12
	mtsrr1 r11
	rfid
	.globl xsdk_msr_le_rfid_end
xsdk_msr_le_rfid_end:
	.long 0
	.globl xsdk_msr_le_entry_fault
xsdk_msr_le_entry_fault:
	mtmsrd r17, 1
	b xsdk_msr_le_return
	.globl xsdk_msr_le_recover
xsdk_msr_le_recover:
	tdi 0, r0, 0x48
	b xsdk_msr_le_restore
	.incbin "build/tests/msr_le_restore_le.bin"
	.long 0
	.globl xsdk_msr_le_return
xsdk_msr_le_return:
	isync
	mfmsr r0
	andi. r11, r0, 1
	bne .Lrestore_failed
	std r0, 16(r14)
	std r26, 8(r14)
	std r27, 24(r14)
	std r28, 32(r14)
	std r29, 40(r14)
	std r30, 48(r14)
	stfd f1, 56(r14)
	stw r31, 64(r14)
	li r0, 1
	stw r0, 72(r14)
	stw r25, 76(r14)
	stw r22, 80(r14)
	.globl xsdk_msr_le_unwind
xsdk_msr_le_unwind:
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, 96 + 8 * (\n - 14)(r1)
	.endr
	addi r1, r1, 256
	lwz r0, 4(r1)
	mtlr r0
	blr
.Lrestore_failed:
// Never run BE stack accesses or C with LE still set.
	.long 0
	b .Lrestore_failed
	.size xsdk_msr_le_run, . - xsdk_msr_le_run

	.globl xsdk_msr_le_stack
	.type xsdk_msr_le_stack, @function
xsdk_msr_le_stack:
// PCR stack bounds (xenia:src/xenia/kernel/xthread.h:78-79).
	lwz r4, 0x74(r13)
	lwz r5, 0x70(r13)
	stw r4, 0(r3)
	stw r5, 4(r3)
	blr
	.size xsdk_msr_le_stack, . - xsdk_msr_le_stack
