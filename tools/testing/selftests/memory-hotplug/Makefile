TEST_STR=/bin/bash ./mem-on-off-test.sh -r 2 || echo 'memory-hotplug selftests: [FAIL]'

all:

install:
ifdef INSTALL_KSFT_PATH
	install ./mem-on-off-test.sh $(INSTALL_KSFT_PATH)/mem-on-off-test.sh
	@echo "$(TEST_STR)" >> $(KSELFTEST) >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests:
	@$(TEST_STR)

run_full_test:
	@/bin/bash ./mem-on-off-test.sh || echo "memory-hotplug selftests: [FAIL]"

clean:
