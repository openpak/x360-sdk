#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msr_le.h"
#include "probe.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	// Hook and context layout measured in docs/HARDWARE.md, "How the kernel hands a title its faults".
	SET_HANDLER = 0x36D,
	CLEAR_HANDLER = 0x36E,
	CONTEXT_IAR = 8,
	CONTEXT_R1 = 0x20,
	CONTEXT_BYTES = 0x228,
	ALIGNMENT = 0x80000002,
	ILLEGAL_INSTRUCTION = 0xC000001D,
	// Console timing: docs/HARDWARE.md, "The time base runs at 49.875 MHz".
	TICKS_PER_SECOND = 49875000,
	CYCLES_PER_TICK = 64,
	SPIN_TICKS = 2 * TICKS_PER_SECOND,
	WORKER_TICKS = 4 * TICKS_PER_SECOND,
	SAMPLES = 17,
	ROUNDS = 256,
	BE_WORD = 0x11223344,
	LE_WORD_VALUE = 0x44332211,
};

typedef uint32_t (*SetHandler)(uint32_t first, uint32_t handler);
typedef uint32_t (*ClearHandler)(uint32_t handle);

typedef struct {
	uint32_t calls;
	uint32_t code;
	uint32_t address;
	uint32_t context_word;
} Fault;

typedef struct {
	MsrLeResult result;
	uint32_t hardware_thread;
	volatile uint8_t ready;
	NTSTATUS status;
} Observer;

static const uint8_t source[] __attribute__((aligned(16))) =
	"\x11\x22\x33\x44\x55\x66\x77\x88"
	"\x00\x00\x80\x3F\xA5\xA5\xA5\xA5"
	"\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5"
	"\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5";
static uint8_t destination[32] __attribute__((aligned(16)));
static uint32_t stack_bounds[2];
static MsrLeResult *active_result;
static Fault fault;
static uint32_t passed_count;
static uint32_t failed_count;
static uint32_t selected_method;
static uint32_t selected_fetch;
// Padding contains a legacy LE byte-address swizzle if the console implements one.
static volatile uint8_t running[8] __attribute__((aligned(8)));
static volatile uint8_t stop_observers;
static Observer observers[2];
static HANDLE wake_event;
static NTSTATUS signal_status;
static uint32_t signal_tick;
static const int64_t wait_timeout = -20000000;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static bool on_stack(uint32_t address, uint32_t bytes)
{
	return address >= stack_bounds[0] && address <= stack_bounds[1] && bytes <= stack_bounds[1] - address;
}

static bool between(uint32_t address, const char *begin, const char *end)
{
	return address >= (uint32_t)begin && address < (uint32_t)end;
}

static int32_t handle_fault(uint32_t pointers)
{
	if (!active_result || !on_stack(pointers, 8))
		return 0;
	const uint32_t *pair = (const uint32_t *)pointers;
	if (!on_stack(pair[0], 0x50) || !on_stack(pair[1], CONTEXT_BYTES))
		return 0;
	const uint32_t *record = (const uint32_t *)pair[0];
	uint8_t *context = (uint8_t *)pair[1];
	const uint32_t iar = *(uint32_t *)(context + CONTEXT_IAR);
	const uint64_t frame = *(uint64_t *)(context + CONTEXT_R1);
	xsdk_log_write("fault: code=0x%08X address=%08X IAR=%08X context+4=%08X frame=%016llX", record[0], record[3], iar,
		*(uint32_t *)(context + 4), (unsigned long long)frame);
	if (frame != active_result->frame || iar != record[3] || fault.calls != 0)
		return 0;
	uint32_t resume;
	if (iar == (uint32_t)xsdk_msr_le_read_site)
		resume = (uint32_t)xsdk_msr_le_unwind;
	else if (iar == (uint32_t)xsdk_msr_le_mtmsrd_site)
		resume = (uint32_t)xsdk_msr_le_return;
	else if (between(iar, xsdk_msr_le_rfid_begin, xsdk_msr_le_rfid_end))
		resume = (uint32_t)xsdk_msr_le_entry_fault;
	else if (iar == (uint32_t)xsdk_msr_le_fault_site || between(iar, xsdk_msr_le_gate, xsdk_msr_le_le_payload_end) ||
		between(iar, xsdk_msr_le_payload, xsdk_msr_le_payload_end))
		resume = (uint32_t)xsdk_msr_le_recover;
	else
		return 0;
	fault = (Fault){ 1, record[0], iar, *(uint32_t *)(context + 4) };
	*(uint32_t *)(context + CONTEXT_IAR) = resume;
	return -1;
}

