CC := $(CROSS_COMPILE)$(CC)
CFLAGS += -I../../../../usr/include/

TEST_STR = ./kcmp_test || echo 'kcmp_test: [FAIL]'

all: kcmp_test

install:
ifdef INSTALL_KSFT_PATH
	install ./kcmp_test $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
	@echo rm -f kcmp-test-file >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(TEST_STR)

clean:
	$(RM) kcmp_test kcmp-test-file
