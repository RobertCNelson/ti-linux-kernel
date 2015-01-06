# Makefile for sysctl selftests.
# Expects kernel.sysctl_writes_strict=1.

# No binaries, but make sure arg-less "make" doesn't trigger "run_tests".
all:

INSTALL_PROGS = common_tests run_numerictests run_stringtests
NUMERIC_TEST_STR = /bin/sh ./run_numerictests
STRING_TEST_STR = /bin/sh ./run_stringtests

# Allow specific tests to be selected.
test_num:
	@$(NUMERIC_TEST_STR)

test_string:
	@$(STRING_TEST_STR)

install: all
ifdef INSTALL_KSFT_PATH
	install $(INSTALL_PROGS) $(INSTALL_KSFT_PATH)
	@echo "$(NUMERIC_TEST_STR)" >> $(KSELFTEST)
	@echo "$(STRING_TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all test_num test_string

# Nothing to clean up.
clean:

.PHONY: all run_tests clean test_num test_string