static bool resolve_handlers(SetHandler *set, ClearHandler *clear)
{
	HANDLE kernel;
	const uint32_t error = XexGetModuleHandle("xboxkrnl.exe", &kernel);
	if (error) {
		xsdk_log_write("XexGetModuleHandle failed: 0x%08X", error);
		return false;
	}
	NTSTATUS status = XexGetProcedureAddress(kernel, SET_HANDLER, (void **)set);
	if (!NT_SUCCESS(status) || !*set) {
		xsdk_log_write("XexGetProcedureAddress(set handler) failed: 0x%08X", (unsigned)status);
		return false;
	}
	status = XexGetProcedureAddress(kernel, CLEAR_HANDLER, (void **)clear);
	if (!NT_SUCCESS(status) || !*clear) {
		xsdk_log_write("XexGetProcedureAddress(clear handler) failed: 0x%08X", (unsigned)status);
		return false;
	}
	return true;
}

static MsrLeRequest request(uint32_t operation, uint32_t method)
{
	return (MsrLeRequest){ .source = source,
		.destination = destination,
		.operation = operation,
		.method = method,
		.ticks = SPIN_TICKS,
		.running = running,
		.expected = method == LE_CONTROL ? BE_WORD : LE_WORD_VALUE };
}

static bool execute(MsrLeResult *result, const MsrLeRequest *work)
{
	*result = (MsrLeResult){ 0 };
	fault = (Fault){ 0 };
	active_result = result;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	xsdk_msr_le_run(result, work);
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	active_result = NULL;
	return result->completed && !fault.calls;
}

static bool run(const char *name, MsrLeResult *result, const MsrLeRequest *work)
{
	xsdk_log_write("BEGIN %s: method=%u operation=%u", name, work->method, work->operation);
	const bool completed = execute(result, work);
	xsdk_log_write("END %s: completed=%u faults=%u fetch=%s MSR=%016llX -> %016llX -> %016llX", name, result->completed,
		fault.calls, result->fetch ? "LE words" : "BE words", (unsigned long long)result->before,
		(unsigned long long)result->inside, (unsigned long long)result->after);
	xsdk_log_write("values: %016llX %016llX passes=%llu errors=%llu", (unsigned long long)result->value,
		(unsigned long long)result->auxiliary, (unsigned long long)result->passes, (unsigned long long)result->errors);
	return completed;
}

static bool kept_le(const MsrLeResult *result)
{
	return (result->inside & 1) && result->after == result->before && result->fetch == selected_fetch;
}

