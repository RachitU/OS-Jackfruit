# Makefile for OS-Jackfruit Multi-Container Runtime
# Supports: make, make ci, make clean

# Kernel build variables
KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

# Kernel module object
obj-m  += monitor.o

# User-space targets
CC     := gcc
CFLAGS := -Wall -Wextra -std=gnu11 -pthread -g

# All targets
.PHONY: all clean ci userspace kmodule

all: userspace kmodule

# ---------------------------------------------------------------------------
# User-space binaries
# ---------------------------------------------------------------------------
userspace: engine memory_hog cpu_hog io_pulse

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o io_pulse io_pulse.c

# ---------------------------------------------------------------------------
# Kernel module
# ---------------------------------------------------------------------------
kmodule:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ---------------------------------------------------------------------------
# CI-safe build (no kernel module, no root needed)
# ---------------------------------------------------------------------------
ci: userspace
	@echo "CI build complete (user-space only)."
	@# Smoke-test: engine with no args must exit non-zero
	@./engine 2>/dev/null; test $$? -ne 0 && echo "Smoke check passed." || (echo "Smoke check FAILED." && exit 1)

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean:
	rm -f engine memory_hog cpu_hog io_pulse
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c .*.cmd modules.order Module.symvers
	rm -rf .tmp_versions
