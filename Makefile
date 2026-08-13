# thermiq-bridge
#
# Generated sources are checked in, so a plain `make` needs only a C compiler.
# Run `make generate` after changing the register table, the widget template
# or anything under web/ - CI fails if the checked-in output is stale.

CC ?= cc
PYTHON ?= python3
TARGET ?=

# The warning set is not optional and not a preference: it is the only review
# this program gets that does not involve a person. It therefore lives in its
# own variable rather than in CFLAGS - CFLAGS is the caller's to set (CI and
# the cross builds both do), and a plain `CFLAGS=-O2 make` must not silently
# take the warnings and the hardening away with it.
WARNINGS := -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion \
            -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wpointer-arith
HARDENING := -fstack-protector-strong
# The sanitizers predefine _FORTIFY_SOURCE to 0 and check the same calls more
# thoroughly, so setting it under SAN=1 only warns about the redefinition.
ifneq ($(SAN),1)
HARDENING += -D_FORTIFY_SOURCE=2
endif

# CFLAGS and LDFLAGS belong to whoever runs make. Everything this program
# requires goes in ALL_CFLAGS/ALL_LDFLAGS, which the recipes use - a variable
# set on the command line overrides a makefile's `+=` entirely, so appending to
# CFLAGS would mean `make CFLAGS=-O2` quietly dropped the warnings, the
# hardening, -std=c11 and the sanitizers along with them.
CFLAGS ?= -Os
LDLIBS ?= -lm

ALL_CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -fno-common \
             -ffunction-sections -fdata-sections \
             $(WARNINGS) $(HARDENING) $(CFLAGS) $(EXTRA_CFLAGS)
ALL_LDFLAGS = $(LDFLAGS) $(EXTRA_LDFLAGS)

# _POSIX_C_SOURCE above asks for strict POSIX, and macOS honours that by
# hiding anything BSD-flavoured - INADDR_LOOPBACK among it. The target is
# Linux either way, but a program the maintainer cannot build on the laptop in
# front of them is a program that only CI can tell you about.
ifeq ($(shell uname -s),Darwin)
ALL_CFLAGS += -D_DARWIN_C_SOURCE
# Apple's linker spells --gc-sections -dead_strip.
LDFLAGS ?= -Wl,-dead_strip
else
LDFLAGS ?= -Wl,--gc-sections
endif

ifneq ($(TARGET),)
ALL_CFLAGS += -target $(TARGET)
ALL_LDFLAGS += -target $(TARGET)
endif

# `make test SAN=1` runs the same tests under AddressSanitizer and
# UndefinedBehaviorSanitizer. The warning set above is a review of the source;
# this is a review of what it does when it runs. Every buffer in this program
# is fixed and bounded, which is a claim until something checks the bounds
# against real input - and the input here is a JSON payload off the network.
#
# -fno-sanitize-recover matters: UBSan's default is to print and carry on, so
# without it a signed overflow leaves a line in the log and a green build.
ifeq ($(SAN),1)
ALL_CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
              -fno-sanitize-recover=all
ALL_LDFLAGS += -fsanitize=address,undefined
endif

SRC := src/main.c src/config.c src/discover.c src/http.c src/json.c src/mqtt.c \
       src/registers.c \
       src/registers_gen.c src/state.c src/util.c src/widget.c src/widget_gen.c \
       src/assets_gen.c
LIB := $(filter-out src/main.c,$(SRC))

BUILD := build
BIN := $(BUILD)/thermiq-bridge

.PHONY: all generate check-generated test clean run-demo

all: $(BIN)

$(BIN): $(SRC) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ $(SRC) $(ALL_LDFLAGS) $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

generate:
	$(PYTHON) codegen/gen_registers.py
	$(PYTHON) codegen/gen_widget.py
	$(PYTHON) codegen/gen_assets.py
	$(PYTHON) codegen/gen_widget_cases.py

check-generated:
	$(PYTHON) codegen/gen_registers.py --check
	$(PYTHON) codegen/gen_widget.py --check
	$(PYTHON) codegen/gen_assets.py --check
	# The runbook in AI_INSTALL.md is followed literally by an agent, so a
	# variable that does not exist is worse there than a missing one.
	$(PYTHON) codegen/check_docs.py

# The widget cases are rendered by real Jinja2; the C renderer must match them
# byte for byte. That is what makes transpiling the template safe.
test: | $(BUILD)
	$(PYTHON) codegen/gen_widget_cases.py
	$(CC) $(ALL_CFLAGS) -Isrc -Itests -o $(BUILD)/test_widget \
	    src/util.c src/widget.c src/widget_gen.c \
	    tests/test_widget.c tests/widget_cases_gen.c $(ALL_LDFLAGS) $(LDLIBS)
	$(BUILD)/test_widget tests/expected
	$(CC) $(ALL_CFLAGS) -Isrc -Itests -o $(BUILD)/test_state \
	    src/util.c src/json.c src/registers.c src/registers_gen.c src/state.c \
	    src/widget.c src/widget_gen.c tests/test_state.c $(ALL_LDFLAGS) $(LDLIBS)
	$(BUILD)/test_state
	$(CC) $(ALL_CFLAGS) -Isrc -Itests -o $(BUILD)/test_http \
	    $(LIB) tests/test_http.c $(ALL_LDFLAGS) $(LDLIBS)
	$(BUILD)/test_http
	$(CC) $(ALL_CFLAGS) -Isrc -Itests -o $(BUILD)/test_config \
	    $(LIB) tests/test_config.c $(ALL_LDFLAGS) $(LDLIBS)
	$(BUILD)/test_config
	# End to end over a real socket speaking real MQTT, which the unit
	# tests never touch.
	$(MAKE) $(BIN)
	sh tests/test_mqtt.sh $(BIN)

run-demo: $(BIN)
	THERMIQ_DEMO=1 $(BIN)

clean:
	rm -rf $(BUILD)
