/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MEMWATCHER_UAPI_H
#define MEMWATCHER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MW_ABI_VERSION 2U
#define MW_MAX_BAD_PAGES 64U
#define MW_DEVICE_PATH "/dev/memwatcher"

struct mw_info {
	__u32 abi_version;
	__u32 page_size;
	__u32 pageblock_pages;
	__u32 max_bad_pages;
	__u64 successful_claims;
	__u64 failed_claims;
	__u64 tested_pages;
	__u64 quarantined_pages;
	__u64 orphaned_quarantine_pages;
	__u64 preloaded_pages;
};

struct mw_claim {
	__u64 start_pfn;
	__u32 nr_pages;
	__u32 flags;
};

#define MW_RESULT_F_QUARANTINE_ALL (1U << 0)
#define MW_RESULT_F_PRELOAD        (1U << 1)
#define MW_RESULT_F_INCOMPLETE     (1U << 2)

struct mw_result {
	__u32 bad_count;
	__u32 flags;
	__u32 bad_page_offsets[MW_MAX_BAD_PAGES];
	/* Explicit padding keeps this ioctl layout identical on 32/64-bit ABIs. */
	__u32 reserved[2];
	__u64 first_bad_byte;
	__u64 expected;
	__u64 observed;
};

#define MW_QUARANTINE_F_WHOLE_CLAIM (1U << 0)
#define MW_QUARANTINE_F_ABANDONED   (1U << 1)
#define MW_QUARANTINE_F_PRELOADED   (1U << 2)
#define MW_QUARANTINE_F_INCOMPLETE  (1U << 3)

struct mw_quarantine_query {
	__u64 index;
	__u64 pfn;
	__u32 flags;
	__u32 reserved;
};

#define MW_IOC_MAGIC 0xB7
#define MW_IOC_GET_INFO       _IOR(MW_IOC_MAGIC, 0x00, struct mw_info)
#define MW_IOC_CLAIM          _IOW(MW_IOC_MAGIC, 0x01, struct mw_claim)
#define MW_IOC_COMPLETE       _IOW(MW_IOC_MAGIC, 0x02, struct mw_result)
#define MW_IOC_QUARANTINE     _IO(MW_IOC_MAGIC, 0x03)
#define MW_IOC_GET_QUARANTINE _IOWR(MW_IOC_MAGIC, 0x04, struct mw_quarantine_query)

#endif
