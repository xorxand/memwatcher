// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "badpages.h"
#include "memtest.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#define VERSION "1.1.0-dev"
#define DEFAULT_INTERVAL 10.0
#define DEFAULT_STATE "/var/lib/memwatcher/state.tsv"
#define DEFAULT_BAD_PAGES "/var/lib/memwatcher/bad-pages.tsv"

struct range { uint64_t start_pfn, end_pfn; };
struct ranges { struct range *items; size_t count, capacity; };

enum child_stage {
	CHILD_STARTED,
	CHILD_CLAIMED,
	CHILD_MAPPED,
	CHILD_TESTED,
	CHILD_COMPLETED,
};

struct child_report {
	uint32_t stage;
	int error;
	int test_status;
	struct mw_result result;
	uint64_t elapsed_ns;
};

static void usage(FILE *out)
{
	fprintf(out,
		"memwatcher %s\n"
		"Usage:\n"
		"  memwatcher info [--device PATH]\n"
		"  memwatcher selftest [--quick] [--no-cache-flush] [--mib N]\n"
		"  memwatcher preload --yes-i-understand [--bad-pages PATH]\n"
		"  memwatcher scan [options]\n\n"
		"Scan options:\n"
		"  --yes-i-understand    required acknowledgement for destructive tests\n"
		"  --device PATH         device node (default %s)\n"
		"  --state PATH          append-only ledger (default %s)\n"
		"  --bad-pages PATH      durable bad-PFN file (default %s)\n"
		"  --interval SECONDS    delay between pageblocks (default %.0f)\n"
		"  --passes N            sweeps to attempt (default 1)\n"
		"  --forever             repeat sweeps until stopped\n"
		"  --start-pfn PFN       restrict start PFN (must pair with --end-pfn)\n"
		"  --end-pfn PFN         restrict end PFN, exclusive\n"
		"  --quick               reduced pattern set\n"
		"  --no-cache-flush      permit cache-resident test (not recommended)\n"
		"  --dry-run             enumerate without opening the device\n"
		"  --allow-virtualized-preload\n"
		"                        override VM/container preload refusal\n",
		VERSION, MW_DEVICE_PATH, DEFAULT_STATE, DEFAULT_BAD_PAGES,
		DEFAULT_INTERVAL);
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;
	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || !*text || *end)
		return -1;
	*value = parsed;
	return 0;
}

static int parse_nonnegative_double(const char *text, double *value)
{
	char *end;
	double parsed;

	errno = 0;
	parsed = strtod(text, &end);
	if (errno || !*text || *end || !isfinite(parsed) || parsed < 0.0)
		return -1;
	*value = parsed;
	return 0;
}

static int align_pfn_up(uint64_t pfn, uint64_t alignment, uint64_t *aligned)
{
	uint64_t add;

	if (!alignment)
		return -1;
	add = (alignment - pfn % alignment) % alignment;
	if (pfn > UINT64_MAX - add)
		return -1;
	*aligned = pfn + add;
	return 0;
}

static int append_range(struct ranges *ranges, uint64_t start, uint64_t end)
{
	struct range *new_items;
	if (start >= end)
		return 0;
	if (ranges->count == ranges->capacity) {
		size_t capacity = ranges->capacity ? ranges->capacity * 2 : 32;
		new_items = realloc(ranges->items, capacity * sizeof(*new_items));
		if (!new_items)
			return -1;
		ranges->items = new_items;
		ranges->capacity = capacity;
	}
	ranges->items[ranges->count++] = (struct range){ start, end };
	return 0;
}

static int compare_ranges(const void *a, const void *b)
{
	const struct range *ra = a, *rb = b;
	return ra->start_pfn < rb->start_pfn ? -1 : ra->start_pfn > rb->start_pfn;
}

static int read_text(const char *path, char *buffer, size_t length)
{
	FILE *file = fopen(path, "re");
	if (!file)
		return -1;
	if (!fgets(buffer, (int)length, file)) {
		fclose(file);
		return -1;
	}
	fclose(file);
	buffer[strcspn(buffer, "\r\n")] = 0;
	return 0;
}

