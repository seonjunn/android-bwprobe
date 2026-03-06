NDK     ?= $(ANDROID_NDK_HOME)
API     ?= 35
ARCH    ?= aarch64
TARGET  := $(ARCH)-linux-android$(API)
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)-clang
CFLAGS  := -O2 -Wall -Wextra -static -std=gnu11 -D_GNU_SOURCE
ADB     ?= adb
DESTDIR ?= /data/local/tmp
BUILD   := build

HEADERS := $(wildcard src/*.h)

.PHONY: all clean runprobe runmatvec runprefault runllcc runicc runbench pulldata

all: $(BUILD)/bwprobe $(BUILD)/matvec $(BUILD)/prefault_exp $(BUILD)/llcc_size $(BUILD)/icc_bw $(BUILD)/bw_bench

$(BUILD)/%: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -lm

$(BUILD):
	mkdir -p $@

data:
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

runicc: $(BUILD)/icc_bw | data
	$(ADB) push $< $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/icc_bw
	$(ADB) shell "$(DESTDIR)/icc_bw > /tmp/icc_bw.csv 2>/tmp/icc_bw.log"
	@echo "--- Results ---" && $(ADB) shell cat /tmp/icc_bw.log
	$(ADB) pull /tmp/icc_bw.csv                        data/icc_bw_summary.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_trace_bwmon.csv     data/icc_bw_trace_bwmon.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_trace_icc.csv       data/icc_bw_trace_icc.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_trace_bus.csv       data/icc_bw_trace_bus.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_context.csv         data/icc_bw_context.csv

runbench: $(BUILD)/bw_bench | data
	$(ADB) push $< $(DESTDIR)/
	$(ADB) shell chmod +x $(DESTDIR)/bw_bench
	$(ADB) shell "$(DESTDIR)/bw_bench > /tmp/bw_bench.csv 2>/tmp/bw_bench.log"
	@echo "--- Results ---" && $(ADB) shell cat /tmp/bw_bench.log
	$(ADB) pull /tmp/bw_bench.csv                          data/bw_bench_summary.csv
	-$(ADB) pull $(DESTDIR)/bw_bench_trace_bwmon.csv       data/bw_bench_trace_bwmon.csv
	-$(ADB) pull $(DESTDIR)/bw_bench_trace_icc.csv         data/bw_bench_trace_icc.csv
	-$(ADB) pull $(DESTDIR)/bw_bench_trace_bus.csv         data/bw_bench_trace_bus.csv
	-$(ADB) pull $(DESTDIR)/bw_bench_context.csv           data/bw_bench_context.csv

pulldata: | data
	-$(ADB) pull /tmp/bwprobe.csv       data/bwprobe_summary.csv
	-$(ADB) pull /tmp/matvec.csv        data/matvec_summary.csv
	-$(ADB) pull /tmp/prefault_exp.csv  data/prefault_summary.csv
	-$(ADB) pull /tmp/llcc_size.csv     data/llcc_size_summary.csv
	-$(ADB) pull /tmp/icc_bw.csv        data/icc_bw_summary.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_trace_bwmon.csv  data/icc_bw_trace_bwmon.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_trace_icc.csv    data/icc_bw_trace_icc.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_trace_bus.csv    data/icc_bw_trace_bus.csv
	-$(ADB) pull $(DESTDIR)/icc_bw_context.csv      data/icc_bw_context.csv
