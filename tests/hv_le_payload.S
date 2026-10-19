#include "hv_le.h"

	.section .text.hv_payload, "ax"
	.balign 128
	.globl xsdk_hv_le_payload
xsdk_hv_le_payload:
// Expansion callbacks receive the operation in r4 (HV-Companion Main.h:32-35).
	cmpwi r4, HV_LE_PING
	beq .Lping
	cmpwi r4, HV_LE_CONTROL
	beq .Lcontrol
	cmpwi r4, HV_LE_READ
	beq .Lread
	cmpwi r4, HV_LE_STACK
	beq .Lstack
	cmplwi r4, HV_LE_BE_DATA
	blt .Linvalid
	cmplwi r4, HV_LE_DATA
	bgt .Linvalid
	mfmsr r10
	srdi r11, r10, 60
	andi. r11, r11, 1
	beq .Lnot_hv
	andi. r11, r10, 1
	bne .Lnot_be
// Only 64-bit saves use this stack; they need eight-byte alignment.
	andi. r11, r1, 7
	bne .Lbad_stack
	mflr r0
	stdu r1, -256(r1)
	std r0, 8(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28
	std r\n, 32 + 8 * (\n - 14)(r1)
	.endr
	mr r14, r10
	mr r18, r4
	bl .Laddress
.Laddress:
	mflr r15
	addi r16, r15, .Linput - .Laddress
	addi r17, r15, .Lscratch - .Laddress
	addi r19, r15, .Lreturned - .Laddress
	addi r15, r15, .Lreport - .Laddress
	li r0, 0
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11
	std r0, 8 * \n(r15)
	.endr
	std r0, 0(r17)
	li r20, 0
	li r21, 0
	li r22, 0
	li r23, 0
	li r24, 0
	li r25, 0
	lis r26, 0x1122
	ori r26, r26, 0x3344
	li r0, 0
	ori r0, r0, 0x8000
	andc r27, r14, r0
	cmpwi r18, HV_LE_BE_DATA
	beq .Lswitch
	ori r27, r27, 1
.Lswitch:
	mtmsrd r27, 1
	sync
	mtmsrd r27, 0
	.globl xsdk_hv_le_gate
xsdk_hv_le_gate:
// BE never-trap / LE branch +8 (Linux arch/powerpc/include/asm/ppc_asm.h, FIXUP_ENDIAN).
	.byte 0x08, 0x00, 0x00, 0x48
	b .Lbig_window
	.globl xsdk_hv_le_little_window
xsdk_hv_le_little_window:
	.incbin "build/tests/hv_le_window_le.bin"
.Lbig_window:
	.globl xsdk_hv_le_big_window
xsdk_hv_le_big_window:
	.incbin "build/tests/hv_le_window_be.bin"
	.globl xsdk_hv_le_returned
xsdk_hv_le_returned:
.Lreturned:
	mfmsr r28
	andi. r0, r28, 1
	bne .Lpark
	std r18, HV_LE_OPERATION * 8(r15)
	std r14, HV_LE_BEFORE * 8(r15)
	std r27, HV_LE_REQUESTED * 8(r15)
	std r20, HV_LE_INSIDE * 8(r15)
	std r28, HV_LE_AFTER * 8(r15)
	std r21, HV_LE_FETCH_ORDER * 8(r15)
	std r22, HV_LE_SENTINEL * 8(r15)
	std r23, HV_LE_WORD * 8(r15)
	std r24, HV_LE_HALF * 8(r15)
	std r25, HV_LE_DOUBLE * 8(r15)
	li r9, 0
	.irp n, 0,1,2,3,4,5,6,7
	lbz r10, \n(r17)
	sldi r9, r9, 8
	or r9, r9, r10
	.endr
	std r9, HV_LE_STORED_BYTES * 8(r15)
	sync
	lis r0, 0x4856
	ori r0, r0, 0x4c45
	std r0, HV_LE_COMPLETE * 8(r15)
	sync
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28
	ld r\n, 32 + 8 * (\n - 14)(r1)
	.endr
	ld r0, 8(r1)
	mtlr r0
	addi r1, r1, 256
	li r3, 0
	blr
.Lping:
	lis r3, 0x4856
	ori r3, r3, 0x4c45
	sldi r3, r3, 32
	ori r3, r3, 1
	blr
.Lcontrol:
	mfmsr r3
	blr
.Lstack:
	mr r3, r1
	blr
.Lread:
	cmplwi r5, HV_LE_FIELDS
	bge .Linvalid
	mflr r11
	bl .Lread_address
.Lread_address:
	mflr r12
	mtlr r11
	addi r12, r12, .Lreport - .Lread_address
	sldi r5, r5, 3
	ldx r3, r12, r5
	blr
.Linvalid:
	li r3, -1
	blr
.Lnot_hv:
	li r3, -2
	blr
.Lnot_be:
	li r3, -3
	blr
.Lbad_stack:
	li r3, -4
	blr
	.globl xsdk_hv_le_park
xsdk_hv_le_park:
.Lpark:
	b .Lpark
	.globl xsdk_hv_le_code_end
xsdk_hv_le_code_end:
	.balign 128
.Linput:
	.byte 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
	.balign 128
.Lscratch:
	.space 128
.Lreport:
	.space HV_LE_FIELDS * 8
	.globl xsdk_hv_le_payload_end
xsdk_hv_le_payload_end:
