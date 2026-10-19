#include "msr_le.h"

// Built in both byte orders, with no relocations; r14-r25 belong to the BE harness.
	.section .text.payload, "ax"
	.balign 4
	.globl xsdk_msr_le_payload
	.type xsdk_msr_le_payload, @function
xsdk_msr_le_payload:
	isync
	mfmsr r26
#ifdef LE_CODE
	li r31, 1
#else
	li r31, 0
#endif
	cmpwi r20, LE_FETCH
	beq .Lfetch
	cmpwi r20, LE_WORD
	beq .Lword
	cmpwi r20, LE_HALF
	beq .Lhalf
	cmpwi r20, LE_DOUBLE
	beq .Ldouble
	cmpwi r20, LE_STORE
	beq .Lstore
	cmpwi r20, LE_FLOAT_LOAD
	beq .Lfloat_load
	cmpwi r20, LE_FLOAT_STORE
	beq .Lfloat_store
	cmpwi r20, LE_MULTIPLE_LOAD
	beq .Lmultiple_load
	cmpwi r20, LE_MULTIPLE_STORE
	beq .Lmultiple_store
	cmpwi r20, LE_SPIN
	beq .Lspin
	cmpwi r20, LE_OBSERVE
	beq .Lobserve
	cmpwi r20, LE_QUERY
	beq .Lquery
	cmpwi r20, LE_WAIT
	beq .Lwait
	.long 0
.Lfetch:
	lis r27, 0x1357
	ori r27, r27, 0x9bdf
	b .Ldone
.Lword:
	lwz r27, 0(r15)
	b .Ldone
.Lhalf:
	lhz r27, 0(r15)
	b .Ldone
.Ldouble:
	ld r27, 0(r15)
	b .Ldone
.Lstore:
	lis r27, 0x1122
	ori r27, r27, 0x3344
	stw r27, 0(r16)
	b .Ldone
.Lfloat_load:
	lfs f1, 8(r15)
	b .Ldone
.Lfloat_store:
	stfs f2, 0(r16)
	b .Ldone
.Lmultiple_load:
	mr r29, r31
// GAS refuses lmw/stmw in LE; these are the ISA D-form encodings under test.
	.long 0xbbcf0000
	mr r27, r30
	mr r28, r31
	mr r31, r29
	li r30, 0
	b .Ldone
.Lmultiple_store:
	mr r29, r31
	lis r30, 0x1122
	ori r30, r30, 0x3344
	lis r31, 0x5566
	ori r31, r31, 0x7788
	.long 0xbfd00000
	mr r31, r29
	li r30, 0
	b .Ldone
.Lspin:
	li r0, 1
	stb r0, 0(r23)
	sync
	b .Lspin_start
.Lobserve:
	lbz r0, 0(r23)
	cmpwi r0, 0
	beq .Ldone
.Lspin_start:
	mftb r12
.Lspin_loop:
	lwz r27, 0(r15)
	cmpw r27, r25
	beq .Ldata_ok
	addi r30, r30, 1
.Ldata_ok:
	mfmsr r28
	xor r0, r28, r26
	andi. r0, r0, 1
	beq .Lmsr_ok
	addi r30, r30, 1
.Lmsr_ok:
	addi r29, r29, 1
	cmpwi r20, LE_OBSERVE
	bne .Ltime
	lbz r0, 0(r23)
	cmpwi r0, 0
	beq .Ldone
.Ltime:
	mftb r11
	subf r11, r12, r11
	cmplw r11, r22
	blt .Lspin_loop
	cmpwi r20, LE_OBSERVE
	beq .Ldone
	sync
	li r0, 0
	stb r0, 0(r23)
	b .Ldone
.Lquery:
	mr r3, r16
	mtctr r24
	mftb r25
	bctrl
	mftb r22
	li r27, 0
	b .Lcalled
.Lwait:
	li r0, 1
	stb r0, 0(r23)
	sync
	mr r3, r27
	li r4, 1
	li r5, 0
	mr r6, r28
	mtctr r24
	mftb r25
	bctrl
	mftb r22
	mr r27, r3
.Lcalled:
	mfmsr r28
	lwz r29, 0(r15)
.Ldone:
	mtctr r21
	bctr
	.globl xsdk_msr_le_payload_end
xsdk_msr_le_payload_end:
	.size xsdk_msr_le_payload, . - xsdk_msr_le_payload

	.section .text.restore, "ax"
	.globl xsdk_msr_le_restore
	.type xsdk_msr_le_restore, @function
xsdk_msr_le_restore:
	mtctr r19
	cmpwi r18, LE_CONTROL
	beq .Lreturn
	cmpwi r18, LE_RFID
	beq .Lrfid
	sync
	mtmsrd r17, 0
// Both fetch orders reach isync and bctr; the target is BE after a successful restore.
	.byte 0x08, 0x00, 0x00, 0x48
	.byte 0x48, 0x00, 0x00, 0x0c
	.byte 0x2c, 0x01, 0x00, 0x4c
	.byte 0x20, 0x04, 0x80, 0x4e
	.byte 0x4c, 0x00, 0x01, 0x2c
	.byte 0x4e, 0x80, 0x04, 0x20
.Lrfid:
	mfmsr r0
	li r11, 0
	ori r11, r11, 0x8000
	andc r0, r0, r11
	mtmsrd r0, 1
	mtsrr0 r19
	mtsrr1 r17
	rfid
	.long 0
.Lreturn:
	isync
	bctr
	.size xsdk_msr_le_restore, . - xsdk_msr_le_restore