static int discover_ranges(struct ranges *ranges, size_t page_size)
{
	DIR *directory;
	struct dirent *entry;
	char text[128], path[PATH_MAX];
	uint64_t block_size;

	if (read_text("/sys/devices/system/memory/block_size_bytes", text,
		      sizeof(text))) {
		fprintf(stderr, "cannot read memory block size\n");
		return -1;
	}
	errno = 0;
	block_size = strtoull(text, NULL, 16); /* sysfs documents this as hex. */
	if (errno || !block_size) {
		fprintf(stderr, "invalid memory block size: %s\n", text);
		return -1;
	}
	directory = opendir("/sys/devices/system/memory");
	if (!directory) {
		perror("opendir memory sysfs");
		return -1;
	}
	while ((entry = readdir(directory))) {
		uint64_t index, start;
		if (strncmp(entry->d_name, "memory", 6) ||
		    entry->d_name[6] < '0' || entry->d_name[6] > '9')
			continue;
		snprintf(path, sizeof(path), "/sys/devices/system/memory/%s/state",
			 entry->d_name);
		if (read_text(path, text, sizeof(text)) || strcmp(text, "online"))
			continue;
		snprintf(path, sizeof(path), "/sys/devices/system/memory/%s/phys_index",
			 entry->d_name);
		if (read_text(path, text, sizeof(text)))
			continue;
		errno = 0;
		index = strtoull(text, NULL, 16);
		if (errno)
			continue;
		start = index * block_size / page_size;
		if (append_range(ranges, start, start + block_size / page_size)) {
			closedir(directory);
			return -1;
		}
	}
	closedir(directory);
	if (ranges->count)
		qsort(ranges->items, ranges->count, sizeof(*ranges->items),
		      compare_ranges);
	return ranges->count ? 0 : -1;
}

static int open_device(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC | O_SYNC);
	if (fd < 0)
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
	return fd;
}

