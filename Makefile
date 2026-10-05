# SPDX-License-Identifier: GPL-2.0-only
VERSION := 1.1.1
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude -Isrc
CPPFLAGS += -D_FORTIFY_SOURCE=2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -fPIE
CFLAGS += -ffile-prefix-map=$(CURDIR)=/usr/src/memwatcher
LDFLAGS += -Wl,-z,relro,-z,now -pie
KDIR ?= /lib/modules/$(shell uname -r)/build
DESTDIR ?=
PREFIX ?= /usr

.PHONY: all userspace module test clean install uninstall dist

all: userspace module

userspace: build/memwatcher

build/memwatcher: src/main.c src/memtest.c src/memtest.h src/badpages.c src/badpages.h include/memwatcher_uapi.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/memtest.c src/badpages.c

build/memtest_test: tests/memtest_test.c src/memtest.c src/memtest.h src/badpages.c src/badpages.h include/memwatcher_uapi.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/memtest_test.c src/memtest.c src/badpages.c

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel \
		KCFLAGS="$(KCFLAGS) -ffile-prefix-map=$(CURDIR)=/usr/src/memwatcher \
		-fdebug-prefix-map=$(CURDIR)=/usr/src/memwatcher \
		-fmacro-prefix-map=$(CURDIR)=/usr/src/memwatcher" modules

test: build/memtest_test build/memwatcher
	./build/memtest_test
	./build/memwatcher selftest --quick --mib 2
	./build/memwatcher scan --dry-run --interval 10
	@./build/memwatcher preload >/dev/null 2>&1; test $$? -eq 2

install: all
	install -D -m 0755 build/memwatcher $(DESTDIR)$(PREFIX)/sbin/memwatcher
	install -D -m 0644 kernel/memwatcher.ko $(DESTDIR)/lib/modules/$(shell uname -r)/extra/memwatcher.ko
	install -D -m 0644 packaging/systemd/memwatcher.service $(DESTDIR)/lib/systemd/system/memwatcher.service
	install -D -m 0644 packaging/systemd/memwatcher-preload.service $(DESTDIR)/lib/systemd/system/memwatcher-preload.service
	install -D -m 0644 packaging/sysusers.d/memwatcher.conf $(DESTDIR)/usr/lib/sysusers.d/memwatcher.conf
	install -D -m 0644 packaging/memwatcher.8 $(DESTDIR)$(PREFIX)/share/man/man8/memwatcher.8
	@if [ -z "$(DESTDIR)" ] && command -v systemd-sysusers >/dev/null 2>&1; then \
		systemd-sysusers /usr/lib/sysusers.d/memwatcher.conf; \
	fi
	@if [ -z "$(DESTDIR)" ]; then depmod -a; fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/memwatcher
	rm -f $(DESTDIR)/lib/modules/$(shell uname -r)/extra/memwatcher.ko
	rm -f $(DESTDIR)/lib/systemd/system/memwatcher.service
	rm -f $(DESTDIR)/lib/systemd/system/memwatcher-preload.service
	rm -f $(DESTDIR)/usr/lib/sysusers.d/memwatcher.conf
	rm -f $(DESTDIR)$(PREFIX)/share/man/man8/memwatcher.8
	@if [ -z "$(DESTDIR)" ]; then depmod -a; fi

dist: all
	@mkdir -p dist
	cp build/memwatcher dist/memwatcher-$(VERSION)-linux-x86_64
	cp kernel/memwatcher.ko dist/memwatcher-$(VERSION)-$(shell uname -r).ko
	tar --sort=name --owner=0 --group=0 --numeric-owner --mode='go-w' \
		--mtime='2026-10-05 00:00Z' --exclude='./.git' --exclude='./build' \
		--exclude='./dist' --exclude='*.cmd' --exclude='*.o' --exclude='*.ko' \
		--exclude='*.mod*' --exclude='Module.symvers' --exclude='modules.order' \
		-czf dist/memwatcher-$(VERSION)-source.tar.gz .
	cd dist && sha256sum \
		memwatcher-$(VERSION)-linux-x86_64 \
		memwatcher-$(VERSION)-$(shell uname -r).ko \
		memwatcher-$(VERSION)-source.tar.gz \
		> SHA256SUMS-$(VERSION)

clean:
	rm -rf build dist
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
