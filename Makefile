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

# A separately qualified baseline AMD64 image is embedded in the single
# installed hosted carrier. Native optimization remains a hosted concern.
OBJCOPY ?= objcopy
FIRMWARE_FLAGS = -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -mno-red-zone -mgeneral-regs-only -fcf-protection=none -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fPIC -fvisibility=hidden
FIRMWARE_LINK = -shared -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now,-z,nopack-relative-relocs,-z,max-page-size=4096
.PHONY: firmware carrier
firmware: build/firmware/EFS.so
build/firmware/EFS.so: native/filesystems/efs/efs.c native/filesystems/efs/runtime.c $(wildcard native/filesystems/efs/*.h) module/firmware/CMakeLists.txt Makefile
	mkdir -p build/firmware
	$(CC) $(FIRMWARE_FLAGS) -DIFS_BOOT_FIRMWARE=1 -I$(COMMON)/include $(FIRMWARE_LINK) -o $@ native/filesystems/efs/efs.c native/filesystems/efs/runtime.c
	test "$$(nm -D --defined-only $@ | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test -z "$$(nm -D --undefined-only $@)"
	! readelf -dW $@ | grep -E 'NEEDED|SONAME|INIT_ARRAY|FINI_ARRAY|RELR'
carrier: build/EFS.so firmware
	mkdir -p build/carrier
	$(OBJCOPY) --add-section .infiltrator_boot=build/firmware/EFS.so --set-section-flags .infiltrator_boot=readonly build/EFS.so build/carrier/EFS.so
	$(OBJCOPY) --dump-section .infiltrator_boot=build/carrier/verify.elf build/carrier/EFS.so build/carrier/verify-carrier.elf
	cmp build/firmware/EFS.so build/carrier/verify.elf
	rm -f build/carrier/verify.elf build/carrier/verify-carrier.elf
