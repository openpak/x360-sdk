#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hv_le.h"
#include "hv_le_image.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

static const uint64_t hv_bit = UINT64_C(0x1000000000000000);
static uint32_t call_service;

static bool check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	return passed;
}

static bool clean_return(const HvCallResult *result)
{
	return result->before == result->after && !(result->after & 1) && !result->damaged_registers;
}

static bool invoke(const char *name, uint32_t service, uint64_t first, uint64_t second, uint64_t third, uint64_t *value)
{
	HvCallResult result;
	xsdk_log_write("BEGIN %s: service=0x%02X argument=%016llX", name, (unsigned)service, (unsigned long long)second);
	xsdk_hv_le_call(&result, service, first, second, third);
	xsdk_log_write("END %s: value=%016llX title MSR=%016llX -> %016llX register damage=%016llX", name,
		(unsigned long long)result.value, (unsigned long long)result.before, (unsigned long long)result.after,
		(unsigned long long)result.damaged_registers);
	*value = result.value;
	return check("HV call returns with unchanged title MSR, nonvolatile GPRs, CR2-4, LR, r2/r13 and stack",
		clean_return(&result));
}

static bool install(uint32_t service)
{
	void *const image = MmAllocatePhysicalMemoryEx(0, HV_LE_IMAGE_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 4096);
	if (!image) {
		xsdk_log_write("MmAllocatePhysicalMemoryEx(HV expansion) returned NULL");
		return false;
	}
	const uint32_t physical = MmGetPhysicalAddress(image);
	if (physical & 4095) {
		xsdk_log_write("FAIL: expansion physical address is not page aligned: 0x%08X", (unsigned)physical);
		MmFreePhysicalMemory(0, image);
		return false;
	}
	memcpy(image, xsdk_hv_le_expansion, HV_LE_IMAGE_BYTES);
	xsdk_hv_le_flush(image, HV_LE_IMAGE_BYTES);
	uint64_t status;
	const bool returned = invoke("install expansion", service, physical, HV_LE_IMAGE_BYTES, 0, &status);
	MmFreePhysicalMemory(0, image);
	if (!returned || status != 0) {
		xsdk_log_write("BLOCKED: expansion installation status=%016llX; no payload call or LE request follows",
			(unsigned long long)status);
		return false;
	}
	return check("source-built expansion installed", true);
}

static bool control(void)
{
	uint64_t value;
	if (!invoke("payload identity", call_service, HV_LE_IMAGE_ID, HV_LE_PING, 0, &value) ||
		!check("payload identity and 64-bit result ABI", value == UINT64_C(0x48564c4500000001)))
		return false;
	if (!invoke("HV control (mfmsr only)", call_service, HV_LE_IMAGE_ID, HV_LE_CONTROL, 0, &value))
		return false;
	xsdk_log_write("HV entry MSR=%016llX HV=%u LE=%u EE=%u SF=%u", (unsigned long long)value,
		(unsigned)((value >> 60) & 1), (unsigned)(value & 1), (unsigned)((value >> 15) & 1), (unsigned)(value >> 63));
	if (!check("HV=1, LE=0 inside the expansion, followed by a clean title return", (value & (hv_bit | 1)) == hv_bit))
		return false;
	if (!invoke("HV stack (register only)", call_service, HV_LE_IMAGE_ID, HV_LE_STACK, 0, &value))
		return false;
	xsdk_log_write(
		"HV stack=%016llX mod8=%u mod16=%u", (unsigned long long)value, (unsigned)(value & 7), (unsigned)(value & 15));
	if (!check("HV stack supports aligned 64-bit saves", (value & 7) == 0))
		return false;
	return invoke("report bounds control", call_service, HV_LE_IMAGE_ID, HV_LE_READ, HV_LE_FIELDS, &value) &&
		check("report getter rejects an out-of-range index", value == UINT64_MAX);
}

static bool read_report(uint64_t report[HV_LE_FIELDS])
{
	xsdk_log_write("BEGIN bounded report read: %u fields", HV_LE_FIELDS);
	for (uint32_t field = 0; field < HV_LE_FIELDS; field++) {
		HvCallResult result;
		xsdk_hv_le_call(&result, call_service, HV_LE_IMAGE_ID, HV_LE_READ, field);
		if (!clean_return(&result)) {
			xsdk_log_write("FAIL: report field %u return MSR=%016llX -> %016llX register damage=%016llX",
				(unsigned)field, (unsigned long long)result.before, (unsigned long long)result.after,
				(unsigned long long)result.damaged_registers);
			return false;
		}
		report[field] = result.value;
	}
	return check("all report reads returned cleanly", true);
}

static bool run_case(const char *name, uint32_t operation, uint64_t report[HV_LE_FIELDS])
{
	uint64_t status;
	if (!invoke(name, call_service, HV_LE_IMAGE_ID, operation, 0, &status))
		return false;
	if (status != 0)
		xsdk_log_write(
			"Expansion rejection: -1 invalid operation/index; -2 HV clear; -3 LE already set; -4 stack unaligned");
	if (!check("expansion operation completed", status == 0) || !read_report(report))
		return false;
	xsdk_log_write("report: operation=%llu complete=%016llX fetch=%s sentinel=%016llX",
		(unsigned long long)report[HV_LE_OPERATION], (unsigned long long)report[HV_LE_COMPLETE],
		report[HV_LE_FETCH_ORDER] == 0       ? "BE words"
			: report[HV_LE_FETCH_ORDER] == 1 ? "LE words"
											 : "INVALID",
		(unsigned long long)report[HV_LE_SENTINEL]);
	xsdk_log_write("HV MSR: before=%016llX requested=%016llX inside=%016llX after=%016llX",
		(unsigned long long)report[HV_LE_BEFORE], (unsigned long long)report[HV_LE_REQUESTED],
		(unsigned long long)report[HV_LE_INSIDE], (unsigned long long)report[HV_LE_AFTER]);
	const uint64_t expected = (report[HV_LE_BEFORE] & ~UINT64_C(0x8000)) | (operation != HV_LE_BE_DATA);
	return check("HV report records the requested MSR, a fetch sentinel, and exact restoration",
		report[HV_LE_COMPLETE] == UINT64_C(0x48564c45) && report[HV_LE_OPERATION] == operation &&
			(report[HV_LE_BEFORE] & (hv_bit | 1)) == hv_bit && report[HV_LE_REQUESTED] == expected &&
			report[HV_LE_INSIDE] == expected && report[HV_LE_AFTER] == report[HV_LE_BEFORE] &&
			report[HV_LE_FETCH_ORDER] <= 1 && report[HV_LE_SENTINEL] == UINT64_C(0x13579bdf));
}

