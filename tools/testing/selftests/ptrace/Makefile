CFLAGS += -iquote../../../../include/uapi -Wall

TEST_STR = ./peeksiginfo || echo 'peeksiginfo selftests: [FAIL]'

all:
	gcc peeksiginfo.c -o peeksiginfo

install:
ifdef INSTALL_KSFT_PATH
	install ./peeksiginfo $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

clean:
	rm -f peeksiginfo

run_tests: all
	@$(TEST_STR)
