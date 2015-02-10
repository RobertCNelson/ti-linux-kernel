TEST_STR = /bin/sh ./ftracetest || echo 'ftrace selftests: [FAIL]'

all:

install:
ifdef INSTALL_KSFT_PATH
	install ./ftracetest $(INSTALL_KSFT_PATH)
	@cp -r test.d $(INSTALL_KSFT_PATH)
	echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests:
	@$(TEST_STR)

clean:
	rm -rf logs/*
