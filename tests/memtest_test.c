// SPDX-License-Identifier: GPL-2.0-only
#include "memtest.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
	struct mw_test_options options = {
		.flush_cache = false,
		.quick = true,
		.page_size = 4096,
		.physical_base = UINT64_C(0x100000),
	};
	struct mw_result result;
	void *memory = NULL;

	_Static_assert(sizeof(struct mw_info) == 56, "mw_info ABI changed");
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
	puts("memtest unit tests passed");
	return 0;
}
