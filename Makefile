# Userspace programs build with plain make; the module uses kbuild (kernel/Makefile).
CC      ?= gcc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes -Werror
UCFLAGS := -std=gnu11 $(WARN) -Iinclude -Ilogger -pthread $(CFLAGS)
B       := build

LOGGER_SRC := logger/vslogger.c logger/bqueue.c logger/hist.c

all: module user

module:
	$(MAKE) -C kernel

user: $(B)/vslogger $(B)/vsctl $(B)/vsensor_test $(B)/unit_test

$(B):
	mkdir -p $(B)

$(B)/vslogger: $(LOGGER_SRC) logger/*.h include/uapi/vsensor.h include/vsensor_model.h | $(B)
	$(CC) $(UCFLAGS) -o $@ $(LOGGER_SRC) -lm

$(B)/vsctl: tools/vsctl.c include/uapi/vsensor.h include/vsensor_model.h | $(B)
	$(CC) $(UCFLAGS) -o $@ tools/vsctl.c

$(B)/vsensor_test: tests/vsensor_test.c include/uapi/vsensor.h include/vsensor_model.h | $(B)
	$(CC) $(UCFLAGS) -o $@ tests/vsensor_test.c

# Hardware-independent unit tests for the logger's queue and histogram.
$(B)/unit_test: tests/unit_test.c logger/bqueue.c logger/hist.c logger/*.h | $(B)
	$(CC) $(UCFLAGS) -o $@ tests/unit_test.c logger/bqueue.c logger/hist.c -lm

# ThreadSanitizer / AddressSanitizer builds of the logger and unit tests.
$(B)/vslogger-tsan: $(LOGGER_SRC) logger/*.h | $(B)
	$(CC) $(UCFLAGS) -O1 -fsanitize=thread -o $@ $(LOGGER_SRC) -lm
$(B)/vslogger-asan: $(LOGGER_SRC) logger/*.h | $(B)
	$(CC) $(UCFLAGS) -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -o $@ $(LOGGER_SRC) -lm
$(B)/unit_test-tsan: tests/unit_test.c logger/bqueue.c logger/hist.c | $(B)
	$(CC) $(UCFLAGS) -O1 -fsanitize=thread -o $@ $^ -lm
$(B)/unit_test-asan: tests/unit_test.c logger/bqueue.c logger/hist.c | $(B)
	$(CC) $(UCFLAGS) -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -o $@ $^ -lm

sanitizers: $(B)/vslogger-tsan $(B)/vslogger-asan $(B)/unit_test-tsan $(B)/unit_test-asan

unit: $(B)/unit_test $(B)/unit_test-tsan $(B)/unit_test-asan
	$(B)/unit_test && $(B)/unit_test-tsan && $(B)/unit_test-asan

sparse:
	$(MAKE) -C kernel sparse

checkpatch:
	/lib/modules/$$(uname -r)/build/scripts/checkpatch.pl --no-tree --strict \
		--ignore SPDX_LICENSE_TAG,LINUX_VERSION_CODE,CONSTANT_COMPARISON -f kernel/vsensor.c

# Full kernel-facing suite (inside the Linux VM). Builds as the user, runs as root.
SUDO := $(shell [ "$$(id -u)" -eq 0 ] || echo sudo)
check: all sanitizers
	$(SUDO) tests/run_all.sh

clean:
	$(MAKE) -C kernel clean
	rm -rf $(B)

.PHONY: all module user sanitizers unit sparse checkpatch check clean
