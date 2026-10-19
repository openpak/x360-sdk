#ifndef XSDK_TEST_MSR_LE_H
#define XSDK_TEST_MSR_LE_H

#define LE_FETCH          0
#define LE_WORD           1
#define LE_HALF           2
#define LE_DOUBLE         3
#define LE_STORE          4
#define LE_FLOAT_LOAD     5
#define LE_FLOAT_STORE    6
#define LE_MULTIPLE_LOAD  7
#define LE_MULTIPLE_STORE 8
#define LE_SPIN           9
#define LE_OBSERVE        10
#define LE_QUERY          11
#define LE_WAIT           12
#define LE_FAULT_CONTROL  13

#define LE_MTMSRD  0
#define LE_RFID    1
#define LE_CONTROL 2

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t before;
	uint64_t inside;
	uint64_t after;
	uint64_t value;
	uint64_t auxiliary;
	uint64_t passes;
	uint64_t errors;
	double floating;
	uint32_t fetch;
	uint32_t frame;
	uint32_t completed;
	uint32_t call_start_ticks;
	uint32_t call_end_ticks;
} MsrLeResult;

typedef struct {
	const void *source;
	void *destination;
	uint32_t operation;
	uint32_t method;
	uint32_t ticks;
	volatile uint8_t *running;
	void *target;
	uint32_t expected;
	uint32_t event;
	const int64_t *timeout;
} MsrLeRequest;

_Static_assert(offsetof(MsrLeResult, before) == 0 && offsetof(MsrLeResult, inside) == 8 &&
		offsetof(MsrLeResult, after) == 16 && offsetof(MsrLeResult, value) == 24 &&
		offsetof(MsrLeResult, auxiliary) == 32 && offsetof(MsrLeResult, passes) == 40 &&
		offsetof(MsrLeResult, errors) == 48 && offsetof(MsrLeResult, floating) == 56 &&
		offsetof(MsrLeResult, fetch) == 64 && offsetof(MsrLeResult, frame) == 68 &&
		offsetof(MsrLeResult, completed) == 72 && offsetof(MsrLeResult, call_start_ticks) == 76 &&
		offsetof(MsrLeResult, call_end_ticks) == 80,
	"assembly result offsets");
_Static_assert(offsetof(MsrLeRequest, source) == 0 && offsetof(MsrLeRequest, destination) == 4 &&
		offsetof(MsrLeRequest, operation) == 8 && offsetof(MsrLeRequest, method) == 12 &&
		offsetof(MsrLeRequest, ticks) == 16 && offsetof(MsrLeRequest, running) == 20 &&
		offsetof(MsrLeRequest, target) == 24 && offsetof(MsrLeRequest, expected) == 28 &&
		offsetof(MsrLeRequest, event) == 32 && offsetof(MsrLeRequest, timeout) == 36 && sizeof(MsrLeRequest) == 40,
	"assembly request offsets");

void xsdk_msr_le_run(MsrLeResult *result, const MsrLeRequest *request);
void xsdk_msr_le_stack(uint32_t bounds[2]);
extern const char xsdk_msr_le_read_site[], xsdk_msr_le_mtmsrd_site[];
extern const char xsdk_msr_le_rfid_begin[], xsdk_msr_le_rfid_end[], xsdk_msr_le_entry_fault[];
extern const char xsdk_msr_le_gate[], xsdk_msr_le_le_payload[], xsdk_msr_le_le_payload_end[];
extern const char xsdk_msr_le_payload[], xsdk_msr_le_payload_end[];
extern const char xsdk_msr_le_recover[], xsdk_msr_le_return[], xsdk_msr_le_unwind[];
extern const char xsdk_msr_le_fault_site[];
#endif

#endif
