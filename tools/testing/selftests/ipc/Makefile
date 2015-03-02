uname_M := $(shell uname -m 2>/dev/null || echo not)
ARCH ?= $(shell echo $(uname_M) | sed -e s/i.86/i386/)
ifeq ($(ARCH),i386)
        ARCH := x86
	CFLAGS := -DCONFIG_X86_32 -D__i386__
endif
ifeq ($(ARCH),x86_64)
	ARCH := x86
	CFLAGS := -DCONFIG_X86_64 -D__x86_64__
endif

CFLAGS += -I../../../../usr/include/

TEST_STR = ./msgque_test || echo 'ipc msgque test: [FAIL]'

all:
ifeq ($(ARCH),x86)
	gcc $(CFLAGS) msgque.c -o msgque_test
else
	echo "Not an x86 target, can't build msgque selftest"
endif

install:
ifdef INSTALL_KSFT_PATH
ifeq ($(ARCH),x86)
	make all
	install ./msgque_test $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Not an x86 target, unable to install ipc msgque selftests"
endif
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
ifeq ($(ARCH),x86)
	@$(TEST_STR)
endif

clean:
	rm -fr ./msgque_test