static int get_info_fd(int fd, struct mw_info *info)
{
	memset(info, 0, sizeof(*info));
	if (ioctl(fd, MW_IOC_GET_INFO, info))
		return -1;
	if (info->abi_version != MW_ABI_VERSION) {
		errno = EPROTO;
		return -1;
	}
	if (info->page_size != (uint32_t)sysconf(_SC_PAGESIZE) ||
	    !info->pageblock_pages || info->max_bad_pages != MW_MAX_BAD_PAGES ||
	    info->pageblock_pages > SIZE_MAX / info->page_size) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static void print_quarantines(int fd, const struct mw_info *info)
{
	uint64_t i;
	for (i = 0; i < info->quarantined_pages - info->orphaned_quarantine_pages; i++) {
		struct mw_quarantine_query query = { .index = i };
		if (ioctl(fd, MW_IOC_GET_QUARANTINE, &query))
			break;
		printf("quarantine[%-4llu] pfn=0x%llx physical=0x%llx flags=0x%x\n",
		       (unsigned long long)i, (unsigned long long)query.pfn,
		       (unsigned long long)(query.pfn * info->page_size), query.flags);
	}
}

static int command_info(const char *device)
{
	struct mw_info info;
	int fd = open_device(device);
	if (fd < 0)
		return 1;
	if (get_info_fd(fd, &info)) {
		perror("MW_IOC_GET_INFO");
		close(fd);
		return 1;
	}
	printf("ABI: %u\npage size: %u bytes\npageblock: %u pages (%llu bytes)\n"
	       "claims: %llu successful, %llu failed\ncompleted tests: %llu pages\n"
	       "quarantined: %llu pages (%llu without metadata)\n"
	       "preloaded known-bad: %llu pages\n",
	       info.abi_version, info.page_size, info.pageblock_pages,
	       (unsigned long long)info.page_size * info.pageblock_pages,
	       (unsigned long long)info.successful_claims,
	       (unsigned long long)info.failed_claims,
	       (unsigned long long)info.tested_pages,
	       (unsigned long long)info.quarantined_pages,
	       (unsigned long long)info.orphaned_quarantine_pages,
	       (unsigned long long)info.preloaded_pages);
	print_quarantines(fd, &info);
	close(fd);
	return 0;
}

static int command_selftest(bool quick, bool flush_cache, uint64_t mib)
{
	struct mw_test_options options = {
		.flush_cache = flush_cache,
		.quick = quick,
		.page_size = (size_t)sysconf(_SC_PAGESIZE),
		.physical_base = UINT64_C(0x100000000),
	};
	struct mw_result result;
	size_t length;
	void *memory;
	uint64_t started;
	int ret;

	if (!mib || mib > SIZE_MAX / (1024U * 1024U))
		return 2;
	length = (size_t)mib * 1024U * 1024U;
	ret = posix_memalign(&memory, options.page_size, length);
	if (ret) {
		fprintf(stderr, "posix_memalign: %s\n", strerror(ret));
		return 1;
	}
	started = monotonic_ns();
	ret = mw_run_memory_tests(memory, length, &options, &result);
	printf("selftest: %s, %llu MiB, %.3f seconds, cache flush %s\n",
	       ret ? "FAILED" : "passed", (unsigned long long)mib,
	       (monotonic_ns() - started) / 1e9, flush_cache ? "on" : "off");
	free(memory);
	return ret ? 1 : 0;
}

static int drop_test_privileges(void)
{
	struct sched_param param = { 0 };
	(void)setpriority(PRIO_PROCESS, 0, 19);
#ifdef SCHED_IDLE
	(void)sched_setscheduler(0, SCHED_IDLE, &param);
#endif
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;
	if (geteuid() == 0) {
		if (setgroups(0, NULL) || setgid(65534) || setuid(65534))
			return -1;
	}
	return 0;
}

static int test_one_block(const char *device, const struct mw_info *info,
			  uint64_t pfn, bool quick, bool flush_cache,
			  struct child_report *report)
{
	struct mw_claim claim = {
		.start_pfn = pfn,
		.nr_pages = info->pageblock_pages,
	};
	struct mw_test_options options = {
		.flush_cache = flush_cache,
		.quick = quick,
		.page_size = info->page_size,
		.physical_base = pfn * info->page_size,
	};
	size_t length = (size_t)info->page_size * info->pageblock_pages;
	void *mapping;
	uint64_t started = monotonic_ns();
	int fd, ret;

	memset(report, 0, sizeof(*report));
	fd = open(device, O_RDWR | O_CLOEXEC | O_SYNC);
	if (fd < 0) {
		report->error = errno;
		return -1;
	}
	if (ioctl(fd, MW_IOC_CLAIM, &claim)) {
		report->error = errno;
		close(fd);
		return -1;
	}
	report->stage = CHILD_CLAIMED;
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		report->error = errno;
		(void)ioctl(fd, MW_IOC_QUARANTINE);
		close(fd);
		return -1;
	}
	report->stage = CHILD_MAPPED;
	if (drop_test_privileges()) {
		report->error = errno;
		munmap(mapping, length);
		(void)ioctl(fd, MW_IOC_QUARANTINE);
		close(fd);
		return -1;
	}
	report->test_status = mw_run_memory_tests(mapping, length, &options,
						     &report->result);
	report->stage = CHILD_TESTED;
	if (munmap(mapping, length)) {
		report->error = errno;
		close(fd); /* fail closed: release quarantines the whole claim */
		return -1;
	}
	if (report->test_status < 0) {
		report->result.flags |= MW_RESULT_F_QUARANTINE_ALL |
					MW_RESULT_F_INCOMPLETE;
		report->error = -report->test_status;
	}
	ret = ioctl(fd, MW_IOC_COMPLETE, &report->result);
	if (ret)
		report->error = errno;
	else
		report->stage = CHILD_COMPLETED;
	report->elapsed_ns = monotonic_ns() - started;
	close(fd);
	return ret ? -1 : 0;
}

static int run_child(const char *device, const struct mw_info *info,
		     uint64_t pfn, bool quick, bool flush_cache,
		     struct child_report *report, int *signal_number)
{
	struct child_report *shared;
	int status;
	pid_t child;
	pid_t waited;

