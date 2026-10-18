#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Listing a folder on a FAT32 USB stick (github.com/xenosworks/xsdk issue 1). Needs a stick in the console: it makes
// \Device\Mass0\xsdk_dirtest with full\a.txt, full\sub and empty, then:
//   1. queries full into a buffer of a pattern and logs which of an entry's fields the driver left as the pattern;
//   2. lists full and empty with readdir from a heap whose free blocks hold the pattern, as a title's heap holds old
//      bytes, checking each name and type; then idles 2 s, as the issue's crash came about a second later.
// docs/HARDWARE.md, "Listing a folder on a FAT32 USB stick".

enum {
	PATTERN = 0xA5,
	ENTRY_BYTES = 320,
	DIRTY_BLOCKS = 64,
	// About xsdk's DIR, so a freed block of the pattern fits one exactly (src/dir.c).
	DIRTY_BYTES = 592,
	PATH_BYTES = 128,
};

static const char *const folder = "\\Device\\Mass0\\xsdk_dirtest";

static bool make_folders(void)
{
	char path[PATH_BYTES];
	static const char *const names[] = { "", "\\full", "\\full\\sub", "\\empty" };
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		snprintf(path, sizeof path, "%s%s", folder, names[i]);
		if (mkdir(path, 0) != 0 && errno != EEXIST) {
			xsdk_log_write("usb dir probe: FAILED, mkdir(%s), errno %d: is a FAT32 stick in?", path, errno);
			return false;
		}
	}
	snprintf(path, sizeof path, "%s\\full\\a.txt", folder);
	FILE *file = fopen(path, "w");
	if (!file || fputs("xsdk usb dir probe\n", file) < 0 || fclose(file) != 0) {
		xsdk_log_write("usb dir probe: FAILED, writing %s", path);
		return false;
	}
	return true;
}

static bool is_pattern(const void *field, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++) {
		if (((const uint8_t *)field)[i] != PATTERN)
			return false;
	}
	return true;
}

// Each query into a buffer of the pattern: which fields the driver wrote.
static void query_fields(void)
{
	char path[PATH_BYTES];
	snprintf(path, sizeof path, "%s\\full", folder);
	ANSI_STRING name = {
		.length = (uint16_t)strlen(path), .maximum_length = (uint16_t)(strlen(path) + 1), .pointer = path
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = 0, .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	NTSTATUS status =
		NtCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("usb dir probe: NtCreateFile(%s) failed: 0x%08X", path, (unsigned)status);
		return;
	}
	static uint64_t buffer[ENTRY_BYTES / 8 * 64];
	static const ANSI_STRING all = { .length = 1, .maximum_length = 2, .pointer = "*" };
	for (int query = 0;; query++) {
		memset(buffer, PATTERN, sizeof buffer);
		status = NtQueryDirectoryFile(handle, 0, NULL, NULL, &io_status, (FILE_DIRECTORY_INFORMATION *)buffer,
			sizeof buffer, query ? NULL : &all, query == 0);
		if (!NT_SUCCESS(status))
			break;
		const FILE_DIRECTORY_INFORMATION *e = (const FILE_DIRECTORY_INFORMATION *)buffer;
		xsdk_log_write("usb dir probe: query %d into %u bytes wrote %u: '%.*s'; next_entry_offset %s, file_index %s",
			query, (unsigned)sizeof buffer, (unsigned)io_status.information, (int)e->file_name_length, e->file_name,
			is_pattern(&e->next_entry_offset, 4) ? "left" : "written",
			is_pattern(&e->file_index, 4) ? "left" : "written");
	}
	xsdk_log_write("usb dir probe: the queries ended with 0x%08X", (unsigned)status);
	(void)NtClose(handle);
}

// The heap's free blocks the size of a DIR hold the pattern, so opendir gets old bytes.
static void dirty_heap(void)
{
	void *blocks[DIRTY_BLOCKS];
	for (int i = 0; i < DIRTY_BLOCKS; i++) {
		blocks[i] = malloc(DIRTY_BYTES);
		if (blocks[i])
			memset(blocks[i], PATTERN, DIRTY_BYTES);
	}
	for (int i = DIRTY_BLOCKS - 1; i >= 0; i--)
		free(blocks[i]);
}

// readdir over the folder: its names joined, and whether each is a folder.
static int list(const char *name, char *names, size_t size, size_t *dirty)
{
	char path[PATH_BYTES];
	snprintf(path, sizeof path, "%s\\%s", folder, name);
	dirty_heap();
	DIR *dir = opendir(path);
	if (!dir)
		return -1;
	// How much of the DIR's memory held the pattern before the first query: the check means something only if most did.
	*dirty = 0;
	for (size_t i = 0; i < malloc_usable_size(dir); i++)
		*dirty += ((const uint8_t *)dir)[i] == PATTERN;
	names[0] = '\0';
	int count = 0;
	for (struct dirent *e; (e = readdir(dir)) != NULL && count < 8; count++) {
		const size_t used = strlen(names);
		snprintf(names + used, size - used, "%s%s%s", count ? "," : "", e->d_name, e->d_type == DT_DIR ? "/" : "");
	}
	(void)closedir(dir);
	return count;
}

int main(void)
{
	if (!make_folders())
		return 1;
	query_fields();
	char names[256];
	size_t dirty_full, dirty_empty;
	const int full = list("full", names, sizeof names, &dirty_full);
	const bool full_right = full == 2 && (strcmp(names, "sub/,a.txt") == 0 || strcmp(names, "a.txt,sub/") == 0);
	xsdk_log_write("usb dir probe: readdir of full from a dirty heap (%u of the DIR's bytes old): %d entries, '%s'; %s",
		(unsigned)dirty_full, full, names, full_right ? "PASS" : "FAIL");
	const int empty = list("empty", names, sizeof names, &dirty_empty);
	xsdk_log_write("usb dir probe: readdir of empty from a dirty heap (%u of the DIR's bytes old): %d entries; %s",
		(unsigned)dirty_empty, empty, empty == 0 ? "PASS" : "FAIL");
	for (int i = 1; i <= 4; i++) {
		const struct timespec half = { 0, 500000000 };
		nanosleep(&half, NULL);
		xsdk_log_write("usb dir probe: alive %d.%d s after", i / 2, i % 2 * 5);
	}
	xsdk_log_write("usb dir probe: done");
	return 0;
}