static bool discover(bool viable[2])
{
	MsrLeResult result;
	MsrLeRequest work = request(LE_FETCH, LE_CONTROL);
	bool ok = run("BE control", &result, &work);
	ok = ok && result.value == 0x13579BDF && !(result.before & 1) && result.before == result.after;
	check("BE control executes and mfmsr is accessible", ok);
	if (!ok)
		return false;
	xsdk_log_write("initial MSR: PR=%u EE=%u HV=%u; only LE is requested", (unsigned)((result.before >> 14) & 1),
		(unsigned)((result.before >> 15) & 1), (unsigned)((result.before >> 60) & 1));
	work.operation = LE_FAULT_CONTROL;
	(void)run("handler control", &result, &work);
	ok = result.completed && fault.calls == 1 && fault.code == ILLEGAL_INSTRUCTION &&
		fault.address == (uint32_t)xsdk_msr_le_fault_site && result.after == result.before;
	check("known illegal instruction reaches the handler and returns in BE", ok);
	if (!ok)
		return false;
	work.operation = LE_WORD;
	ok = run("BE lwz control", &result, &work);
	ok = ok && result.value == BE_WORD && !(result.inside & 1) && result.before == result.after;
	check("BE lwz sees 11 22 33 44 at the source address", ok);
	if (!ok)
		return false;
	for (uint32_t method = LE_MTMSRD; method <= LE_RFID; method++) {
		work = request(LE_FETCH, method);
		ok = run(method == LE_MTMSRD ? "mtmsrd LE fetch" : "rfid LE fetch", &result, &work);
		viable[method] = ok && (result.inside & 1) && result.after == result.before && result.value == 0x13579BDF;
		xsdk_log_write("OBS switch %u: %s", method, viable[method] ? "LE round trip observed" : "LE not established");
		if (viable[method] && (!viable[LE_MTMSRD] || method == LE_MTMSRD)) {
			selected_method = method;
			selected_fetch = result.fetch;
		}
	}
	check("at least one switch sets LE, runs its fetch path, and restores the original MSR", viable[0] || viable[1]);
	return viable[0] || viable[1];
}

static bool guard_after(uint32_t used)
{
	for (uint32_t i = used; i < sizeof destination; i++)
		if (destination[i] != 0xA5)
			return false;
	return true;
}

