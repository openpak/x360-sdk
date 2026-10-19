#include "hv_le.h"

	.text
	.balign 4
	.globl xsdk_hv_le_call
	.type xsdk_hv_le_call, @function
xsdk_hv_le_call:
	mflr r0
	stwu r1, -320(r1)
	std r0, 8(r1)
	stw r3, 24(r1)
	mfcr r10
	stw r10, 32(r1)
	mfmsr r11
	std r11, 40(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, 128 + 8 * (\n - 14)(r1)
	.endr
	std r2, 272(r1)
	std r13, 280(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	lis r\n, 0x4856
	ori r\n, r\n, \n
	sldi r\n, r\n, 32
	ori r\n, r\n, 0x4c45
	.endr
	mr r31, r1
// Service and argument registers follow HV-Companion Main.cpp:25-40,137-145.
	mr r0, r4
	mr r3, r5
	mr r4, r6
	mr r5, r7
	sc
// An unexpected LE fetch parks before touching the title stack.
	.byte 0x08, 0x00, 0x00, 0x48
	.byte 0x48, 0x00, 0x00, 0x08
	.byte 0x00, 0x00, 0x00, 0x48
	isync
	mfmsr r11
	andi. r0, r11, 1
	bne .Lcall_park
	cmpd r1, r31
	bne .Lcall_park
	li r9, 0
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	lis r10, 0x4856
	ori r10, r10, \n
	sldi r10, r10, 32
	ori r10, r10, 0x4c45
	xor r10, r10, r\n
	or r9, r9, r10
	.endr
	ld r10, 272(r1)
	cmpd r10, r2
	bne .Lcall_park
	xor r10, r10, r2
	or r9, r9, r10
	ld r10, 280(r1)
	cmpd r10, r13
	bne .Lcall_park
	xor r10, r10, r13
	or r9, r9, r10
	mflr r12
	ld r10, 8(r1)
	xor r10, r10, r12
	or r9, r9, r10
	mfcr r12
	lwz r10, 32(r1)
	xor r10, r10, r12
	lis r12, 0x00ff
	ori r12, r12, 0xf000
	and r10, r10, r12
	or r9, r9, r10
	lwz r12, 24(r1)
	std r3, 0(r12)
	ld r10, 40(r1)
	std r10, 8(r12)
	std r11, 16(r12)
	std r9, 24(r12)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, 128 + 8 * (\n - 14)(r1)
	.endr
	lwz r10, 32(r1)
	mtcrf 0x38, r10
	ld r0, 8(r1)
	mtlr r0
	addi r1, r1, 320
	blr
.Lcall_park:
	b .Lcall_park
	.size xsdk_hv_le_call, . - xsdk_hv_le_call

	.globl xsdk_hv_le_flush
	.type xsdk_hv_le_flush, @function
xsdk_hv_le_flush:
	li r5, 0
.Lflush:
	dcbf r5, r3
	addi r5, r5, 128
	cmplw r5, r4
	blt .Lflush
	sync
	blr
	.size xsdk_hv_le_flush, . - xsdk_hv_le_flush

	.section .rodata.hv_le_expansion, "a"
	.balign 128
	.globl xsdk_hv_le_expansion
xsdk_hv_le_expansion:
	.incbin "build/tests/hv_le.exp"
	.size xsdk_hv_le_expansion, . - xsdk_hv_le_expansion
