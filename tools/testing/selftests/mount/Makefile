# Makefile for mount selftests.

TEST_STR = if [ -f /proc/self/uid_map ] ; then ./unprivileged-remount-test ; fi

all: unprivileged-remount-test

unprivileged-remount-test: unprivileged-remount-test.c
	gcc -Wall -O2 unprivileged-remount-test.c -o unprivileged-remount-test

# Allow specific tests to be selected.
test_unprivileged_remount: unprivileged-remount-test
	@$(TEST_STR)

install:
ifdef INSTALL_KSFT_PATH
	install ./unprivileged-remount-test $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all test_unprivileged_remount

clean:
	rm -f unprivileged-remount-test

.PHONY: all test_unprivileged_remount
