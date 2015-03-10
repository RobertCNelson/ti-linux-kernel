# Taken from perf makefile
uname_M := $(shell uname -m 2>/dev/null || echo not)
ARCH ?= $(shell echo $(uname_M) | sed -e s/i.86/i386/)
ifeq ($(ARCH),i386)
        ARCH := x86
endif
ifeq ($(ARCH),x86_64)
	ARCH := x86
endif

TEST_STR = ./breakpoint_test || echo 'breakpoints selftests: [FAIL]'

all:
ifeq ($(ARCH),x86)
	gcc breakpoint_test.c -o breakpoint_test
else
	echo "Not an x86 target, can't build breakpoints selftests"
endif

install:
ifdef INSTALL_KSFT_PATH
ifeq ($(ARCH),x86)
	install ./breakpoint_test $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Not an x86 target, unable to install breakpoints selftests"
endif
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
ifeq ($(ARCH),x86)
	@$(TEST_STR)
endif

clean:
	rm -fr breakpoint_test