	memset(report, 0, sizeof(*report));
	*signal_number = 0;
	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		return -1;
	memset(shared, 0, sizeof(*shared));
	child = fork();
	if (child < 0) {
		munmap(shared, sizeof(*shared));
		return -1;
	}
	if (!child) {
		int rc;

		(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
		rc = test_one_block(device, info, pfn, quick, flush_cache, shared);
		_exit(rc ? 1 : shared->test_status ? 10 : 0);
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0) {
		munmap(shared, sizeof(*shared));
		return -1;
	}
	memcpy(report, shared, sizeof(*report));
	munmap(shared, sizeof(*shared));
	if (WIFSIGNALED(status))
		*signal_number = WTERMSIG(status);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + *signal_number;
}

static FILE *open_ledger(const char *path)
{
	char directory[PATH_MAX], *slash;
	FILE *file;

	if (strlen(path) >= sizeof(directory)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	strcpy(directory, path);
	slash = strrchr(directory, '/');
	if (slash) {
		*slash = 0;
		if (*directory && mkdir(directory, 0750) && errno != EEXIST)
			return NULL;
	}
	file = fopen(path, "a+e");
	if (file)
		(void)fchmod(fileno(file), 0600);
	return file;
}

static void log_result(FILE *ledger, uint64_t pfn, const struct mw_info *info,
		       const struct child_report *report, int child_rc, int sig)
{
	struct timespec now;
	const char *status = sig ?
		(report->stage >= CHILD_CLAIMED ? "crashed-quarantined" :
		 "crashed-before-claim") :
		report->error ?
		(report->stage == CHILD_COMPLETED ? "test-error-quarantined" :
		 report->stage >= CHILD_CLAIMED ? "error-quarantined" : "skipped") :
		report->test_status ? "bad-quarantined" : "good";
	clock_gettime(CLOCK_REALTIME, &now);
	fprintf(ledger, "%lld\t0x%llx\t%u\t%s\t%u\t%d\t%d\t%.6f\n",
		(long long)now.tv_sec, (unsigned long long)pfn,
		info->pageblock_pages, status, report->result.bad_count,
		report->error, sig ? sig : child_rc, report->elapsed_ns / 1e9);
	fflush(ledger);
}

static void sleep_interval(double seconds)
{
	struct timespec req;
	if (seconds <= 0)
		return;
	req.tv_sec = (time_t)seconds;
	req.tv_nsec = (long)((seconds - req.tv_sec) * 1e9);
	while (nanosleep(&req, &req) && errno == EINTR) { }
}

static volatile sig_atomic_t stop_requested;
static void request_stop(int sig) { (void)sig; stop_requested = 1; }

static bool text_file_contains(const char *path, const char *needle)
{
	char buffer[4096];
	FILE *file = fopen(path, "re");
	size_t got;

	if (!file)
		return false;
	got = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	buffer[got] = '\0';
	return got && (!needle || strstr(buffer, needle));
}

static bool virtualized_or_containerized(void)
{
	const char *container = getenv("container");

	if ((container && *container) || access("/.dockerenv", F_OK) == 0 ||
	    text_file_contains("/run/systemd/container", NULL) ||
	    text_file_contains("/sys/hypervisor/type", NULL) ||
	    text_file_contains("/proc/1/cgroup", "docker") ||
	    text_file_contains("/proc/1/cgroup", "lxc") ||
	    text_file_contains("/proc/1/cgroup", "kubepods") ||
	    text_file_contains("/proc/1/cgroup", "containerd"))
		return true;
#if defined(__x86_64__) || defined(__i386__)
	{
		unsigned int eax, ebx, ecx, edx;

		if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1U << 31)))
			return true;
	}
#endif
	return false;
}

static bool database_intersects(const struct mw_bad_page_db *database,
				uint64_t start, uint64_t nr_pages)
{
	size_t low = 0, high = database->count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;

		if (database->pfns[middle] < start)
			low = middle + 1;
		else
			high = middle;
	}
	return low < database->count && database->pfns[low] - start < nr_pages;
}

static bool kernel_has_all_bad_pfns(int fd, const struct mw_info *info,
				    const struct mw_bad_page_db *database)
{
	uint64_t index, queryable, matched = 0;

	if (!database->count)
		return true;
	queryable = info->quarantined_pages - info->orphaned_quarantine_pages;
	for (index = 0; index < queryable; index++) {
		struct mw_quarantine_query query = { .index = index };

		if (ioctl(fd, MW_IOC_GET_QUARANTINE, &query))
			return false;
		if (mw_bad_pages_contains(database, query.pfn))
			matched++;
	}
	return matched == database->count;
}

