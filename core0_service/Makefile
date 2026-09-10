# core0_service Makefile — A7-Core0 Linux 侧（第3步 RPMSG + 第4步业务守护）
#
# 用法：
#   make                                                  # 本机编译（板端原生 gcc / PC Linux）
#   make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-     # 交叉编译（book 上的 100ASK SDK）
#   make selftest                                         # 只编译并运行纯逻辑自测（不需要板子）
#   make host-check                                       # 只做语法检查（Linux 宿主）
#   make host-check-win                                   # 语法检查（Windows/MinGW 宿主，用桩头文件）
#
# 产物：
#   core0_business   业务守护进程（P4-01~P4-08，G4 验收入口）
#   core0_selftest   纯逻辑自测（宿主可跑，见 make selftest）
#   core1_stub       Core1 打桩进程（联调：心跳/结果/远程开闸请求）
#   rpmsg_demo       第3步通道演示（P3-06~P3-10）
#   tools/rpmsg_cli/rpmsg_cli  第3步打桩 CLI（P3-11）
#
# 说明：core0_business 依赖 Linux 专有接口（epoll/signalfd/timerfd/
#       shm_open/eventfd/termios），只能在 Linux 上编译；纯逻辑层
#       （business/app_config/whitelist/log/store）与 core0_selftest
#       可在任意宿主编译运行 → Windows 上也能 make selftest /
#       host-check-win（桩头文件见 tools/hostcheck/，不参与板上构建）。

CC      ?= gcc
# GNU make 自带内建默认 CC=cc，`?=` 不会覆盖它；这里显式纠正为 gcc
# （交叉编译用 make CROSS_COMPILE=... 不动 CC）。
ifeq ($(origin CC),default)
CC := gcc
endif
# 交叉编译：make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
# （GNU 惯例是 CROSS_COMPILE；CROSS 为等价旧别名，两者都可）
CROSS_COMPILE ?=
CROSS         ?= $(CROSS_COMPILE)
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  ?= -lpthread
# librt: shm_open() lives there on glibc < 2.34 (the arm-buildroot toolchain
# here is gcc 8.4 / glibc ~2.31) -> "undefined reference to shm_open".
# MinGW has no librt, so only add it for non-Windows hosts (the host selftest
# build runs on Windows).
ifneq ($(OS),Windows_NT)
LDLIBS += -lrt
endif

INC     := -Irpmsg -Ibusiness -Iipc_shm -Istorage

RPMSG_SRC   := rpmsg/rpmsg_proto.c rpmsg/rpmsg_link.c
BIZ_SRC     := business/business.c business/app_config.c business/whitelist.c business/log.c
IPC_SRC     := ipc_shm/ipc_shm.c
STORE_SRC   := storage/store.c
CORE0_LIB   := $(BIZ_SRC) $(IPC_SRC) $(STORE_SRC) $(RPMSG_SRC)
SELFTEST_LIB:= $(BIZ_SRC) $(STORE_SRC)

BIN_BIZ  := core0_business
BIN_TEST := core0_selftest
BIN_STUB := core1_stub
BIN_DEMO := rpmsg_demo
BIN_CLI  := tools/rpmsg_cli/rpmsg_cli

HDRS     := $(wildcard rpmsg/*.h business/*.h ipc_shm/*.h storage/*.h)

ALL_SRC  := core0_main.c $(CORE0_LIB) tools/core1_stub.c tools/core0_selftest.c \
            tools/rpmsg_cli/rpmsg_cli.c rpmsg/rpmsg_demo.c

# Windows/MinGW 宿主缺失 sys/{mman,eventfd,epoll,signalfd,timerfd}.h 与
# termios.h/poll.h：host-check-win 用 tools/hostcheck/ 桩头文件 + 强制包含的
# 兼容头（补 O_NOCTTY/O_NONBLOCK/pipe 等）。Linux 宿主用真头文件。
WIN_FLAGS := -Itools/hostcheck -include tools/hostcheck/host_compat.h

# 自测可执行文件在 Windows 上带 .exe 后缀，运行时不加 ./
ifeq ($(OS),Windows_NT)
RUN_PREFIX :=
else
RUN_PREFIX := ./
endif

all: $(BIN_BIZ) $(BIN_STUB) $(BIN_TEST) $(BIN_DEMO) $(BIN_CLI)

# 业务守护（第4步主产物）
$(BIN_BIZ): core0_main.c $(CORE0_LIB) $(HDRS)
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ core0_main.c $(CORE0_LIB) $(LDLIBS)

# Core1 打桩（联调）
$(BIN_STUB): tools/core1_stub.c ipc_shm/park_shm.h
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ $< $(LDLIBS)

# 纯逻辑自测（不需要板子；Windows 亦可）
$(BIN_TEST): tools/core0_selftest.c $(SELFTEST_LIB) $(HDRS)
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ tools/core0_selftest.c $(SELFTEST_LIB) $(LDLIBS)

# 第3步通道演示 + 打桩 CLI
$(BIN_DEMO): rpmsg/rpmsg_demo.c $(RPMSG_SRC) $(HDRS)
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ rpmsg/rpmsg_demo.c $(RPMSG_SRC) $(LDLIBS)

$(BIN_CLI): tools/rpmsg_cli/rpmsg_cli.c rpmsg/rpmsg_proto.c $(HDRS)
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ tools/rpmsg_cli/rpmsg_cli.c rpmsg/rpmsg_proto.c $(LDLIBS)

# 跑自测：退出码 0 = 全部用例通过
selftest: $(BIN_TEST)
	$(RUN_PREFIX)$(BIN_TEST)

# 只做语法检查（逐文件模式规则，Linux/Windows 的 shell 都能跑）
# 只做语法检查：用 $(eval) 生成"逐文件"规则，避免依赖 sh 的 for 循环
# （Windows 的 cmd 没有 for/do/done；且 make 的模式规则对带目录的目标只
#  匹配文件名部分，故不能用 hc-% 覆盖子目录源文件）。
define HC_RULE
hc-$(subst /,_,$(1)):
	@echo "check $(1)"
	@$$(CROSS)$$(CC) $$(CFLAGS) $$(INC) -fsyntax-only $(1)
endef
define HCW_RULE
hcw-$(subst /,_,$(1)):
	@echo "check $(1)"
	@$$(CROSS)$$(CC) $$(CFLAGS) $$(INC) $$(WIN_FLAGS) -fsyntax-only $(1)
endef
$(foreach f,$(ALL_SRC),$(eval $(call HC_RULE,$(f))))
$(foreach f,$(ALL_SRC),$(eval $(call HCW_RULE,$(f))))

HC_TARGETS  := $(addprefix hc-,$(subst /,_,$(ALL_SRC)))
HCW_TARGETS := $(addprefix hcw-,$(subst /,_,$(ALL_SRC)))

host-check: $(HC_TARGETS)
	@echo "host-check OK ($(words $(ALL_SRC)) files)"

host-check-win: $(HCW_TARGETS)
	@echo "host-check-win OK ($(words $(ALL_SRC)) files)"

clean:
	rm -f $(BIN_BIZ) $(BIN_TEST) $(BIN_STUB) $(BIN_DEMO) $(BIN_CLI) *.o \
	      rpmsg/*.o business/*.o ipc_shm/*.o storage/*.o tools/*.o tools/rpmsg_cli/*.o

.PHONY: all selftest host-check host-check-win clean
