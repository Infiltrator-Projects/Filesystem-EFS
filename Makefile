# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
COMMON ?= Libraries
CFLAGS ?= -O2
BASE = -std=c11 -Wall -Wextra -Werror -I$(COMMON)/include
.PHONY: all check clean
all: build/EFS.so
build/EFS.so: native/filesystems/efs/efs.c native/filesystems/efs/journal.h native/filesystems/efs/mutation.h native/filesystems/efs/defrag.h
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-z,defs -o $@ $<
build/test_efs: tests/test_efs.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_format: tests/test_format.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_journal: tests/test_journal.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_namespace: tests/test_namespace.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_namespace_recovery: tests/test_namespace_recovery.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_defrag: tests/test_defrag.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_defrag_recovery: tests/test_defrag_recovery.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
check: all build/test_efs build/test_format build/test_journal build/test_namespace build/test_namespace_recovery build/test_defrag build/test_defrag_recovery
	test "$$(nm -D --defined-only build/EFS.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	build/test_efs ./build/EFS.so
	build/test_format ./build/EFS.so
	build/test_journal ./build/EFS.so
	build/test_namespace ./build/EFS.so
	build/test_namespace_recovery ./build/EFS.so
	build/test_defrag ./build/EFS.so
	build/test_defrag_recovery ./build/EFS.so
clean:
	rm -rf build
