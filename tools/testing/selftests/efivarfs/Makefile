CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall

test_objs = open-unlink create-read

TEST_STR = /bin/bash ./efivarfs.sh || echo 'efivarfs selftests: [FAIL]'

all:
	gcc open-unlink.c -o open-unlink
	gcc create-read.c -o create-read

install:
ifdef INSTALL_KSFT_PATH
	install ./efivarfs.sh $(test_objs) $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(TEST_STR)

clean:
	rm -f $(test_objs)