static bool data_values(const uint64_t report[HV_LE_FIELDS], bool little)
{
	const uint64_t stored = report[HV_LE_STORED_BYTES];
	xsdk_log_write("lwz=%016llX lhz=%016llX ld=%016llX", (unsigned long long)report[HV_LE_WORD],
		(unsigned long long)report[HV_LE_HALF], (unsigned long long)report[HV_LE_DOUBLE]);
	xsdk_log_write("stw 11223344, read with lbz after restoring BE: %02X %02X %02X %02X | %02X %02X %02X %02X",
		(unsigned)(stored >> 56), (unsigned)((stored >> 48) & 255), (unsigned)((stored >> 40) & 255),
		(unsigned)((stored >> 32) & 255), (unsigned)((stored >> 24) & 255), (unsigned)((stored >> 16) & 255),
		(unsigned)((stored >> 8) & 255), (unsigned)(stored & 255));
	return check(little ? "LE aligned lwz/lhz/ld/stw values" : "BE aligned lwz/lhz/ld/stw controls",
		report[HV_LE_WORD] == (little ? UINT64_C(0x44332211) : UINT64_C(0x11223344)) &&
			report[HV_LE_HALF] == (little ? UINT64_C(0x2211) : UINT64_C(0x1122)) &&
			report[HV_LE_DOUBLE] == (little ? UINT64_C(0x8877665544332211) : UINT64_C(0x1122334455667788)) &&
			stored == (little ? UINT64_C(0x4433221100000000) : UINT64_C(0x1122334400000000)));
}

static bool read_mode(char profile[16], char mode[16])
{
	strcpy(profile, "retail");
	strcpy(mode, "control");
	FILE *const file = fopen("probe_hv_le.txt", "r");
	if (!file) {
		if (errno == ENOENT)
			return true;
		xsdk_log_write("fopen(probe_hv_le.txt) failed: errno=%d", errno);
		return false;
	}
	char extra;
	const int fields = fscanf(file, "%15s %15s %c", profile, mode, &extra);
	const bool read_ok = !ferror(file);
	const int closed = fclose(file);
	return read_ok && closed == 0 && fields == 2 &&
		(strcmp(profile, "retail") == 0 || strcmp(profile, "devkit") == 0) &&
		(strcmp(mode, "control") == 0 || strcmp(mode, "le") == 0);
}

int main(void)
{
	char profile[16], mode[16];
	if (!read_mode(profile, mode)) {
		xsdk_log_write("FAIL: probe_hv_le.txt must contain retail/devkit followed by control/le");
		return 1;
	}
	const bool devkit = strcmp(profile, "devkit") == 0;
	const uint32_t install_service = devkit ? 0x70 : 0x72;
	call_service = devkit ? 0x71 : 0x73;
	xsdk_log_write("probe_hv_le v2: profile=%s mode=%s expansion=%08X payload SHA256=%s", profile, mode,
		(unsigned)HV_LE_IMAGE_ID, HV_LE_IMAGE_SHA256);
	xsdk_log_write("Unsigned plaintext expansion: firmware must already accept it; this probe changes no firmware");
	xsdk_log_write("Services install=%02X call=%02X; no fault injection or title exception handler",
		(unsigned)install_service, (unsigned)call_service);
	const NTSTATUS moved = xsdk_thread_move(0);
	if (!NT_SUCCESS(moved)) {
		xsdk_log_write("xsdk_thread_move(0) failed: 0x%08X", (unsigned)moved);
		return 1;
	}
	if (!install(install_service) || !control())
		return 1;
	uint64_t report[HV_LE_FIELDS];
	if (!run_case("BE data control", HV_LE_BE_DATA, report) ||
		!check("BE control fetched BE words", report[HV_LE_FETCH_ORDER] == 0) || !data_values(report, false))
		return 1;
	if (strcmp(mode, "control") == 0) {
		xsdk_log_write("PASS: control stage complete; no LE request was made. Next configuration: %s le", profile);
		return 0;
	}
	if (!run_case("LE fetch (registers only)", HV_LE_FETCH, report)) {
		xsdk_log_write("BLOCKED: LE fetch/readback/restoration did not pass; no LE data access follows");
		return 1;
	}
	const uint64_t fetch = report[HV_LE_FETCH_ORDER];
	if (!run_case("LE aligned data", HV_LE_DATA, report) ||
		!check("fetch order is stable", report[HV_LE_FETCH_ORDER] == fetch) || !data_values(report, true))
		return 1;
	xsdk_log_write("PASS: HV control, LE fetch and aligned integer data; every call returned to the title in BE");
	xsdk_log_write(
		"Title-side LE, FP, unaligned/multiple accesses, interrupt persistence and switch cost remain untested");
	return 0;
}
