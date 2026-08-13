# thermiq-bridge
#
# Generated sources are checked in, so a plain `make` needs only a C compiler.
# Run `make generate` after changing the register table, the widget template
# or anything under web/ - CI fails if the checked-in output is stale.

CC ?= cc
PYTHON ?= python3
TARGET ?=

CFLAGS ?= -Os -std=c11 -D_POSIX_C_SOURCE=200809L \
          -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion \
          -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wpointer-arith \
          -fno-common -ffunction-sections -fdata-sections
# Hardening: stack protector and fortified libc calls. _FORTIFY_SOURCE is left
# alone under SAN=1 below - the sanitizers predefine it to 0 and check the same
# calls more thoroughly, so setting it here only produces a redefinition
# warning on every file.
CFLAGS += -fstack-protector-strong
ifneq ($(SAN),1)
CFLAGS += -D_FORTIFY_SOURCE=2
endif

# _POSIX_C_SOURCE above asks for strict POSIX, and macOS honours that by
# hiding anything BSD-flavoured - INADDR_LOOPBACK among it. The target is
# Linux either way, but a program the maintainer cannot build on the laptop in
# front of them is a program that only CI can tell you about.
ifeq ($(shell uname -s),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
# Apple's linker spells --gc-sections -dead_strip.
LDFLAGS ?= -Wl,-dead_strip
else
LDFLAGS ?= -Wl,--gc-sections
endif
LDLIBS ?= -lm

ifneq ($(TARGET),)
CFLAGS += -target $(TARGET)
LDFLAGS += -target $(TARGET)
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
CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
          -fno-sanitize-recover=all
LDFLAGS += -fsanitize=address,undefined
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
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

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
	$(CC) $(CFLAGS) -Isrc -Itests -o $(BUILD)/test_widget \
	    src/util.c src/widget.c src/widget_gen.c \
	    tests/test_widget.c tests/widget_cases_gen.c $(LDFLAGS) $(LDLIBS)
	$(BUILD)/test_widget tests/expected
	$(CC) $(CFLAGS) -Isrc -Itests -o $(BUILD)/test_state \
	    src/util.c src/json.c src/registers.c src/registers_gen.c src/state.c \
	    src/widget.c src/widget_gen.c tests/test_state.c $(LDFLAGS) $(LDLIBS)
	$(BUILD)/test_state
	$(CC) $(CFLAGS) -Isrc -Itests -o $(BUILD)/test_http \
	    $(LIB) tests/test_http.c $(LDFLAGS) $(LDLIBS)
	$(BUILD)/test_http
	$(CC) $(CFLAGS) -Isrc -Itests -o $(BUILD)/test_config \
	    $(LIB) tests/test_config.c $(LDFLAGS) $(LDLIBS)
	$(BUILD)/test_config
	# End to end over a real socket speaking real MQTT, which the unit
	# tests never touch.
	$(MAKE) $(BIN)
	sh tests/test_mqtt.sh $(BIN)

run-demo: $(BIN)
	THERMIQ_DEMO=1 $(BIN)

clean:
	rm -rf $(BUILD)
