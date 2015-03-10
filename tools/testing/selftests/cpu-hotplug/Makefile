TEST_STR=/bin/bash ./cpu-on-off-test.sh || echo 'cpu-hotplug selftests: [FAIL]'

all:

install:
ifdef INSTALL_KSFT_PATH
	install ./cpu-on-off-test.sh $(INSTALL_KSFT_PATH)/cpu-on-off-test.sh
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests:
	@$(TEST_STR)

run_full_test:
	@/bin/bash ./cpu-on-off-test.sh -a || echo "cpu-hotplug selftests: [FAIL]"

clean:
