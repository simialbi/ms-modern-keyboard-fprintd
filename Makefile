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

.PHONY: all clean install uninstall test

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

test: $(TESTBIN)
	./$(TESTBIN)

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
