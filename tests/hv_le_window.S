#include "hv_le.h"

	.section .text.hv_window, "ax"
	.balign 4
	.globl xsdk_hv_le_window
xsdk_hv_le_window:
	isync
	mfmsr r20
#ifdef HV_LE_CODE
	li r21, 1
#else
	li r21, 0
#endif
	lis r22, 0x1357
	ori r22, r22, 0x9bdf
	cmpwi r18, HV_LE_FETCH
	beq .Lrestore
	cmpwi r18, HV_LE_BE_DATA
	beq .Ldata
	andi. r0, r20, 1
	beq .Lrestore
.Ldata:
	lwz r23, 0(r16)
	lhz r24, 0(r16)
	ld r25, 0(r16)
	stw r26, 0(r17)
.Lrestore:
	mtctr r19
	sync
	mtmsrd r14, 0
// Both banks carry the same BE return gate; LE fetch after restoration parks in place.
	.byte 0x08, 0x00, 0x00, 0x48
	.byte 0x48, 0x00, 0x00, 0x08
	.byte 0x00, 0x00, 0x00, 0x48
	.byte 0x4c, 0x00, 0x01, 0x2c
	.byte 0x4e, 0x80, 0x04, 0x20
	.globl xsdk_hv_le_window_end
xsdk_hv_le_window_end:
