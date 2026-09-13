# SPDX-License-Identifier: GPL-2.0-only
obj-m += memwatcher.o
ccflags-y += -I$(src)/../include
# Avoid embedding the out-of-tree build path in UBSAN location records.
UBSAN_SANITIZE_memwatcher.o := n
