#!/bin/sh
# load_m4.sh — remoteproc 加载 M4 固件并等待 /dev/ttyRPMSG0（PhaseMd/04 P3-05）
#
# 用法（在 MP157 板端 root 下）：
#   ./load_m4.sh start          # 停止旧实例后加载并等待通道
#   ./load_m4.sh stop           # 停止 M4
#   ./load_m4.sh status         # 打印 remoteproc 状态与通道存在性
#
# 可调环境变量：
#   FW=/lib/firmware/m4_fw.elf     固件路径（构建出的 .elf 先 cp 过去）
#   RP=/sys/class/remoteproc/remoteprocX   实际 X 以 dmesg 为准
# 验收：重启 5 次均稳定出 /dev/ttyRPMSG0。

FW=${FW:-/lib/firmware/m4_fw.elf}
RP=${RP:-/sys/class/remoteproc/remoteproc0}
DEV=/dev/ttyRPMSG0

# --- 释放 FDCAN2(can0)：Linux m_can 驱动不得在 M4 运行期间持有 4400f000.can。
#     幂等(重复执行无害)，失败静默忽略(ASCII 注释，板端安全)。
release_can() {
    ip link set can0 down 2>/dev/null
    dev="/sys/bus/platform/devices/4400f000.can"
    if [ -L "$dev/driver" ]; then
        drv=$(readlink "$dev/driver" 2>/dev/null)
        drv=${drv##*/}
        if [ -n "$drv" ] && [ -w "/sys/bus/platform/drivers/$drv/unbind" ]; then
            echo 4400f000.can > "/sys/bus/platform/drivers/$drv/unbind" 2>/dev/null
        fi
    fi
}

do_status() {
    echo "== status =="
    if [ -d "$RP" ]; then
        echo "remoteproc: $RP  state=$(cat $RP/state 2>/dev/null)"
        echo "firmware   : $(cat $RP/firmware 2>/dev/null)"
    else
        echo "remoteproc: 未找到 $RP（ls /sys/class/remoteproc/ 查实际 X）"
    fi
    [ -e "$DEV" ] && echo "rpmsg 通道: $DEV 存在" || echo "rpmsg 通道: $DEV 不存在"
}

wait_dev() {
    i=0
    while [ $i -lt 50 ]; do
        bind_channel
        [ -e "$DEV" ] && return 0
        i=$((i + 1))
        sleep 0.2
    done
    return 1
}

# 5.4 BSP: 远端 NS 建出的 rpmsg-tty 通道不会自动绑 rpmsg_tty 驱动
# (板验 2026-09-10: creating channel 有, 但 /dev/ttyRPMSG0 需手动 bind 才出现)。
# ⚠️ 直接 bind 会报 "No such device"(5.4 rpmsg 总线 id 匹配 bug)——
#    必须先写 driver_override 再 bind(实测有效)。已绑/无通道则无事可做, 幂等。
bind_channel() {
    for d in /sys/bus/rpmsg/devices/*rpmsg-tty*; do
        [ -e "$d" ] || continue
        [ -e "$d/driver" ] && continue
        echo rpmsg_tty > "$d/driver_override" 2>/dev/null
        echo "${d##*/}" > /sys/bus/rpmsg/drivers/rpmsg_tty/bind 2>/dev/null
    done
}

case "$1" in
    start)
        release_can
        if [ ! -f "$FW" ]; then
            echo "固件不存在: $FW （先把编译出的 .elf cp 到 /lib/firmware/）" >&2
            exit 2
        fi
        # 若已运行先 stop（重复 start 需先复位）
        if [ -d "$RP" ] && [ "$(cat $RP/state 2>/dev/null)" = "running" ]; then
            echo stop > "$RP/state" 2>/dev/null
            sleep 1
        fi
        # 指定固件名: 内核默认按 DT firmware-name(rproc-m4-fw) 找, 不写会 -2 失败
        echo "$(basename "$FW")" > "$RP/firmware" 2>/dev/null
        echo start > "$RP/state" 2>/dev/null || {
            echo "remoteproc start 失败（$RP 是否存在？dmesg 查原因）" >&2
            exit 1
        }
        echo "remoteproc start 已下发，等待 $DEV ..."
        if wait_dev; then
            echo "OK: $DEV 就绪"
        else
            echo "超时: $DEV 未出现（看 dmesg：rsc_table/固件路径/CONFIG_RPMSG_CHAR）" >&2
            exit 1
        fi
        ;;
    stop)
        if [ -d "$RP" ]; then
            echo stop > "$RP/state" 2>/dev/null && echo "M4 已停止"
        else
            echo "未找到 $RP" >&2
            exit 1
        fi
        ;;
    status)
        do_status
        ;;
    *)
        echo "用法: $0 {start|stop|status}  （FW/RP 可用环境变量覆盖）" >&2
        exit 1
        ;;
esac
