/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MEMWATCHER_MEMTEST_H
#define MEMWATCHER_MEMTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memwatcher_uapi.h"

struct mw_test_options {
	bool flush_cache;
	bool quick;
	size_t page_size;
	uint64_t physical_base;
};

int mw_run_memory_tests(void *memory, size_t length,
			const struct mw_test_options *options,
			struct mw_result *result);
bool mw_cache_flush_supported(void);

#endif
