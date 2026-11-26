# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
COMMON ?= Libraries
PROFILE ?= portable
ifeq ($(PROFILE),aggressive)
CFLAGS ?= -O3 -march=native -flto
else
CFLAGS ?= -O2 -march=x86-64 -mtune=generic
endif
NATIVE = -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns
LINK = -shared -nostdlib -Wl,-z,defs,-Bsymbolic
# Sanitizer builds deliberately link the test runtime; release builds never do.
ifneq ($(findstring -fsanitize=,$(CFLAGS)),)
LINK = -shared -Wl,-z,defs,-Bsymbolic
endif
BASE = -std=c11 -Wall -Wextra -Werror -I$(COMMON)/include
.PHONY: all check clean
all: build/EFS.so
build/runtime.o: native/filesystems/efs/runtime.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) $(NATIVE) -fno-lto -fPIC -fvisibility=hidden -c -o $@ $<
build/EFS.so: native/filesystems/efs/efs.c native/filesystems/efs/journal.h native/filesystems/efs/mutation.h native/filesystems/efs/defrag.h native/filesystems/efs/runtime.h build/runtime.o
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) $(NATIVE) -fPIC -fvisibility=hidden $(LINK) -o $@ $< build/runtime.o
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
build/test_oracle: tests/test_oracle.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
check: all build/test_efs build/test_format build/test_journal build/test_namespace build/test_namespace_recovery build/test_defrag build/test_defrag_recovery
	test "$$(nm -D --defined-only build/EFS.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	@if ! echo '$(CFLAGS)' | grep -q -- '-fsanitize='; then test -z "$(nm -D --undefined-only build/EFS.so)" && ! readelf -d build/EFS.so | grep -q NEEDED; fi
	build/test_efs ./build/EFS.so
	build/test_format ./build/EFS.so
	build/test_journal ./build/EFS.so
	build/test_namespace ./build/EFS.so
	build/test_namespace_recovery ./build/EFS.so
	build/test_defrag ./build/EFS.so
	build/test_defrag_recovery ./build/EFS.so
clean:
	rm -rf build
