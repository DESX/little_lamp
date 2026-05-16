# Host-side unit tests. No ESP-IDF, no Zigbee stack — just gcc.
# Run with `make test` from the project root, or `make` directly here.

CC      := gcc
CFLAGS  := -std=gnu99 -Wall -Wextra -Wno-unused -Wno-unused-function -O0 -g -Istubs -I../../main -I..

TESTS   := test_schedule test_state

.PHONY: all clean run
all: $(TESTS)
	@for t in $(TESTS); do echo "── $$t ──"; ./$$t || exit 1; done
	@echo "all unit tests passed"

test_schedule: test_schedule.c ../../main/schedule.c
	$(CC) $(CFLAGS) -o $@ $^

test_state: test_state.c ../../main/state.c ../../main/schedule.c \
            stubs/bulb_stub.c stubs/rtc_stub.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TESTS)
