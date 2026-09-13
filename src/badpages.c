// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "badpages.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MW_BAD_PAGE_FILE_MAX_BYTES (8U * 1024U * 1024U)
#define MW_MACHINE_ID_PATH "/etc/machine-id"

static int compare_pfns(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return a < b ? -1 : a > b;
}

static int read_machine_id(char id[33])
{
	FILE *file;
	size_t i;

	file = fopen(MW_MACHINE_ID_PATH, "re");
	if (!file)
		return -1;
	if (!fgets(id, 33, file)) {
		fclose(file);
		errno = EINVAL;
		return -1;
	}
	fclose(file);
	if (strlen(id) != 32) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < 32; i++) {
		if (!((id[i] >= '0' && id[i] <= '9') ||
		      (id[i] >= 'a' && id[i] <= 'f') ||
		      (id[i] >= 'A' && id[i] <= 'F'))) {
			errno = EINVAL;
			return -1;
		}
	}
	id[32] = '\0';
	return 0;
}

static int validate_file(const struct stat *status)
{
	if (!S_ISREG(status->st_mode) || status->st_uid != geteuid() ||
	    (status->st_mode & 0022) || status->st_size < 0 ||
	    (uint64_t)status->st_size > MW_BAD_PAGE_FILE_MAX_BYTES) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static int fsync_parent_directory(const char *path)
{
	char directory[PATH_MAX];
	char *slash;
	int fd, ret, saved_errno;

	if (strlen(path) >= sizeof(directory)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(directory, path);
	slash = strrchr(directory, '/');
	if (!slash) {
		strcpy(directory, ".");
	} else if (slash == directory) {
		directory[1] = '\0';
	} else {
		*slash = '\0';
	}
	fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
	if (fd < 0)
		return -1;
	ret = fsync(fd);
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return ret;
}

bool mw_bad_pages_contains(const struct mw_bad_page_db *database, uint64_t pfn)
{
	return database && database->count &&
	       bsearch(&pfn, database->pfns, database->count,
		       sizeof(*database->pfns), compare_pfns) != NULL;
}

int mw_bad_pages_add(struct mw_bad_page_db *database, uint64_t pfn)
{
	uint64_t *resized;
	size_t capacity;

	if (!database) {
		errno = EINVAL;
		return -1;
	}
	if (database->count >= MW_MAX_PERSISTED_BAD_PAGES) {
		errno = EFBIG;
		return -1;
	}
	if (database->count == database->capacity) {
		capacity = database->capacity ? database->capacity * 2 : 64;
		if (capacity > MW_MAX_PERSISTED_BAD_PAGES)
			capacity = MW_MAX_PERSISTED_BAD_PAGES;
		resized = realloc(database->pfns, capacity * sizeof(*resized));
		if (!resized)
			return -1;
		database->pfns = resized;
		database->capacity = capacity;
	}
	database->pfns[database->count++] = pfn;
	return 0;
}

void mw_bad_pages_free(struct mw_bad_page_db *database)
{
	if (!database)
		return;
	free(database->pfns);
	memset(database, 0, sizeof(*database));
}

static int parse_database(FILE *file, struct mw_bad_page_db *database)
{
	char current_id[33], file_id[33];
	char *line = NULL;
	size_t line_capacity = 0;
	ssize_t length;
	bool have_header = false, have_machine = false;
	int ret = -1;

	if (read_machine_id(current_id))
		return -1;
	while ((length = getline(&line, &line_capacity, file)) >= 0) {
		char *end, *tail;
		unsigned long long parsed;

		while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
			line[--length] = '\0';
		if (!length)
			continue;
		if (!have_header) {
			if (strcmp(line, MW_BAD_PAGE_FILE_HEADER)) {
				errno = EPROTO;
				goto out;
			}
			have_header = true;
			continue;
		}
		if (!have_machine) {
			static const char prefix[] = "# machine-id ";

			if (strlen(line) != sizeof(prefix) - 1 + 32 ||
			    strncmp(line, prefix, sizeof(prefix) - 1)) {
				errno = EPROTO;
				goto out;
			}
			memcpy(file_id, line + sizeof(prefix) - 1, 32);
			file_id[32] = '\0';
			if (strcmp(file_id, current_id)) {
				errno = EXDEV;
				goto out;
			}
			have_machine = true;
			continue;
		}
		if (line[0] == '#')
			continue;
		errno = 0;
		parsed = strtoull(line, &end, 0);
		if (errno || end == line || line[0] == '-') {
			errno = EPROTO;
			goto out;
		}
		tail = end;
		while (*tail == ' ' || *tail == '\t')
			tail++;
		if (*tail) {
			char *timestamp_start = tail;

			errno = 0;
			(void)strtoull(timestamp_start, &end, 10);
			while (*end == ' ' || *end == '\t')
				end++;
			if (errno || end == timestamp_start || timestamp_start[0] == '-' ||
			    *end) {
				errno = EPROTO;
				goto out;
			}
		}
		if (mw_bad_pages_add(database, (uint64_t)parsed))
			goto out;
	}
	if (ferror(file) || !have_header || !have_machine) {
		errno = EPROTO;
		goto out;
	}
	if (database->count) {
		size_t source, destination = 1;

		qsort(database->pfns, database->count, sizeof(*database->pfns),
		      compare_pfns);
		for (source = 1; source < database->count; source++)
			if (database->pfns[source] != database->pfns[destination - 1])
				database->pfns[destination++] = database->pfns[source];
		database->count = destination;
	}
	ret = 0;
out:
	free(line);
	return ret;
}

int mw_bad_pages_load(const char *path, struct mw_bad_page_db *database)
{
	struct stat status;
	FILE *file;
	int fd, saved_errno, ret;

	if (!path || !database) {
		errno = EINVAL;
		return -1;
	}
	memset(database, 0, sizeof(*database));
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	if (fstat(fd, &status) || validate_file(&status)) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (!status.st_size) {
		close(fd);
		return 0;
	}
	file = fdopen(fd, "r");
	if (!file) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	ret = parse_database(file, database);
	saved_errno = errno;
	if (fclose(file) && !ret) {
		mw_bad_pages_free(database);
		return -1;
	}
	if (ret)
		mw_bad_pages_free(database);
	errno = saved_errno;
	return ret;
}

int mw_bad_pages_append(const char *path, const uint64_t *pfns, size_t count)
{
	struct mw_bad_page_db existing;
	struct stat status;
	char machine_id[33];
	FILE *file = NULL;
	int fd = -1, saved_errno, ret = -1;
	size_t i;
	time_t now;

	if (!path || (!pfns && count) || count > MW_MAX_PERSISTED_BAD_PAGES) {
		errno = EINVAL;
		return -1;
	}
	if (!count)
		return 0;
	if (mw_bad_pages_load(path, &existing))
		return -1;
	if (read_machine_id(machine_id))
		goto out_existing;
	fd = open(path, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0 || flock(fd, LOCK_EX) || fstat(fd, &status) ||
	    validate_file(&status))
		goto out;
	file = fdopen(fd, "a");
	if (!file)
		goto out;
	fd = -1;
	if (!status.st_size &&
	    fprintf(file, "%s\n# machine-id %s\n", MW_BAD_PAGE_FILE_HEADER,
		    machine_id) < 0)
		goto out;
	now = time(NULL);
	for (i = 0; i < count; i++) {
		if (mw_bad_pages_contains(&existing, pfns[i]))
			continue;
		if (mw_bad_pages_add(&existing, pfns[i]))
			goto out;
		qsort(existing.pfns, existing.count, sizeof(*existing.pfns),
		      compare_pfns);
		if (fprintf(file, "0x%" PRIx64 "\t%lld\n", pfns[i],
			    (long long)now) < 0)
			goto out;
	}
	if (fflush(file) || fsync(fileno(file)) || fsync_parent_directory(path))
		goto out;
	ret = 0;
out:
	saved_errno = errno;
	if (file) {
		if (fclose(file) && !ret) {
			ret = -1;
			saved_errno = errno;
		}
	} else if (fd >= 0) {
		close(fd);
	}
	errno = saved_errno;
out_existing:
	mw_bad_pages_free(&existing);
	return ret;
}
