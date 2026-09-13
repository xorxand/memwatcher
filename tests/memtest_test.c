// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "badpages.h"
#include "memtest.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	struct mw_test_options options = {
		.flush_cache = false,
		.quick = true,
		.page_size = 4096,
		.physical_base = UINT64_C(0x100000),
	};
	struct mw_result result;
	struct mw_bad_page_db database;
	uint64_t bad_pfns[] = { UINT64_C(0x12), UINT64_C(0x34), UINT64_C(0x12) };
	char bad_page_path[] = "/tmp/memwatcher-bad-pages-XXXXXX";
	void *memory = NULL;
	int bad_page_fd;

	_Static_assert(sizeof(struct mw_info) == 64, "mw_info ABI changed");
	_Static_assert(sizeof(struct mw_claim) == 16, "mw_claim ABI changed");
	_Static_assert(sizeof(struct mw_result) == 296, "mw_result ABI changed");
	_Static_assert(sizeof(struct mw_quarantine_query) == 24,
		       "mw_quarantine_query ABI changed");

	memory = aligned_alloc(4096, 2 * 1024 * 1024);
	assert(memory != NULL);
	memset(memory, 0xa5, 2 * 1024 * 1024);
	assert(mw_run_memory_tests(memory, 2 * 1024 * 1024, &options, &result) == 0);
	assert(result.bad_count == 0);
	assert(mw_run_memory_tests(NULL, 4096, &options, &result) < 0);
	free(memory);

	bad_page_fd = mkstemp(bad_page_path);
	assert(bad_page_fd >= 0);
	assert(close(bad_page_fd) == 0);
	assert(mw_bad_pages_load(bad_page_path, &database) == 0);
	assert(database.count == 0);
	mw_bad_pages_free(&database);
	assert(mw_bad_pages_append(bad_page_path, bad_pfns,
				   sizeof(bad_pfns) / sizeof(bad_pfns[0])) == 0);
	assert(mw_bad_pages_load(bad_page_path, &database) == 0);
	assert(database.count == 2);
	assert(mw_bad_pages_contains(&database, UINT64_C(0x12)));
	assert(mw_bad_pages_contains(&database, UINT64_C(0x34)));
	assert(!mw_bad_pages_contains(&database, UINT64_C(0x56)));
	mw_bad_pages_free(&database);
	assert(chmod(bad_page_path, 0666) == 0);
	errno = 0;
	assert(mw_bad_pages_load(bad_page_path, &database) < 0);
	assert(errno == EPERM);
	assert(unlink(bad_page_path) == 0);
	puts("unit tests passed");
	return 0;
}
