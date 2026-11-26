# Build the standalone userspace driver and capture tool.
#
#   make            build build/fpc-capture
#   make install    install the tool and the udev rule (needs root)
#   make clean

PREFIX     ?= /usr/local
CC         ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc
CFLAGS  += $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LDLIBS  += $(shell $(PKG_CONFIG) --libs libusb-1.0)

# Hooks for CI (sanitizers, extra warnings) that survive a command line
# override of CFLAGS/LDLIBS.
CFLAGS  += $(EXTRA_CFLAGS)
LDLIBS  += $(EXTRA_LDLIBS)

BUILD   := build
OBJS    := $(BUILD)/fpc_usb.o $(BUILD)/fpc_capture.o
TARGET  := $(BUILD)/fpc-capture
TESTBIN := $(BUILD)/test-frames
MATCHTEST := $(BUILD)/test-match
EVAL      := $(BUILD)/fpc-match-eval
BENCH   := $(BUILD)/fpc-bench

# fpc-bench reuses the NBIS matcher of libfprint, so it needs a checkout
# that libfprint/install.sh has already built (build/libfprint by default).
LIBFPRINT ?= $(BUILD)/libfprint
NBIS      := $(LIBFPRINT)/_build/libfprint/libnbis.a

.PHONY: all clean install uninstall test bench eval eval-smoke

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/fpc_usb.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDLIBS)

# Offline checks of the recovered framing (no hardware required).
$(TESTBIN): tests/test_frames.c $(BUILD)/fpc_usb.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: $(TESTBIN) $(MATCHTEST)
	./$(TESTBIN)
	./$(MATCHTEST)

$(BUILD)/fpc_match.o: src/fpc_match.c src/fpc_match.h | $(BUILD)
	$(CC) $(CFLAGS) -std=c99 -c $< -o $@

$(MATCHTEST): tests/test_match.c $(BUILD)/fpc_match.o
	$(CC) $(CFLAGS) -std=c99 $^ -o $@ $(EXTRA_LDLIBS) -lm

eval: $(EVAL)

$(EVAL): tools/fpc_match_eval.c $(BUILD)/fpc_match.o
	$(CC) $(CFLAGS) -std=c99 $^ -o $@ $(EXTRA_LDLIBS) -lm

eval-smoke: $(EVAL)
	sh tests/test_match_eval.sh ./$(EVAL) $(BUILD)

# Offline matcher: scores PGM files captured with fpc-capture exactly the
# way libfprint scores them during a verification.
bench: $(BENCH)

$(BENCH): tools/fpc_bench.c $(NBIS) | $(BUILD)
	@test -r "$(NBIS)" || { \
		echo "$(NBIS) is missing - run libfprint/install.sh first"; \
		exit 1; }
	$(CC) $(CFLAGS) \
		-I$(LIBFPRINT)/libfprint \
		-I$(LIBFPRINT)/libfprint/nbis/include \
		-I$(LIBFPRINT)/libfprint/nbis/libfprint-include \
		$(shell $(PKG_CONFIG) --cflags glib-2.0) \
		$< $(NBIS) -o $@ \
		$(shell $(PKG_CONFIG) --libs glib-2.0) -lm

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/fpc-capture
	install -Dm644 udev/60-fpc-keyboard.rules \
		$(DESTDIR)/etc/udev/rules.d/60-fpc-keyboard.rules
	@echo "Run: udevadm control --reload && udevadm trigger"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/fpc-capture
	rm -f $(DESTDIR)/etc/udev/rules.d/60-fpc-keyboard.rules

clean:
	rm -rf $(BUILD)
