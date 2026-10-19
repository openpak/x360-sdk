#ifndef XSDK_TEST_HV_LE_H
#define XSDK_TEST_HV_LE_H

#define HV_LE_PING    0
#define HV_LE_CONTROL 1
#define HV_LE_BE_DATA 2
#define HV_LE_FETCH   3
#define HV_LE_DATA    4
#define HV_LE_READ    5
#define HV_LE_STACK   6

#define HV_LE_COMPLETE     0
#define HV_LE_OPERATION    1
#define HV_LE_BEFORE       2
#define HV_LE_REQUESTED    3
#define HV_LE_INSIDE       4
#define HV_LE_AFTER        5
#define HV_LE_FETCH_ORDER  6
#define HV_LE_SENTINEL     7
#define HV_LE_WORD         8
#define HV_LE_HALF         9
#define HV_LE_DOUBLE       10
#define HV_LE_STORED_BYTES 11
#define HV_LE_FIELDS       12

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t value;
	uint64_t before;
	uint64_t after;
	uint64_t damaged_registers;
} HvCallResult;

_Static_assert(offsetof(HvCallResult, before) == 8 && offsetof(HvCallResult, after) == 16 &&
		offsetof(HvCallResult, damaged_registers) == 24 && sizeof(HvCallResult) == 32,
	"HV call assembly offsets");

void xsdk_hv_le_call(HvCallResult *result, uint32_t service, uint64_t first, uint64_t second, uint64_t third);
void xsdk_hv_le_flush(const void *address, uint32_t size_bytes);
extern const uint8_t xsdk_hv_le_expansion[];
#endif

#endif