static int persist_bad_result(const char *path, uint64_t start_pfn,
			      const struct mw_info *info,
			      const struct child_report *report,
			      struct mw_bad_page_db *database)
{
	uint64_t *pfns;
	size_t count, i;
	int ret;

	if (report->stage != CHILD_COMPLETED || report->test_status <= 0 ||
	    !report->result.bad_count)
		return 0;
	count = report->result.flags & MW_RESULT_F_QUARANTINE_ALL ?
		info->pageblock_pages : report->result.bad_count;
	pfns = calloc(count, sizeof(*pfns));
	if (!pfns)
		return -1;
	if (report->result.flags & MW_RESULT_F_QUARANTINE_ALL) {
		for (i = 0; i < count; i++)
			pfns[i] = start_pfn + i;
	} else {
		for (i = 0; i < count; i++)
			pfns[i] = start_pfn + report->result.bad_page_offsets[i];
	}
	ret = mw_bad_pages_append(path, pfns, count);
	free(pfns);
	if (!ret) {
		mw_bad_pages_free(database);
		ret = mw_bad_pages_load(path, database);
	}
	return ret;
}

static int command_preload(const char *device, const char *bad_pages,
			   bool acknowledged, bool allow_virtualized)
{
	struct mw_bad_page_db database;
	struct mw_info info;
	size_t index = 0;
	uint64_t retained = 0, failed = 0;
	int fd;

	if (!acknowledged) {
		fprintf(stderr, "refusing preload without --yes-i-understand\n");
		return 2;
	}
	if (!allow_virtualized && virtualized_or_containerized()) {
		fprintf(stderr, "refusing persistent PFN preload in a VM/container; "
			"guest PFNs are not stable hardware identities\n");
		return 2;
	}
	if (mw_bad_pages_load(bad_pages, &database)) {
		fprintf(stderr, "cannot load %s: %s\n", bad_pages, strerror(errno));
		return 1;
	}
	if (!database.count) {
		printf("no persisted bad PFNs to preload\n");
		mw_bad_pages_free(&database);
		return 0;
	}
	fd = open_device(device);
	if (fd < 0 || get_info_fd(fd, &info)) {
		if (fd >= 0) {
			perror("MW_IOC_GET_INFO");
			close(fd);
		}
		mw_bad_pages_free(&database);
		return 1;
	}
	close(fd);

	while (index < database.count) {
		struct mw_claim claim;
		struct mw_result result = { .flags = MW_RESULT_F_PRELOAD };
		uint64_t block = database.pfns[index] -
			database.pfns[index] % info.pageblock_pages;
		size_t end = index;

		while (end < database.count &&
		       database.pfns[end] - database.pfns[end] %
		       info.pageblock_pages == block)
			end++;
		claim = (struct mw_claim){
			.start_pfn = block,
			.nr_pages = info.pageblock_pages,
		};
		if (end - index > MW_MAX_BAD_PAGES) {
			result.flags |= MW_RESULT_F_QUARANTINE_ALL;
		} else {
			size_t cursor;

			result.bad_count = (uint32_t)(end - index);
			for (cursor = index; cursor < end; cursor++)
				result.bad_page_offsets[cursor - index] =
					(uint32_t)(database.pfns[cursor] - block);
		}

		fd = open_device(device);
		if (fd < 0 || ioctl(fd, MW_IOC_CLAIM, &claim) ||
		    ioctl(fd, MW_IOC_COMPLETE, &result)) {
			int saved_errno = errno;

			if (fd >= 0)
				close(fd); /* fail closed if the claim was acquired */
			fprintf(stderr, "cannot preload pageblock 0x%llx: %s\n",
				(unsigned long long)block, strerror(saved_errno));
			failed += end - index;
		} else {
			close(fd);
			retained += result.flags & MW_RESULT_F_QUARANTINE_ALL ?
				info.pageblock_pages : end - index;
		}
		index = end;
	}
	printf("preload: %llu pages retained, %llu known PFNs failed\n",
	       (unsigned long long)retained, (unsigned long long)failed);
	mw_bad_pages_free(&database);
	return failed ? 1 : 0;
}

