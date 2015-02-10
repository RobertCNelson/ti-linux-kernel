CFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -I../../../../include/uapi/
CFLAGS += -I../../../../include/

INSTALL_PROGS = memfd_test fuse_test run_fuse_test.sh
MEMFD_TEST_STR = ./memfd_test || echo 'memfd_test: [FAIL]'
FUSE_TEST_STR = ./run_fuse_test.sh || echo 'fuse_test: [FAIL]'

all:
	gcc $(CFLAGS) memfd_test.c -o memfd_test

install:
ifdef INSTALL_KSFT_PATH
	install $(INSTALL_PROGS) $(INSTALL_KSFT_PATH)
	@echo "$(MEMFD_TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(MEMFD_TEST_STR)

build_fuse:
	gcc $(CFLAGS) fuse_mnt.c `pkg-config fuse --cflags --libs` -o fuse_mnt
	gcc $(CFLAGS) fuse_test.c -o fuse_test

run_fuse: build_fuse
	@$(FUSE_TEST_STR)

clean:
	$(RM) memfd_test fuse_test