static void log_bytes(void)
{
	const volatile uint8_t *bytes = destination;
	xsdk_log_write("BE byte reads: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
		bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
		bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static bool data_accesses(void)
{
	const uint32_t failed_before = failed_count;
	const uint32_t operations[] = { LE_WORD, LE_HALF, LE_DOUBLE };
	const char *const names[] = { "lwz 0x44332211", "lhz 0x2211", "ld 0x8877665544332211" };
	const uint64_t expected[] = { 0x44332211, 0x2211, UINT64_C(0x8877665544332211) };
	MsrLeResult result;
	for (uint32_t i = 0; i < 3; i++) {
		const MsrLeRequest work = request(operations[i], selected_method);
		const bool ok = run(names[i], &result, &work);
		check(names[i], ok && kept_le(&result) && result.value == expected[i]);
	}
	MsrLeRequest work = request(LE_STORE, selected_method);
	memset(destination, 0xA5, sizeof destination);
	bool ok = run("stw", &result, &work);
	log_bytes();
	const uint8_t stored[] = { 0x44, 0x33, 0x22, 0x11 };
	check("stw leaves 44 33 22 11, with the rest untouched",
		ok && kept_le(&result) && memcmp(destination, stored, sizeof stored) == 0 && guard_after(4));
	work.operation = LE_FLOAT_LOAD;
	ok = run("lfs LE bytes for 1.0", &result, &work);
	xsdk_log_write("lfs result: %.17g", result.floating);
	check("lfs reads 00 00 80 3F as 1.0", ok && kept_le(&result) && result.floating == 1.0);
	work.operation = LE_FLOAT_STORE;
	memset(destination, 0xA5, sizeof destination);
	ok = run("stfs 1.0", &result, &work);
	log_bytes();
	const uint8_t floating[] = { 0x00, 0x00, 0x80, 0x3F };
	check("stfs writes 00 00 80 3F, with the rest untouched",
		ok && kept_le(&result) && memcmp(destination, floating, sizeof floating) == 0 && guard_after(4));
	const bool basic_accesses = failed_count == failed_before;
	for (uint32_t offset = 1; offset <= 3; offset++) {
		work = request(LE_WORD, selected_method);
		work.source = source + offset;
		char name[40];
		(void)snprintf(name, sizeof name, "unaligned lwz +%u", offset);
		ok = run(name, &result, &work);
		const uint32_t value = (uint32_t)source[offset] | (uint32_t)source[offset + 1] << 8 |
			(uint32_t)source[offset + 2] << 16 | (uint32_t)source[offset + 3] << 24;
		xsdk_log_write("OBS %s: %s, value=%08X fault=0x%08X", name, ok ? "executed" : "faulted", (unsigned)result.value,
			fault.code);
		check("unaligned lwz gives the LE value or a reported alignment fault",
			(ok && kept_le(&result) && result.value == value) ||
				(fault.calls == 1 && fault.code == ALIGNMENT && result.completed && result.after == result.before));
	}
	for (uint32_t operation = LE_MULTIPLE_LOAD; operation <= LE_MULTIPLE_STORE; operation++) {
		work = request(operation, selected_method);
		memset(destination, 0xA5, sizeof destination);
		(void)run(operation == LE_MULTIPLE_LOAD ? "lmw r30" : "stmw r30", &result, &work);
		if (operation == LE_MULTIPLE_STORE)
			log_bytes();
		check(operation == LE_MULTIPLE_LOAD ? "lmw raises alignment in LE" : "stmw raises alignment in LE",
			fault.calls == 1 && fault.code == ALIGNMENT && result.completed && (result.inside & 1) &&
				result.after == result.before);
	}
	return basic_accesses;
}

static uint32_t observe(void *argument)
{
	Observer *observer = argument;
	observer->hardware_thread = xsdk_thread_hardware_thread();
	__atomic_store_n(&observer->ready, 1, __ATOMIC_RELEASE);
	const uint32_t start = probe_time_base();
	while (!__atomic_load_n(&running[0], __ATOMIC_ACQUIRE) && !__atomic_load_n(&stop_observers, __ATOMIC_ACQUIRE)) {
		observer->status = xsdk_thread_sleep(1);
		if (!NT_SUCCESS(observer->status) || probe_time_base() - start >= WORKER_TICKS)
			return 1;
	}
	if (__atomic_load_n(&stop_observers, __ATOMIC_ACQUIRE))
		return 0;
	MsrLeRequest work = request(LE_OBSERVE, LE_CONTROL);
	work.ticks = WORKER_TICKS;
	xsdk_msr_le_run(&observer->result, &work);
	return 0;
}

static bool join(HANDLE thread)
{
	const int64_t timeout = -50000000;
	const NTSTATUS waited = NtWaitForSingleObjectEx(thread, USER_MODE, 0, &timeout);
	const NTSTATUS closed = NtClose(thread);
	if (waited != STATUS_SUCCESS)
		xsdk_log_write("NtWaitForSingleObjectEx(worker) failed: 0x%08X", (unsigned)waited);
	if (!NT_SUCCESS(closed))
		xsdk_log_write("NtClose(worker) failed: 0x%08X", (unsigned)closed);
	return waited == STATUS_SUCCESS && NT_SUCCESS(closed);
}

static void persistence(void)
{
	HANDLE threads[2];
	uint32_t created = 0;
	const uint32_t placements[] = { 0, 1 };
	bool ok = true;
	running[0] = 0;
	stop_observers = 0;
	memset(observers, 0, sizeof observers);
	for (uint32_t i = 0; i < 2; i++) {
		const NTSTATUS status = xsdk_thread_create_on(&threads[i], 0, observe, &observers[i], placements[i]);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("xsdk_thread_create_on(%u) failed: 0x%08X", placements[i], (unsigned)status);
			ok = false;
			break;
		}
		created++;
	}
	const uint32_t start = probe_time_base();
	while (ok &&
		(!__atomic_load_n(&observers[0].ready, __ATOMIC_ACQUIRE) ||
			!__atomic_load_n(&observers[1].ready, __ATOMIC_ACQUIRE))) {
		const NTSTATUS status = xsdk_thread_sleep(1);
		if (!NT_SUCCESS(status))
			xsdk_log_write("xsdk_thread_sleep(observer readiness) failed: 0x%08X", (unsigned)status);
		ok = NT_SUCCESS(status) && probe_time_base() - start < WORKER_TICKS;
	}
	MsrLeResult result = { 0 };
	uint32_t elapsed = 0;
	if (ok) {
		const MsrLeRequest work = request(LE_SPIN, selected_method);
		const uint32_t begin = probe_time_base();
		ok = run("two seconds of LE loads with interrupts enabled", &result, &work);
		elapsed = probe_time_base() - begin;
	}
	__atomic_store_n(&running[0], 0, __ATOMIC_RELEASE);
	__atomic_store_n(&stop_observers, 1, __ATOMIC_RELEASE);
	for (uint32_t i = 0; i < created; i++)
		ok = join(threads[i]) && ok;
	xsdk_log_write("spin: elapsed TB=%u, expected at least %u", elapsed, SPIN_TICKS);
	check("LE loads and MSR survive two seconds with EE set",
		ok && kept_le(&result) && (result.inside & 0x8000) && result.passes && !result.errors &&
			(result.auxiliary & 1) && elapsed >= SPIN_TICKS);
	for (uint32_t i = 0; i < 2; i++) {
		const Observer *observer = &observers[i];
		const MsrLeResult *seen = &observer->result;
		xsdk_log_write("observer %u: ran on %u, passes=%llu errors=%llu MSR=%016llX -> %016llX status=0x%08X", i,
			observer->hardware_thread, (unsigned long long)seen->passes, (unsigned long long)seen->errors,
			(unsigned long long)seen->inside, (unsigned long long)seen->auxiliary, (unsigned)observer->status);
		check(i == 0 ? "a BE thread ran on the same hardware thread during the LE spin"
					 : "a thread on sibling hardware thread 1 stayed BE during the LE spin",
			ok && NT_SUCCESS(observer->status) && observer->hardware_thread == placements[i] && seen->completed &&
				seen->passes && !seen->errors && !(seen->inside & 1) && !(seen->auxiliary & 1) &&
				seen->value == BE_WORD && seen->after == seen->before);
	}
	xsdk_log_write(
		"OBS: tick/interrupt persistence is inferred from elapsed time, EE, and observed scheduling; no vector "
		"counter");
}

static uint64_t read_time_bytes(bool little)
{
	uint64_t value = 0;
	for (uint32_t i = 0; i < 8; i++)
		value = value << 8 | destination[little ? 7 - i : i];
	return value;
}

static void query_boundary(void)
{
	uint64_t before;
	uint64_t after;
	KeQuerySystemTime(&before);
	MsrLeRequest work = request(LE_QUERY, selected_method);
	work.target = (void *)KeQuerySystemTime;
	MsrLeResult result;
	memset(destination, 0xA5, sizeof destination);
	const bool ok = run("direct KeQuerySystemTime while LE", &result, &work);
	KeQuerySystemTime(&after);
	log_bytes();
	const uint64_t be = read_time_bytes(false);
	const uint64_t le = read_time_bytes(true);
	const bool be_valid = before <= be && be <= after;
	const bool le_valid = before <= le && le <= after;
	xsdk_log_write("query: before=%llu after=%llu output as BE=%llu as LE=%llu", (unsigned long long)before,
		(unsigned long long)after, (unsigned long long)be, (unsigned long long)le);
	check("direct query returns a bounded timestamp, intact guards, LE MSR and an LE load",
		ok && kept_le(&result) && (result.auxiliary & 1) && result.passes == LE_WORD_VALUE && guard_after(8) &&
			(be_valid || le_valid));
	xsdk_log_write("OBS query output byte order: %s", be_valid ? "BE" : le_valid ? "LE" : "invalid");
}

static uint32_t signal_wait(void *unused)
{
	(void)unused;
	const uint32_t start = probe_time_base();
	while (!__atomic_load_n(&running[0], __ATOMIC_ACQUIRE)) {
		signal_status = xsdk_thread_sleep(1);
		if (!NT_SUCCESS(signal_status))
			return 1;
		if (probe_time_base() - start >= WORKER_TICKS) {
			signal_status = STATUS_TIMEOUT;
			return 1;
		}
	}
	signal_status = xsdk_thread_sleep(50);
	if (!NT_SUCCESS(signal_status))
		return 1;
	signal_tick = probe_time_base();
	signal_status = NtSetEvent(wake_event, NULL);
	return NT_SUCCESS(signal_status) ? 0 : 1;
}

static void wait_boundary(void)
{
	NTSTATUS status = NtCreateEvent(&wake_event, NULL, 0, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("NtCreateEvent failed: 0x%08X", (unsigned)status);
		check("wait event created", false);
		return;
	}
	HANDLE thread;
	for (uint32_t i = 0; i < sizeof running; i++)
		running[i] = 0;
	signal_tick = 0;
	signal_status = STATUS_NOT_IMPLEMENTED;
	status = xsdk_thread_create_on(&thread, 0, signal_wait, NULL, 2);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("xsdk_thread_create_on(signal) failed: 0x%08X", (unsigned)status);
		check("wait signaler created", false);
	} else {
		MsrLeRequest work = request(LE_WAIT, selected_method);
		work.target = (void *)NtWaitForSingleObjectEx;
		work.event = wake_event;
		work.timeout = &wait_timeout;
		MsrLeResult result;
		const bool ok = run("direct NtWaitForSingleObjectEx while LE, timeout stored BE", &result, &work);
		const uint32_t elapsed = result.call_end_ticks - result.call_start_ticks;
		__atomic_store_n(&running[0], 0, __ATOMIC_RELEASE);
		const bool joined = join(thread);
		xsdk_log_write("wait: elapsed TB=%u signal offset TB=%u signal status=0x%08X wait status=0x%08X", elapsed,
			signal_tick - result.call_start_ticks, (unsigned)signal_status, (unsigned)result.value);
		check("direct wait blocks until the event, then returns still LE with the correct load",
			ok && joined && signal_status == STATUS_SUCCESS && kept_le(&result) &&
				(uint32_t)result.value == STATUS_SUCCESS && (result.auxiliary & 1) && result.passes == LE_WORD_VALUE &&
				signal_tick - result.call_start_ticks <= elapsed && elapsed >= TICKS_PER_SECOND / 100);
	}
	status = NtClose(wake_event);
	if (!NT_SUCCESS(status))
		xsdk_log_write("NtClose(event) failed: 0x%08X", (unsigned)status);
	check("wait event closed", NT_SUCCESS(status));
}