static int command_scan(const char *device, const char *state, double interval,
			uint64_t passes, bool forever, bool quick,
			bool flush_cache, bool dry_run, bool acknowledged,
			bool restricted, uint64_t restrict_start, uint64_t restrict_end,
			const char *bad_pages)
{
	struct ranges ranges = { 0 };
	struct mw_bad_page_db known_bad = { 0 };
	struct mw_info info;
	FILE *ledger = NULL;
	uint64_t pass, candidates = 0, known_bad_blocks = 0;
	int fd = -1, rc = 1;
	bool persistence_failed = false;
	bool persistence_enabled = !virtualized_or_containerized();
	size_t i;

	if (!dry_run && !acknowledged) {
		fprintf(stderr, "refusing destructive scan without --yes-i-understand\n");
		return 2;
	}
	if (!dry_run) {
		fd = open_device(device);
		if (fd < 0 || get_info_fd(fd, &info)) {
			if (fd >= 0) perror("MW_IOC_GET_INFO");
			goto out;
		}
		if (persistence_enabled && mw_bad_pages_load(bad_pages, &known_bad)) {
			fprintf(stderr, "cannot validate %s: %s\n", bad_pages,
				strerror(errno));
			goto out;
		}
		if (persistence_enabled &&
		    !kernel_has_all_bad_pfns(fd, &info, &known_bad)) {
			fprintf(stderr, "persisted bad PFNs are not all quarantined; "
				"run preload before scanning\n");
			goto out;
		}
		if (!persistence_enabled)
			fprintf(stderr, "VM/container detected: persistent PFN recording is disabled\n");
		close(fd); fd = -1;
	} else {
		info.page_size = (uint32_t)sysconf(_SC_PAGESIZE);
		info.pageblock_pages = 512; /* typical x86-64; dry-run estimate only */
	}
	if (restricted) {
		if (restrict_start >= restrict_end ||
		    append_range(&ranges, restrict_start, restrict_end))
			goto out;
	} else if (discover_ranges(&ranges, info.page_size)) {
		goto out;
	}
	for (i = 0; i < ranges.count; i++) {
		uint64_t pfn;

		if (align_pfn_up(ranges.items[i].start_pfn, info.pageblock_pages,
				 &pfn))
			continue;
		if (ranges.items[i].end_pfn > pfn)
			candidates += (ranges.items[i].end_pfn - pfn) /
				      info.pageblock_pages;
		for (; persistence_enabled &&
		       pfn <= ranges.items[i].end_pfn &&
		       ranges.items[i].end_pfn - pfn >= info.pageblock_pages;
		     pfn += info.pageblock_pages)
			if (database_intersects(&known_bad, pfn,
						info.pageblock_pages))
				known_bad_blocks++;
	}
	printf("%llu candidate pageblocks, %u pages each; theoretical sweep %.2f days\n",
	       (unsigned long long)candidates, info.pageblock_pages,
	       candidates * interval / 86400.0);
	if (known_bad_blocks)
		printf("%llu pageblocks contain quarantined known-bad PFNs and will be skipped\n",
		       (unsigned long long)known_bad_blocks);
	if (dry_run) { rc = 0; goto out; }
	ledger = open_ledger(state);
	if (!ledger) { perror("open state ledger"); goto out; }
	if (fseek(ledger, 0, SEEK_END) == 0 && ftell(ledger) == 0)
		fprintf(ledger, "# unix_time\tstart_pfn\tpages\tstatus\tbad_pages\terrno\tchild_status\tseconds\n");

	signal(SIGINT, request_stop);
	signal(SIGTERM, request_stop);
	for (pass = 0; (forever || pass < passes) && !stop_requested; pass++) {
		printf("starting sweep %llu\n", (unsigned long long)(pass + 1));
		for (i = 0; i < ranges.count && !stop_requested; i++) {
			uint64_t pfn;

			if (align_pfn_up(ranges.items[i].start_pfn,
					 info.pageblock_pages, &pfn))
				continue;
			for (; pfn <= ranges.items[i].end_pfn &&
			       ranges.items[i].end_pfn - pfn >= info.pageblock_pages &&
			       !stop_requested; pfn += info.pageblock_pages) {
				struct child_report report;

				if (persistence_enabled &&
				    database_intersects(&known_bad, pfn,
							info.pageblock_pages))
					continue;

				int sig, child_rc = run_child(device, &info, pfn, quick,
							flush_cache, &report, &sig);
				if (persistence_enabled &&
				    persist_bad_result(bad_pages, pfn, &info, &report,
						       &known_bad)) {
					fprintf(stderr, "cannot persist bad PFNs to %s: %s; stopping scan\n",
						bad_pages, strerror(errno));
					stop_requested = 1;
					persistence_failed = true;
				}
				log_result(ledger, pfn, &info, &report, child_rc, sig);
				if (sig)
					fprintf(stderr, "pfn 0x%llx: tester died on signal %d; claim quarantined\n",
						(unsigned long long)pfn, sig);
				else if (report.error && report.error != EBUSY && report.error != EINVAL)
					fprintf(stderr, "pfn 0x%llx: %s\n",
						(unsigned long long)pfn, strerror(report.error));
				else if (report.test_status > 0)
					fprintf(stderr, "pfn 0x%llx: %u bad pages quarantined\n",
						(unsigned long long)pfn, report.result.bad_count);
				sleep_interval(interval);
			}
		}
	}
	rc = persistence_failed ? 1 : 0;
out:
	if (ledger) fclose(ledger);
	if (fd >= 0) close(fd);
	mw_bad_pages_free(&known_bad);
	free(ranges.items);
	return rc;
}

