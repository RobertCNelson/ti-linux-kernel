# Makefile for net selftests

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -O2 -g

CFLAGS += -I../../../../usr/include/

NET_PROGS = socket psock_fanout psock_tpacket
INSTALL_PROGS = run_netsocktests run_afpackettests test_bpf.sh $(NET_PROGS)
NETSOCK_TEST_STR = /bin/sh ./run_netsocktests || echo 'sockettests: [FAIL]'
AFPKT_TEST_STR = /bin/sh ./run_afpackettests || echo 'afpackettests: [FAIL]'
BFP_TEST_STR = ./test_bpf.sh

all: $(NET_PROGS)
%: %.c
	$(CC) $(CFLAGS) -o $@ $^

install:
ifdef INSTALL_KSFT_PATH
	install $(INSTALL_PROGS) $(INSTALL_KSFT_PATH)
	@echo "$(NETSOCK_TEST_STR)" >> $(KSELFTEST)
	@echo "$(AFPKT_TEST_STR)" >> $(KSELFTEST)
	@echo "$(BFP_TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(NETSOCK_TEST_STR)
	@$(AFPKT_TEST_STR)
	@$(BFP_TEST_STR)
clean:
	$(RM) $(NET_PROGS)