static int compare_ticks(const void *first, const void *second)
{
	const int64_t a = *(const int64_t *)first;
	const int64_t b = *(const int64_t *)second;
	return (a > b) - (a < b);
}

static bool timed_batch(uint32_t method, int64_t *ticks)
{
	const MsrLeRequest work = request(LE_FETCH, method);
	MsrLeResult result;
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < ROUNDS; i++) {
		if (!execute(&result, &work) || result.before != result.after || result.value != 0x13579BDF ||
			(result.inside & 1) != (method != LE_CONTROL))
			return false;
	}
	*ticks = probe_time_base() - start;
	return true;
}

static void switch_cost(uint32_t method)
{
	int64_t baseline[SAMPLES];
	int64_t switched[SAMPLES];
	int64_t difference[SAMPLES];
	int64_t warm;
	xsdk_log_write("BEGIN timing method %u: %u samples of %u round trips", method, SAMPLES, ROUNDS);
	bool ok = timed_batch(LE_CONTROL, &warm) && timed_batch(method, &warm);
	for (uint32_t i = 0; ok && i < SAMPLES; i++) {
		ok = timed_batch(LE_CONTROL, &baseline[i]) && timed_batch(method, &switched[i]);
		if (ok)
			difference[i] = switched[i] - baseline[i];
	}
	check("timed switches all set LE, execute their payload, and restore BE", ok);
	if (!ok)
		return;
	qsort(baseline, SAMPLES, sizeof baseline[0], compare_ticks);
	qsort(switched, SAMPLES, sizeof switched[0], compare_ticks);
	qsort(difference, SAMPLES, sizeof difference[0], compare_ticks);
	xsdk_log_write("timing %u: batch TB baseline median=%lld, switched min/median/max=%lld/%lld/%lld, net median=%lld",
		method, (long long)baseline[SAMPLES / 2], (long long)switched[0], (long long)switched[SAMPLES / 2],
		(long long)switched[SAMPLES - 1], (long long)difference[SAMPLES / 2]);
	xsdk_log_write("timing %u: TB-derived cycles/round trip gross median=%lld, net min/median/max=%lld/%lld/%lld",
		method, (long long)(switched[SAMPLES / 2] * CYCLES_PER_TICK / ROUNDS),
		(long long)(difference[0] * CYCLES_PER_TICK / ROUNDS),
		(long long)(difference[SAMPLES / 2] * CYCLES_PER_TICK / ROUNDS),
		(long long)(difference[SAMPLES - 1] * CYCLES_PER_TICK / ROUNDS));
}

