# lansync -- 局域网文件夹同步
#
#   make            构建：
#                     build/lansync_d                  Linux 调试版
#                     build/lansync-android-arm64      Android 发布版（strip + upx -9）
#   make test       跑端到端同步测试
#   make demo       构建进度条渲染演示
#   make clean
#
# 当前不需要的组合（Linux 的 release、Android 的 debug）规则保留在下面并注释掉了，
# 需要时把注释解开即可。
#
# 发布版不含任何调试输出：没定义 LS_DEBUG 时 ls_dbg 展开成 ((void)0)，
# 连参数字符串都不会进二进制。

CC        ?= gcc
CFLAGS    ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE

# 编译时刻，注入二进制：两端握手时比对它，不同就要求更新。
# 用 := 立即求值，且没有源文件改动时不会重新编译，所以它稳定反映「这份二进制何时编的」。
BUILD_TS   := $(shell date +%s)
BUILD_DATE := $(shell date '+%Y-%m-%d %H:%M:%S')
CFLAGS     += -DLS_BUILD_TS=$(BUILD_TS) -DLS_BUILD_DATE='"$(BUILD_DATE)"'

DEBUG_FLAGS := -DLS_DEBUG
LDLIBS      ?= -pthread -lm -lz

# 内容哈希用的 gxhash 静态库（Rust 编的，只含真正被引用的那一个 .o）。
# 它是 aarch64 目标文件，所以别的架构不链它——src/hash.c 会退化成
# 「哈希功能不可用」，程序照跑，只是 --verify hash 没效果。
HASH_LIB :=
ifeq ($(shell uname -m),aarch64)
HASH_LIB := third_party/libgxhash_arm64.a
else
$(warning 当前架构不是 aarch64，内容哈希功能将不可用)
endif

STRIP ?= strip
UPX   ?= upx

SRCDIR    := src
BUILDDIR  := build
SRCS      := $(SRCDIR)/main.c $(SRCDIR)/common.c $(SRCDIR)/server.c $(SRCDIR)/client.c \
             $(SRCDIR)/entropy.c $(SRCDIR)/hash.c

OBJDIR     := $(BUILDDIR)/obj
OBJDIR_DBG := $(BUILDDIR)/obj-dbg
OBJS       := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
OBJS_DBG   := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR_DBG)/%.o)
DEPS       := $(OBJS:.o=.d) $(OBJS_DBG:.o=.d)

BIN     := $(BUILDDIR)/lansync
BIN_DBG := $(BUILDDIR)/lansync_d

# Android（精简版 NDK，默认 aarch64-linux-android21）
# 纯 C 只依赖 bionic 的 libc/libdl/libm/libz（都是系统自带），动态链接即可。
ANDROID_CLANG   ?= /r/ndk-min/bin/android-clang
ANDROID_FLAGS   := -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -pthread
ANDROID_LIBS    := -lm -lz $(HASH_LIB)
ANDROID_BIN     := $(BUILDDIR)/lansync-android-arm64
ANDROID_BIN_DBG := $(BUILDDIR)/lansync-android-arm64-d

.PHONY: all linux android test test-hash demo clean

# Linux 出调试版，Android 出发布版
all: $(BIN_DBG) $(ANDROID_BIN)

linux: $(BIN_DBG)

android: $(ANDROID_BIN)

# ---------------- Linux 调试版 ----------------

$(OBJDIR_DBG)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -MMD -MP -c -o $@ $<

$(BIN_DBG): $(OBJS_DBG) $(HASH_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $@ $(OBJS_DBG) $(HASH_LIB) $(LDLIBS)
	@echo "linux  调试版: $@  (`stat -c%s $@` 字节)"

# ---------------- Linux 发布版（当前不需要，保留） ----------------
#
# $(OBJDIR)/%.o: $(SRCDIR)/%.c
# 	@mkdir -p $(dir $@)
# 	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<
#
# $(BIN): $(OBJS)
# 	@mkdir -p $(dir $@)
# 	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)
# 	@$(STRIP) --strip-all $@
# 	@$(UPX) -9 -q $@ 2>/dev/null || echo "（upx 跳过）"
# 	@echo "linux  发布版: $@"

# ---------------- Android 发布版 ----------------

$(ANDROID_BIN): $(SRCS) $(HASH_LIB)
	@mkdir -p $(BUILDDIR)
	$(ANDROID_CLANG) $(ANDROID_FLAGS) $(SRCS) -o $@ $(ANDROID_LIBS)
	@$(STRIP) --strip-all $@
	@$(UPX) -9 -q $@ 2>/dev/null || echo "（upx 跳过，仍是非压缩版）"
	@echo "android 发布版: $@  (`stat -c%s $@` 字节)"

# ---------------- Android 调试版（当前不需要，保留） ----------------
#
# $(ANDROID_BIN_DBG): $(SRCS)
# 	@mkdir -p $(BUILDDIR)
# 	$(ANDROID_CLANG) $(ANDROID_FLAGS) $(DEBUG_FLAGS) $(SRCS) -o $@ $(ANDROID_LIBS)
# 	@echo "android 调试版: $@"

# ---------------- 其他 ----------------

test: $(BIN_DBG) test-hash
	bash test/test_sync.sh

# 用上游 is_stable 的原始期望值校验 third_party 里的 gxhash。
# 整个内容哈希方案依赖「两端算出同一个值」，这条测试钉的就是那个前提。
ifeq ($(HASH_LIB),)
test-hash:
	@echo "（当前架构不是 aarch64，跳过 gxhash 向量测试）"
else
test-hash: $(BUILDDIR)/hash-vectors
	@$(BUILDDIR)/hash-vectors

$(BUILDDIR)/hash-vectors: test/hash_vectors.c $(HASH_LIB)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ test/hash_vectors.c $(HASH_LIB)
endif

# 进度条渲染演示（不传输，只看效果）
demo: $(BUILDDIR)/progress-demo

$(BUILDDIR)/progress-demo: test/progress_demo.c $(SRCDIR)/common.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -I$(SRCDIR) -o $@ $^ $(LDLIBS)
	@echo "构建完成：$@  ——  直接运行 ./build/progress-demo"

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)
