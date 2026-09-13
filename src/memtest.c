// SPDX-License-Identifier: GPL-2.0-only
#include "memtest.h"

#include <errno.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <emmintrin.h>
#define MW_CPUID_CLFLUSH (1U << 19)
#endif

struct test_context {
	volatile uint64_t *words;
	size_t word_count;
	size_t length;
	struct mw_test_options options;
	struct mw_result *result;
};

bool mw_cache_flush_supported(void)
{
#if defined(__x86_64__) || defined(__i386__)
	unsigned int eax, ebx, ecx, edx;
	return __get_cpuid(1, &eax, &ebx, &ecx, &edx) &&
	       (edx & MW_CPUID_CLFLUSH);
#else
	return false;
#endif
}

static size_t cache_line_size(void)
{
#if defined(__x86_64__) || defined(__i386__)
	unsigned int eax, ebx, ecx, edx;
	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) &&
	    (edx & MW_CPUID_CLFLUSH)) {
		size_t size = ((ebx >> 8) & 0xffU) * 8U;
		return size ? size : 64U;
	}
#endif
	return 64U;
}

static void flush_region(struct test_context *ctx)
{
	size_t offset, line;

	if (!ctx->options.flush_cache)
		return;
#if defined(__x86_64__) || defined(__i386__)
	line = cache_line_size();
	for (offset = 0; offset < ctx->length; offset += line)
		_mm_clflush((const void *)((const char *)ctx->words + offset));
	_mm_mfence();
#else
	(void)offset;
	(void)line;
#endif
}

static void record_mismatch(struct test_context *ctx, size_t word,
			    uint64_t expected, uint64_t observed)
{
	uint32_t page = (uint32_t)((word * sizeof(uint64_t)) /
					 ctx->options.page_size);
	uint32_t i;

	if (!ctx->result->bad_count) {
		ctx->result->first_bad_byte = word * sizeof(uint64_t);
		ctx->result->expected = expected;
		ctx->result->observed = observed;
	}
	for (i = 0; i < ctx->result->bad_count; i++)
		if (ctx->result->bad_page_offsets[i] == page)
			return;
	if (ctx->result->bad_count == MW_MAX_BAD_PAGES) {
		ctx->result->flags |= MW_RESULT_F_QUARANTINE_ALL;
		return;
	}
	ctx->result->bad_page_offsets[ctx->result->bad_count++] = page;
}

static void fill(struct test_context *ctx, uint64_t value)
{
	size_t i;
	for (i = 0; i < ctx->word_count; i++)
		ctx->words[i] = value;
}

static void verify(struct test_context *ctx, uint64_t value)
{
	size_t i;
	flush_region(ctx);
	for (i = 0; i < ctx->word_count; i++) {
		uint64_t observed = ctx->words[i];
		if (observed != value)
			record_mismatch(ctx, i, value, observed);
	}
}

static void fixed_pattern(struct test_context *ctx, uint64_t value)
{
	fill(ctx, value);
	verify(ctx, value);
}

static uint64_t mix64(uint64_t value)
{
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

static void address_pattern(struct test_context *ctx, bool inverted)
{
	size_t i;
	for (i = 0; i < ctx->word_count; i++) {
		uint64_t expected = mix64(ctx->options.physical_base +
					  i * sizeof(uint64_t));
		ctx->words[i] = inverted ? ~expected : expected;
	}
	flush_region(ctx);
	for (i = 0; i < ctx->word_count; i++) {
		uint64_t expected = mix64(ctx->options.physical_base +
					  i * sizeof(uint64_t));
		uint64_t observed;
		if (inverted)
			expected = ~expected;
		observed = ctx->words[i];
		if (observed != expected)
			record_mismatch(ctx, i, expected, observed);
	}
}

static void march_up(struct test_context *ctx, uint64_t expected,
			 uint64_t replacement)
{
	size_t i;
	flush_region(ctx);
	for (i = 0; i < ctx->word_count; i++) {
		uint64_t observed = ctx->words[i];
		if (observed != expected)
			record_mismatch(ctx, i, expected, observed);
		ctx->words[i] = replacement;
	}
}

static void march_down(struct test_context *ctx, uint64_t expected,
			   uint64_t replacement)
{
	size_t i;
	flush_region(ctx);
	for (i = ctx->word_count; i-- > 0;) {
		uint64_t observed = ctx->words[i];
		if (observed != expected)
			record_mismatch(ctx, i, expected, observed);
		ctx->words[i] = replacement;
	}
}

static void march_c_minus(struct test_context *ctx)
{
	fill(ctx, 0);
	march_up(ctx, 0, UINT64_MAX);
	march_up(ctx, UINT64_MAX, 0);
	march_down(ctx, 0, UINT64_MAX);
	march_down(ctx, UINT64_MAX, 0);
	verify(ctx, 0);
}

int mw_run_memory_tests(void *memory, size_t length,
			const struct mw_test_options *options,
			struct mw_result *result)
{
	struct test_context ctx;
	static const uint64_t patterns[] = {
		UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff),
		UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555),
		UINT64_C(0x3333333333333333), UINT64_C(0xcccccccccccccccc),
		UINT64_C(0x0f0f0f0f0f0f0f0f), UINT64_C(0xf0f0f0f0f0f0f0f0),
	};
	size_t i, pattern_count;

	if (!memory || !options || !result || !options->page_size ||
	    !length || length % sizeof(uint64_t) || length % options->page_size)
		return -EINVAL;
	if (options->flush_cache && !mw_cache_flush_supported())
		return -EOPNOTSUPP;

	memset(result, 0, sizeof(*result));
	ctx.words = memory;
	ctx.word_count = length / sizeof(uint64_t);
	ctx.length = length;
	ctx.options = *options;
	ctx.result = result;

	pattern_count = options->quick ? 4 : sizeof(patterns) / sizeof(patterns[0]);
	for (i = 0; i < pattern_count; i++)
		fixed_pattern(&ctx, patterns[i]);
	march_c_minus(&ctx);
	address_pattern(&ctx, false);
	address_pattern(&ctx, true);
	/* Leave a known, verified value before the kernel can reuse good pages. */
	fixed_pattern(&ctx, 0);
	flush_region(&ctx);

	return result->bad_count || (result->flags & MW_RESULT_F_QUARANTINE_ALL)
		? 1 : 0;
}
