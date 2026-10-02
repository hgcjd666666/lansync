# lansync -- 局域网文件同步
#
#   make            构建本机 Linux 版      -> build/lansync
#   make android    构建 Android arm64 版  -> build/lansync-android-arm64（静态链接）
#   make test       跑端到端同步测试
#   make demo       构建进度条渲染演示
#   make clean

CC        ?= gcc
CFLAGS    ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE

# 编译时刻，注入二进制：两端握手时比对它，不同就要求更新。
# 注意用 := 立即求值，且没有源文件改动时不会重新编译，所以它稳定反映「这份二进制何时编的」。
BUILD_TS   := $(shell date +%s)
BUILD_DATE := $(shell date '+%Y-%m-%d %H:%M:%S')
CFLAGS     += -DLS_BUILD_TS=$(BUILD_TS) -DLS_BUILD_DATE='"$(BUILD_DATE)"'
LDLIBS    ?= -pthread -lm -lz

SRCDIR    := src
BUILDDIR  := build
OBJDIR    := $(BUILDDIR)/obj
BIN       := $(BUILDDIR)/lansync

SRCS      := $(SRCDIR)/main.c $(SRCDIR)/common.c $(SRCDIR)/server.c $(SRCDIR)/client.c \
             $(SRCDIR)/entropy.c
OBJS      := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
DEPS      := $(OBJS:.o=.d)

# Android 工具链（精简版 NDK，默认 aarch64-linux-android21）
# 纯 C 代码只依赖 bionic 的 libc.so（Android 系统自带），因此动态链接即可，
# 产物几十 KB，push 到 /data/local/tmp 就能直接跑。
ANDROID_CLANG ?= /r/ndk-min/bin/android-clang
ANDROID_BIN   := $(BUILDDIR)/lansync-android-arm64
ANDROID_FLAGS := -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -pthread
# NDK 不默认链接数学库，entropy.c 用了 log()
ANDROID_LIBS  := -lm -lz

.PHONY: all android test demo clean

all: $(BIN)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@echo "构建完成：$@"

android: $(ANDROID_BIN)

$(ANDROID_BIN): $(SRCS)
	@mkdir -p $(BUILDDIR)
	$(ANDROID_CLANG) $(ANDROID_FLAGS) $(SRCS) -o $@ $(ANDROID_LIBS)
	@echo "构建完成（Android aarch64）：$@"

test: $(BIN)
	bash test/test_sync.sh

# 进度条渲染演示（不传输，只看效果）
demo: $(BUILDDIR)/progress-demo

$(BUILDDIR)/progress-demo: test/progress_demo.c $(SRCDIR)/common.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $^ $(LDLIBS)
	@echo "构建完成：$@  ——  直接运行 ./build/progress-demo"

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)
