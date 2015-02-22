CC = $(CROSS_COMPILE)gcc

TEST_STR = ./get_size || echo 'get_size selftests: [FAIL]'

all: get_size

get_size: get_size.c
	$(CC) -static -ffreestanding -nostartfiles -s $< -o $@

install:
ifdef INSTALL_KSFT_PATH
	install ./get_size $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(TEST_STR)

clean:
	$(RM) get_size
