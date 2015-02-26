MQ_OPEN_TEST_STR = ./mq_open_tests /test1 || echo 'mq_open_tests: [FAIL]'
MQ_PERF_TEST_STR = ./mq_perf_tests || echo 'mq_perf_tests: [FAIL]'

all:
	gcc -O2 mq_open_tests.c -o mq_open_tests -lrt
	gcc -O2 -o mq_perf_tests mq_perf_tests.c -lrt -lpthread -lpopt

install:
ifdef INSTALL_KSFT_PATH
	install ./mq_open_tests ./mq_perf_tests $(INSTALL_KSFT_PATH)
	@echo "$(MQ_OPEN_TEST_STR)" >> $(KSELFTEST)
	@echo "$(MQ_PERF_TEST_STR)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests: all
	@$(MQ_OPEN_TEST_STR)
	@$(MQ_PERF_TEST_STR)

clean:
	rm -f mq_open_tests mq_perf_tests
