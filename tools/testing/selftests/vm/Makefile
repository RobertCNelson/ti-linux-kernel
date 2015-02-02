# Makefile for vm selftests

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall
BINARIES = hugepage-mmap hugepage-shm map_hugetlb thuge-gen hugetlbfstest
BINARIES += transhuge-stress
TEST_STR = /bin/sh ./run_vmtests || echo 'vmtests: [FAIL]'

all: $(BINARIES)
%: %.c
	$(CC) $(CFLAGS) -o $@ $^ -lrt

install:
ifdef INSTALL_KSFT_PATH
	install run_vmtests $(BINARIES) $(INSTALL_KSFT_PATH)
	@echo "$(TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(TEST_STR)

clean:
	$(RM) $(BINARIES)
