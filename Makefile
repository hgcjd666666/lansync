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
# -ffunction-sections/-fdata-sections 配合链接时的 --gc-sections，
# 让静态链接进来的 wolfSSL 能按函数（而不是按 .o）裁剪。
CFLAGS    ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE \
             -ffunction-sections -fdata-sections

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

# ---- wolfSSL：TLS 1.3 静态链接 ----
#
# 静态库按平台分开编：Android 是 bionic、Linux 是 glibc，两者 libc 不通用。
# options.h 也随平台不同（WC_RNG_AUTOFORK 一项），所以头路径要选对应那份。
#
# 两个库都带 -ffunction-sections，链接时加 --gc-sections 能按**函数**粒度裁掉
# 没引用的部分（静态链接默认以 .o 为单位，一个 .o 里几十个函数只要引到一个就全拉进来）。
# 实测这一步能省约 25% 体积。
WOLFSSL_DIR      := third_party/wolfssl
WOLFSSL_LIB      := $(WOLFSSL_DIR)/linux/libwolfssl.a
WOLFSSL_INC      := -I$(WOLFSSL_DIR)/linux -I$(WOLFSSL_DIR)/include
ANDROID_WS_LIB   := $(WOLFSSL_DIR)/android/libwolfssl.a
ANDROID_WS_INC   := -I$(WOLFSSL_DIR)/android -I$(WOLFSSL_DIR)/include
#
# 库本身编进来的东西比用到得多（TLS 栈、X.509、AEAD 都会连带一堆），
# 所以要靠 gc-sections 裁。注意别把这两个 -f 去掉。
SECTION_FLAGS := -ffunction-sections -fdata-sections
GC_FLAGS      := -Wl,--gc-sections

STRIP ?= strip
UPX   ?= upx

SRCDIR    := src
BUILDDIR  := build
SRCS      := $(SRCDIR)/main.c $(SRCDIR)/common.c $(SRCDIR)/server.c $(SRCDIR)/client.c \
             $(SRCDIR)/entropy.c $(SRCDIR)/hash.c $(SRCDIR)/tls.c

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
ANDROID_FLAGS   := -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -pthread \
                   $(SECTION_FLAGS) $(ANDROID_WS_INC)
ANDROID_LIBS    := -lm -lz $(HASH_LIB) $(ANDROID_WS_LIB) $(GC_FLAGS)
ANDROID_BIN     := $(BUILDDIR)/lansync-android-arm64
ANDROID_BIN_DBG := $(BUILDDIR)/lansync-android-arm64-d

.PHONY: all linux android test test-hash hash-bench hash-bench-android demo clean

# Linux 出调试版；Android 同时出发布版和调试版
# （发布版不含任何诊断输出，排查发现/连接问题时换调试版跑）
all: $(BIN_DBG) $(ANDROID_BIN) $(ANDROID_BIN_DBG)

linux: $(BIN_DBG)

android: $(ANDROID_BIN) $(ANDROID_BIN_DBG)

# ---------------- Linux 调试版 ----------------

$(OBJDIR_DBG)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WOLFSSL_INC) $(DEBUG_FLAGS) -MMD -MP -c -o $@ $<

$(BIN_DBG): $(OBJS_DBG) $(HASH_LIB) $(WOLFSSL_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $@ $(OBJS_DBG) $(HASH_LIB) $(WOLFSSL_LIB) \
	    $(LDLIBS) $(GC_FLAGS)
	@# 服务端按「可执行文件同目录」找证书/私钥，而测试跑的是 build/ 下的它，
	@# 所以把 certs/ 里那份放过去一份。
	@if [ -f certs/lansync-server.crt ] && [ -f certs/lansync-server.key ]; then \
	    cp -f certs/lansync-server.crt certs/lansync-server.key $(BUILDDIR)/; \
	else \
	    echo "注意：certs/ 里没有证书和私钥，服务端起不来。先跑 sh certs/gen.sh 再 make。"; \
	fi
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

$(ANDROID_BIN): $(SRCS) $(HASH_LIB) $(ANDROID_WS_LIB)
	@mkdir -p $(BUILDDIR)
	$(ANDROID_CLANG) $(ANDROID_FLAGS) $(SRCS) -o $@ $(ANDROID_LIBS)
	@$(STRIP) --strip-all $@
	@$(UPX) -9 -q $@ 2>/dev/null || echo "（upx 跳过，仍是非压缩版）"
	@echo "android 发布版: $@  (`stat -c%s $@` 字节)"

# ---------------- Android 调试版 ----------------
# 带 -DLS_DEBUG：诊断输出（发现请求、判定细节等）都在这个版本里，
# 发布版里它们被编译掉了。不 strip 也不 upx，方便直接看符号和串。

$(ANDROID_BIN_DBG): $(SRCS) $(HASH_LIB) $(ANDROID_WS_LIB)
	@mkdir -p $(BUILDDIR)
	$(ANDROID_CLANG) $(ANDROID_FLAGS) $(DEBUG_FLAGS) $(SRCS) -o $@ $(ANDROID_LIBS)
	@echo "android 调试版: $@  (`stat -c%s $@` 字节)"

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

# 哈希吞吐基准（纯内存，不写盘）。
# 开发机和手机的 CPU 不是一回事，要看真实数字得用 Android 版在手机上跑。
hash-bench: $(BUILDDIR)/hash-bench
	@$(BUILDDIR)/hash-bench

$(BUILDDIR)/hash-bench: test/hash_bench.c $(HASH_LIB) $(WOLFSSL_LIB)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(WOLFSSL_INC) -o $@ test/hash_bench.c $(HASH_LIB) \
	    $(WOLFSSL_LIB) $(LDLIBS) $(GC_FLAGS)

hash-bench-android: $(BUILDDIR)/hash-bench-android

$(BUILDDIR)/hash-bench-android: test/hash_bench.c $(HASH_LIB) $(ANDROID_WS_LIB)
	@mkdir -p $(BUILDDIR)
	$(ANDROID_CLANG) $(ANDROID_FLAGS) -o $@ test/hash_bench.c $(ANDROID_LIBS)
	@echo "推到手机上跑："
	@echo "  adb push $@ /data/local/tmp/hash-bench"
	@echo "  adb shell chmod 755 /data/local/tmp/hash-bench"
	@echo "  adb shell /data/local/tmp/hash-bench"

# 进度条渲染演示（不传输，只看效果）
demo: $(BUILDDIR)/progress-demo

$(BUILDDIR)/progress-demo: test/progress_demo.c $(SRCDIR)/common.c $(SRCDIR)/tls.c $(WOLFSSL_LIB)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(WOLFSSL_INC) $(DEBUG_FLAGS) -I$(SRCDIR) -o $@ $^ $(WOLFSSL_LIB) \
	    $(LDLIBS) $(GC_FLAGS)
	@echo "构建完成：$@  ——  直接运行 ./build/progress-demo"

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)