int main(int argc, char **argv)
{
	const char *command, *device = MW_DEVICE_PATH, *state = DEFAULT_STATE;
	const char *bad_pages = DEFAULT_BAD_PAGES;
	double interval = DEFAULT_INTERVAL;
	uint64_t passes = 1, start = 0, end = 0, mib = 16;
	bool forever = false, quick = false, flush_cache = true, dry_run = false;
	bool acknowledged = false, have_start = false, have_end = false;
	bool allow_virtualized_preload = false;
	int option;
	static const struct option options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "state", required_argument, NULL, 's' },
		{ "bad-pages", required_argument, NULL, 1003 },
		{ "interval", required_argument, NULL, 'i' },
		{ "passes", required_argument, NULL, 'p' },
		{ "forever", no_argument, NULL, 'f' },
		{ "start-pfn", required_argument, NULL, 1000 },
		{ "end-pfn", required_argument, NULL, 1001 },
		{ "quick", no_argument, NULL, 'q' },
		{ "no-cache-flush", no_argument, NULL, 1002 },
		{ "allow-virtualized-preload", no_argument, NULL, 1004 },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "yes-i-understand", no_argument, NULL, 'y' },
		{ "mib", required_argument, NULL, 'm' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};

	if (argc < 2) { usage(stderr); return 2; }
	if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
		puts(VERSION);
		return 0;
	}
	command = argv[1];
	argc--; argv++;
	while ((option = getopt_long(argc, argv, "d:s:i:p:fqnym:hV", options, NULL)) != -1) {
		switch (option) {
		case 'd': device = optarg; break;
		case 's': state = optarg; break;
		case 'i': if (parse_nonnegative_double(optarg, &interval)) return 2; break;
		case 'p': if (parse_u64(optarg, &passes) || !passes) return 2; break;
		case 'f': forever = true; break;
		case 'q': quick = true; break;
		case 'n': dry_run = true; break;
		case 'y': acknowledged = true; break;
		case 'm': if (parse_u64(optarg, &mib)) return 2; break;
		case 1000: if (parse_u64(optarg, &start)) return 2; have_start = true; break;
		case 1001: if (parse_u64(optarg, &end)) return 2; have_end = true; break;
		case 1002: flush_cache = false; break;
		case 1003: bad_pages = optarg; break;
		case 1004: allow_virtualized_preload = true; break;
		case 'V': puts(VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: return 2;
		}
	}
	if (!strcmp(command, "info"))
		return command_info(device);
	if (!strcmp(command, "selftest"))
		return command_selftest(quick, flush_cache, mib);
	if (!strcmp(command, "preload"))
		return command_preload(device, bad_pages, acknowledged,
				       allow_virtualized_preload);
	if (!strcmp(command, "scan")) {
		if (have_start != have_end) {
			fprintf(stderr, "--start-pfn and --end-pfn must be used together\n");
			return 2;
		}
		return command_scan(device, state, interval, passes, forever, quick,
				    flush_cache, dry_run, acknowledged, have_start,
				    start, end, bad_pages);
	}
	usage(stderr);
	return 2;
}
