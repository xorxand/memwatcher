// SPDX-License-Identifier: GPL-2.0-only
#ifndef MEMWATCHER_BADPAGES_H
#define MEMWATCHER_BADPAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MW_BAD_PAGE_FILE_HEADER "# memwatcher-bad-pages-v1"
#define MW_MAX_PERSISTED_BAD_PAGES 65536U

struct mw_bad_page_db {
	uint64_t *pfns;
	size_t count;
	size_t capacity;
};

int mw_bad_pages_load(const char *path, struct mw_bad_page_db *database);
int mw_bad_pages_append(const char *path, const uint64_t *pfns, size_t count);
bool mw_bad_pages_contains(const struct mw_bad_page_db *database, uint64_t pfn);
int mw_bad_pages_add(struct mw_bad_page_db *database, uint64_t pfn);
void mw_bad_pages_free(struct mw_bad_page_db *database);

#endif