static bool read_mode(char mode[16])
{
	strcpy(mode, "all");
	FILE *file = fopen("probe_msr_le.txt", "r");
	if (!file) {
		if (errno == ENOENT)
			return true;
		xsdk_log_write("fopen(probe_msr_le.txt) failed: errno=%d", errno);
		return false;
	}
	const bool read = fgets(mode, 16, file) != NULL;
	const int closed = fclose(file);
	if (!read || closed != 0) {
		xsdk_log_write("reading/closing probe_msr_le.txt failed: errno=%d", errno);
		return false;
	}
	mode[strcspn(mode, "\r\n \t")] = '\0';
	return strcmp(mode, "all") == 0 || strcmp(mode, "core") == 0 || strcmp(mode, "query") == 0 ||
		strcmp(mode, "wait") == 0;
}

int main(void)
{
	char mode[16];
	if (!read_mode(mode)) {
		xsdk_log_write("probe_msr_le.txt must contain all, core, query, or wait");
		return 1;
	}
	xsdk_log_write("probe_msr_le: mode=%s; hardware experiment, no HV/HVX calls", mode);
	xsdk_log_write("Fetch is measured before data tests; C and logging run only after restoring BE");
	const NTSTATUS moved = xsdk_thread_move(0);
	if (!NT_SUCCESS(moved)) {
		xsdk_log_write("xsdk_thread_move(0) failed: 0x%08X", (unsigned)moved);
		return 1;
	}
	xsdk_msr_le_stack(stack_bounds);
	SetHandler set = NULL;
	ClearHandler clear = NULL;
	if (!resolve_handlers(&set, &clear))
		return 1;
	const uint32_t address = (uint32_t)handle_fault;
	const uint32_t hook = set(address, address);
	xsdk_log_write("set handler returned 0x%08X; the fault control verifies installation", hook);
	bool viable[2] = { false, false };
	if (discover(viable)) {
		xsdk_log_write("selected switch=%u fetch=%s", selected_method, selected_fetch ? "LE words" : "BE words");
		if (strcmp(mode, "all") == 0 || strcmp(mode, "core") == 0) {
			if (data_accesses())
				persistence();
			else
				xsdk_log_write(
					"BLOCKED persistence: basic LE loads/stores failed; shared byte addressing is not established");
			for (uint32_t method = 0; method < 2; method++)
				if (viable[method])
					switch_cost(method);
		}
		if (selected_fetch) {
			xsdk_log_write(
				"BLOCKED direct kernel imports: BE kernel instructions cannot be entered with flipped fetch");
		} else {
			if (strcmp(mode, "all") == 0 || strcmp(mode, "query") == 0)
				query_boundary();
			if (strcmp(mode, "all") == 0 || strcmp(mode, "wait") == 0)
				wait_boundary();
		}
	} else {
		xsdk_log_write("BLOCKED data/persistence/kernel/timing: no verified LE switch; no port decision inferred");
	}
	const uint32_t cleared = clear(hook);
	xsdk_log_write("clear handler returned 0x%08X", cleared);
	xsdk_log_write("probe_msr_le: %u passed, %u failed; raw sc untested (no established non-HV syscall ABI)",
		passed_count, failed_count);
	return failed_count ? 1 : 0;
}
