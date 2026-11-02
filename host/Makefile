# host/Makefile — native build of the firmware's integer pipeline.
#
# No RISC-V toolchain required. Compiles the real firmware sources with
# -DFW_HOST_BUILD, against the software device model in mock_soc.c, and
# runs test_host.c. See README.md in this directory for what that does and
# does not prove.
#
#   make run           build and run the harness
#   make syntax-check  parse the target-only sources with the host
#                      compiler (main.c, trap.c, sensor.c, test/*.c)
#   make clean

CC      ?= cc
ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))..
OUT     := $(ROOT)/host/build

CFLAGS  := -std=c99 -O2 -g -Wall -Wextra -DFW_HOST_BUILD \
           -I$(ROOT)/include -I$(ROOT)/telemetry -I$(ROOT)/host

# ── macOS SDK ──────────────────────────────────────────────────────────────
# On macOS, clang needs a sysroot to find stdio.h and friends. Normally
# `cc` gets one from xcrun, but when the Xcode licence has not been
# accepted every xcrun-backed tool refuses to run, so this Makefile locates
# the SDK itself rather than depending on SDKROOT being exported by
# whoever invokes it. An SDKROOT already in the environment or passed on
# the command line wins (?=), and on Linux or anywhere without these
# directories nothing is added.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SDKROOT ?= $(firstword \
      $(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk) \
      $(wildcard /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk))
  ifneq ($(SDKROOT),)
    CFLAGS += -isysroot $(SDKROOT)
  endif
endif

# Real firmware sources. src/main.c is excluded (its inference loop never
# returns) and src/crt0.S / src/trap.c are excluded (RV32 startup and the
# machine-mode trap handler have no host equivalent -- mock_soc.c defines
# the ISR flag symbols the drivers expect instead). src/sensor.c is
# replaced by host/mock_sensor.c, which is the byte-source swap sensor.h
# already describes for bring-up.
FW_SRCS := \
    $(ROOT)/src/conv1d.c              \
    $(ROOT)/src/features.c            \
    $(ROOT)/src/inference.c           \
    $(ROOT)/src/model_weights_data.c  \
    $(ROOT)/src/latency.c             \
    $(ROOT)/src/snn.c                 \
    $(ROOT)/src/uart.c                \
    $(ROOT)/src/intc.c                \
    $(ROOT)/src/timer.c               \
    $(ROOT)/src/dma.c                 \
    $(ROOT)/src/gpio.c                \
    $(ROOT)/src/spi.c                 \
    $(ROOT)/src/i2c.c                 \
    $(ROOT)/src/pwm.c                 \
    $(ROOT)/telemetry/crc16.c         \
    $(ROOT)/telemetry/telemetry.c

HOST_SRCS := \
    $(ROOT)/host/mock_soc.c     \
    $(ROOT)/host/mock_sensor.c  \
    $(ROOT)/host/test_host.c

# Target-only sources: they cannot be linked on the host (RV32 startup,
# the machine-mode trap handler, the UART-interrupt sensor path, and the
# test images' own main()s), but they can still be parsed, which catches
# a typo or a stale symbol without a RISC-V toolchain. Two warning classes
# are expected and suppressed: uint32_t<->pointer casts, which are correct
# on RV32 and wrong-sized on a 64-bit host, and the unknown 'interrupt'
# attribute, which only the RISC-V backend implements.
TARGET_ONLY := \
    $(ROOT)/src/main.c    \
    $(ROOT)/src/trap.c    \
    $(ROOT)/src/sensor.c  \
    $(wildcard $(ROOT)/test/test_*.c)

.PHONY: run syntax-check clean

run: $(OUT)/test_host
	@echo
	@$(OUT)/test_host

$(OUT)/test_host: $(FW_SRCS) $(HOST_SRCS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(FW_SRCS) $(HOST_SRCS) -o $@

syntax-check:
	@for f in $(TARGET_ONLY); do \
	    $(CC) -fsyntax-only $(CFLAGS) \
	          -Wno-pointer-to-int-cast -Wno-unknown-attributes \
	          -Wno-asm-operand-widths \
	          $$f || exit 1; \
	    echo "  parsed $$(basename $$f)"; \
	done
	@echo "syntax-check: all target-only sources parse"

clean:
	rm -rf $(OUT)
