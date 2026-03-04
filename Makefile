NDK     ?= $(ANDROID_NDK_HOME)
API     ?= 35
ARCH    ?= aarch64
TARGET  := $(ARCH)-linux-android$(API)
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)-clang
CFLAGS  := -O2 -Wall -Wextra -static -std=gnu11
ADB     ?= adb
DESTDIR ?= /data/local/tmp

.PHONY: all clean runprobe runmatvec runprefault runllcc

all: bwprobe matvec prefault_exp llcc_size

bwprobe: src/bwprobe.c src/common.h
	$(CC) $(CFLAGS) $< -o $@ -lm

matvec: src/matvec.c src/common.h
	$(CC) $(CFLAGS) $< -o $@ -lm

prefault_exp: src/prefault_exp.c src/common.h
	$(CC) $(CFLAGS) $< -o $@ -lm

llcc_size: src/llcc_size.c src/common.h
	$(CC) $(CFLAGS) $< -o $@ -lm

clean:
	rm -f bwprobe matvec prefault_exp llcc_size

runprobe: bwprobe
	$(ADB) push bwprobe $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/bwprobe
	$(ADB) shell $(DESTDIR)/bwprobe > /tmp/bwprobe.csv 2>/tmp/bwprobe.log
	@echo "--- Results ---" && cat /tmp/bwprobe.log

runmatvec: matvec
	$(ADB) push matvec $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/matvec
	$(ADB) shell $(DESTDIR)/matvec > /tmp/matvec.csv 2>/tmp/matvec.log
	@echo "--- Results ---" && cat /tmp/matvec.log

runprefault: prefault_exp
	$(ADB) push prefault_exp $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/prefault_exp
	$(ADB) shell $(DESTDIR)/prefault_exp > /tmp/prefault_exp.csv 2>/tmp/prefault_exp.log
	@echo "--- Results ---" && cat /tmp/prefault_exp.log

runllcc: llcc_size
	$(ADB) push llcc_size $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/llcc_size
	$(ADB) shell $(DESTDIR)/llcc_size > /tmp/llcc_size.csv 2>/tmp/llcc_size.log
	@echo "--- Results ---" && cat /tmp/llcc_size.log
