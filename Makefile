# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
COMMON ?= Libraries
CFLAGS ?= -O2
BASE = -std=c11 -Wall -Wextra -Werror -I$(COMMON)/include
.PHONY: all check clean
all: build/EFS.so
build/EFS.so: native/filesystems/efs/efs.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-z,defs -o $@ $<
build/test_efs: tests/test_efs.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
check: all build/test_efs
	test "$$(nm -D --defined-only build/EFS.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	build/test_efs ./build/EFS.so
clean:
	rm -rf build
