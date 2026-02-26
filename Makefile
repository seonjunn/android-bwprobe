NDK     ?= $(ANDROID_NDK_HOME)
API     ?= 35
ARCH    ?= aarch64
TARGET  := $(ARCH)-linux-android$(API)
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)-clang
CFLAGS  := -O2 -Wall -Wextra -static -std=gnu11
ADB     ?= adb
DESTDIR ?= /data/local/tmp

.PHONY: all clean runprobe

all: bwprobe

bwprobe: bwprobe.c common.h
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f bwprobe

runprobe: bwprobe
	$(ADB) push bwprobe $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/bwprobe
	$(ADB) shell $(DESTDIR)/bwprobe > /tmp/bwprobe.csv 2>/tmp/bwprobe.log
	@echo "--- Results ---" && cat /tmp/bwprobe.log
