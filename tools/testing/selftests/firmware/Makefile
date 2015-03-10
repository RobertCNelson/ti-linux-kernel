# Makefile for firmware loading selftests

# No binaries, but make sure arg-less "make" doesn't trigger "run_tests"

__fw_filesystem:
fw_filesystem  = if /bin/sh ./fw_filesystem.sh ; then
fw_filesystem += echo 'fw_filesystem: ok';
fw_filesystem += else echo 'fw_filesystem: [FAIL]';
fw_filesystem += fi

__fw_userhelper:
fw_userhelper  = if /bin/sh ./fw_userhelper.sh ; then
fw_userhelper += echo 'fw_userhelper: ok';
fw_userhelper += else
fw_userhelper += echo 'fw_userhelper: [FAIL]';
fw_userhelper += fi

all:

install:
ifdef INSTALL_KSFT_PATH
	install ./fw_filesystem.sh ./fw_userhelper.sh $(INSTALL_KSFT_PATH)
	@echo "$(fw_filesystem)" >> $(KSELFTEST)
	@echo "$(fw_userhelper)" >> $(KSELFTEST)
else
	@echo "Run make kselftest_install in top level source directory"
endif

run_tests:
	@$(fw_filesystem)
	@$(fw_userhelper)

# Nothing to clean up.
clean:

.PHONY: all clean run_tests fw_filesystem fw_userhelper
