# Makefile for user memory selftests

TEST_STR = ./test_user_copy.sh

# No binaries, but make sure arg-less "make" doesn't trigger "run_tests"
all:

install:
ifdef INSTALL_KSFT_PATH
	install ./test_user_copy.sh $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(TEST_STR)
