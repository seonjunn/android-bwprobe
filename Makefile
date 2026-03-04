NDK     ?= $(ANDROID_NDK_HOME)
API     ?= 35
ARCH    ?= aarch64
TARGET  := $(ARCH)-linux-android$(API)
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)-clang
CFLAGS  := -O2 -Wall -Wextra -static -std=gnu11
ADB     ?= adb
DESTDIR ?= /data/local/tmp
BUILD   := build

.PHONY: all clean runprobe runmatvec runprefault runllcc

all: $(BUILD)/bwprobe $(BUILD)/matvec $(BUILD)/prefault_exp $(BUILD)/llcc_size

$(BUILD)/%: src/%.c src/common.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -lm

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

runprobe: $(BUILD)/bwprobe
	$(ADB) push $< $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/bwprobe
	$(ADB) shell $(DESTDIR)/bwprobe > /tmp/bwprobe.csv 2>/tmp/bwprobe.log
	@echo "--- Results ---" && cat /tmp/bwprobe.log

runmatvec: $(BUILD)/matvec
	$(ADB) push $< $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/matvec
	$(ADB) shell $(DESTDIR)/matvec > /tmp/matvec.csv 2>/tmp/matvec.log
	@echo "--- Results ---" && cat /tmp/matvec.log

runprefault: $(BUILD)/prefault_exp
	$(ADB) push $< $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/prefault_exp
	$(ADB) shell $(DESTDIR)/prefault_exp > /tmp/prefault_exp.csv 2>/tmp/prefault_exp.log
	@echo "--- Results ---" && cat /tmp/prefault_exp.log

runllcc: $(BUILD)/llcc_size
	$(ADB) push $< $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/llcc_size
	$(ADB) shell $(DESTDIR)/llcc_size > /tmp/llcc_size.csv 2>/tmp/llcc_size.log
	@echo "--- Results ---" && cat /tmp/llcc_size.log
