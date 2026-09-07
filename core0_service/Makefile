# core0_service Makefile — A7-Core0 Linux 侧（第3步：RPMSG 通道 + 打桩工具）
#
# 用法（在 ST SDK / 板端 Linux 环境）：
#   make                 # 本机 gcc（板端原生）
#   make CROSS_COMPILE=arm-openstlinux-linux-gnueabihf-   # 交叉编译
#
# 产物：
#   rpmsg_demo            通道演示（P3-06~P3-10 验收入口）
#   tools/rpmsg_cli/rpmsg_cli   打桩 CLI（P3-11）
# 说明：本环境无 arm-none-eabi / Linux 工具链，未在沙箱内编译；如需先在
#       PC(Linux) 冒烟可 make host-check（见下）。

CC      ?= gcc
CROSS   ?=
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  ?= -lpthread

RPMSG_SRC := rpmsg/rpmsg_proto.c rpmsg/rpmsg_link.c
RPMSG_INC := -Irpmsg

BIN_DEMO := rpmsg_demo
BIN_CLI  := tools/rpmsg_cli/rpmsg_cli

all: $(BIN_DEMO) $(BIN_CLI)

$(BIN_DEMO): rpmsg/rpmsg_demo.c $(RPMSG_SRC) rpmsg/rpmsg_types.h rpmsg/rpmsg_proto.h rpmsg/rpmsg_link.h
	$(CROSS)$(CC) $(CFLAGS) $(RPMSG_INC) -o $@ $< $(RPMSG_SRC) $(LDLIBS)

$(BIN_CLI): tools/rpmsg_cli/rpmsg_cli.c rpmsg/rpmsg_proto.c rpmsg/rpmsg_types.h rpmsg/rpmsg_proto.h
	$(CROSS)$(CC) $(CFLAGS) $(RPMSG_INC) -o $@ tools/rpmsg_cli/rpmsg_cli.c rpmsg/rpmsg_proto.c $(LDLIBS)

# PC(Linux) 冒烟：只编不跑，验证语法（无板子时用）
host-check:
	$(CC) $(CFLAGS) $(RPMSG_INC) -fsyntax-only rpmsg/rpmsg_proto.c
	$(CC) $(CFLAGS) $(RPMSG_INC) -fsyntax-only rpmsg/rpmsg_link.c
	$(CC) $(CFLAGS) $(RPMSG_INC) -fsyntax-only rpmsg/rpmsg_demo.c
	$(CC) $(CFLAGS) $(RPMSG_INC) -fsyntax-only tools/rpmsg_cli/rpmsg_cli.c

clean:
	rm -f $(BIN_DEMO) $(BIN_CLI) *.o rpmsg/*.o tools/rpmsg_cli/*.o

.PHONY: all clean host-check
