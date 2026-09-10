# Core0 业务守护 G4 验收手册

> 对象：第4步 P4-01~P4-08（验收门 G4），权威需求 `.codeartsdoer/specs/core0_business/spec.md`，
> 任务分解 `PhaseMd/05_第4步_Linux本地业务.md`。协议依据 `docs/protocols.md` §3/§4。
> 板端事实：board `root@192.168.189.65`（无 gcc/make/sudo），交叉编译在 book；
> **粘到板子终端的任何脚本/命令必须纯 ASCII**。

本手册分三层，可分别独立执行；三层都过 = G4 全绿。

| 层级 | 需要什么 | 能证明什么 | 预计耗时 |
|---|---|---|---|
| L1 宿主逻辑验收 | 任意 Linux 或 Windows/MinGW | 状态机/白名单/计数/超时/配置/日志/存储**全部逻辑正确** | 2 分钟 |
| L2 板端离线验收 | MP157 板（**不需要 M4/C8T6**） | 进程可跑、shm v3 契约、watchdog、远程请求脉冲、热加载、日志与存储开关 | 15 分钟 |
| L3 板端端到端 | MP157 + M4（remoteproc）+ C8T6 上电 | 车到位→开闸全链路、KPI 时延、真机降级 | 40 分钟 |

---

## L1 宿主逻辑验收（spec 5.6.3 的"打桩验收"主体）

```bash
cd core0_service
make selftest              # Linux/macOS
# Windows/MinGW:  mingw32-make CC=D:/mingw64/bin/gcc.exe selftest
```

**通过判据**：末行 `142 checks, 0 failures`，退出码 0。

```bash
make host-check            # Linux 语法检查（13 文件 -Wall -Wextra）
make host-check-win        # Windows 桩头文件版本，需 -Itools/hostcheck
```

**用例 ↔ G4 条款映射**

| G4 条款 | 用例 | 覆盖点 |
|---|---|---|
| P4-01 状态机全边 | S1/S2/S5/S6/S7/S11/S12 | 7 态全边、重复边沿、链路边沿 |
| P4-02 超时降级 | S2/S3/S4 | 3s 拒绝、`cloud_pending` 延到 6s、Core1 失联立即降级 |
| P4-03 白名单 | S5/S6/S10 | 放行/拒绝/过期/禁用、配置增删生效 |
| P4-04 车位计数 | S8/S9 | 10 轮进出准确、重复不重计、限幅 [0,总位数] |
| P4-05 配置与控制 | S7/S10 | 远程开/关闸脉冲自清零、配置解析与非法值拒绝、阈值下发 |
| P4-06 Core1 监测 | S4 | 心跳停 → 故障字 bit2 |
| P4-07 存储 | S13 | 默认关=no-op；`-DENABLE_STORAGE=1` 版落盘 |
| P4-08 日志 | S13 | 级别过滤、`HH:MM:SS.mmm LEVEL [tag]` 格式 |

存储开关两侧都要跑（两种编译各一遍）：

```bash
gcc -O2 -Wall -Wextra -std=gnu11 -Irpmsg -Ibusiness -Iipc_shm -Istorage \
    -DENABLE_STORAGE=1 -o st_en tools/core0_selftest.c business/business.c \
    business/app_config.c business/whitelist.c business/log.c storage/store.c -lpthread
./st_en        # 同样 142 checks, 0 failures
```

---

## L2 板端离线验收（不需 M4/C8T6）

### 2.1 交叉编译 + 部署（在 book）

```bash
cd <repo>/core0_service
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf- core0_business core1_stub
file core0_business            # 应为 ARM 32-bit ELF
scp core0_business core1_stub core0.conf.example root@192.168.189.65:/root/c0/
```

板端（纯 ASCII 命令）：

```bash
cd /root/c0 && cp core0.conf.example core0.conf && chmod +x core0_business core1_stub
```

### 2.2 启动并看首屏日志

```bash
cd /root/c0 && ./core0_business -c core0.conf
```

期望（顺序可见、tag 固定）：

```
[config] config file: core0.conf
[biz] business daemon created: state=IDLE slots=20 wl=3 ...
[main] core0 business daemon running: dev=/dev/ttyRPMSG0 conf=core0.conf evt_c1_fd=-1
[rpmsg] link DOWN                       <- 还没起 M4，正常
```

**L2-1 shm 契约**：另开一个终端

```bash
ls -l /dev/shm/park_shm        # 期望 size = 76
```

起打桩 Core1（**必须在 core0 之后**，core0 每次启动重建 shm）：

```bash
cd /root/c0 && ./core1_stub
# 期望：core1_stub attached to /park_shm (v3)
```

若报 `version mismatch` / `bad magic` / `shm_open failed` → core0 没在跑，或两边版本不一致（一起重编）。

### 2.3 逐项检查

| 编号 | 操作（core1_stub 里敲） | 期望（core0 终端） | 判据 |
|---|---|---|---|
| L2-2 watchdog 置位 | `h`（关心跳）后等 3~4s | `[watchdog] core1 heartbeat lost >=3s: fault bit2 set` | ≤3s 出现 |
| L2-3 故障字可见 | `p` 看 `fault=0x..` | `fault` 含 **0x04**；`link` 的 bit2（core1-online）为 0 | shm 字段与日志一致 |
| L2-4 心跳恢复 | 再敲 `h`（开心跳） | `[watchdog] core1 heartbeat alive again`，`p` 里 fault 清掉 bit2 | 无人工干预自愈 |
| L2-5 远程开闸请求 | `o` | `[remote] gate open request from core1 (ui)`，随后（离线时）`[gate] OPEN 0x11 command send failed (rpmsg down?)` | 请求被消费（脉冲自清零，`p` 不再重复触发） |
| L2-6 关闸 + 观测态幂等 | `c` | `[remote] gate close request from core1 (ui)` + `[gate] CLOSE 0x12 skipped: gate already closed (observed)` | 观测态幂等生效（离线时 M4 不上报闸位，故走"已关"分支） |
| L2-7 配置热加载 | 编辑 `core0.conf` 把 `total_slots = 20` 改成 `5` 保存 | ≤500ms 内（tick 200ms）`[config] hot reload done: ok` + `[config] config applied: slots=5 ...` | ≤500ms KPI；`p` 里 `free` 同步变化 |
| L2-8 日志级别 | 另起一次 `./core0_business -c core0.conf -l warn` | 只有 WARN/ERROR，无 INFO | 级别过滤生效 |
| L2-9 存储开关 | 用 `-DENABLE_STORAGE=1` 编的版本 + `storage_dir = /root/c0` | 生成 `/root/c0/events.log`、`/root/c0/gate_log` | 默认关版本不生成任何文件 |
| L2-10 优雅退出 | Ctrl-C（SIGINT） | `[main] signal 2 received, exiting` + `[main] shutting down` | signalfd 路径 |

一键跑完 L2-2~L2-6（可贴，纯 ASCII）：

```bash
cd /root/c0
{ echo p; sleep 1; echo o; sleep 1; echo c; sleep 1; echo c; \
  sleep 1; echo h; sleep 5; echo p; echo h; sleep 2; echo p; echo x; } | ./core1_stub
```

**L2 通过 = L2-1~L2-10 全部符合期望。**

---

## L3 板端端到端验收（+ M4 + C8T6，G4 最终判定）

### 3.1 前置：M4 起来 + CAN 时钟点亮（按 G3 recipe）

```bash
sudo ./core0_service/tools/load_m4.sh status
sudo ./core0_service/tools/load_m4.sh start   # 内含 ensure_can_clock + bind_channel
ls -l /dev/ttyRPMSG0                          # 必须出现
dmesg | tail -20                              # 期望 creating channel rpmsg-tty addr 0x400
```

M4 起来后**不要频繁重启**（flaky 已知）；失败就等 2~5 分钟再 `start`。
C8T6 上电、CAN 两端 120Ω、共地；`can0` 保持 up（A7 静听不发）。

### 3.2 起服务

```bash
cd /root/c0 && ./core0_business -c core0.conf &     # 期望 [rpmsg] link UP
./core1_stub
```

> ⚠️ 本轮**不要同时跑 park_ui**：当前 `ipc_reader.cpp` 是只读且不写心跳，
> Core0 会判 Core1 失联（fault bit2）并跳过识别；真 UI 接入属第6步。

### 3.3 用例

| 编号 | 操作 | 期望日志序列 | 通过判据 |
|---|---|---|---|
| G4-A 车到位 | 手遮 C8T6 的 BH1750 ≥1s 后松手 | `[shade] car arrival observed (CAN 0x200 d0=0x01)` → `[state] IDLE -> CAR_WAIT (shade event 0x200 d0=0x01)` → `[state] CAR_WAIT -> RECOGNIZING (recog_pending=1, trigger 0x01 sent)`；3s 后 `[recog] recognition timeout: downgrade path (ui open stays available)` → `[state] RECOGNIZING -> DENY (recognition timeout)` → `[state] DENY -> COOL_DOWN (after deny)` | 全边有日志、3s±0.5s 降级（DENY 为瞬时态，紧接 COOL_DOWN 属正常） |
| G4-B 识别失效不阻塞开闸 | 紧跟 G4-A，stub 敲 `o` | `[remote] gate open request from core1 (ui)` → `[gate] OPEN 0x11 command sent (source=ui)` → **SG90 抬起**，C8T6 OLED 行3 `Gate:OPEN` | 开闸成功（KPI ≤500ms）；`p` 里 gate=OPEN |
| G4-C 白名单放行 | **先 `c` 关闸并等 M4 上报闸位闭合**，再手遮，`[state] RECOGNIZING` 时敲 `r TEST001 0.95` | `[ipc] recognition result: 'TEST001' conf=0.95 src=edge` → `[state] RECOGNIZING -> WHITELIST_CHECK ...` → `GATE_OPEN` → `[gate] OPEN 0x11 command sent (source=auto)` | SG90 动作、`p` 里 plate=TEST001 |
| G4-D 白名单拒绝 | 手遮后敲 `r TEST002 0.95` | `[whitelist] plate 'TEST002' rejected: entry not allowed` → `[state] ... -> DENY (entry not allowed)` | **不开闸**，DENY 留痕（`p` 里仍显示该车牌） |
| G4-E 过期拒绝 | 手遮后敲 `r TEST003 0.95` | `[whitelist] plate 'TEST003' rejected: whitelist entry expired` | 不开闸 |
| G4-F 云兜底延时 | 手遮后立刻敲 `k`（cloud_pending=1） | `[recog] cloud fallback pending: timeout extended to 6000ms`；3s 时**不**降级，6s 才降级 | 6s±0.5s 降级 |
| G4-G Core1 失联降级 | stub 敲 `h` 关心跳，等 3s，再手遮 | `[watchdog] core1 heartbeat lost >=3s: fault bit2 set` + `[biz] core1 offline at registration: skip recognition` + `[state] CAR_WAIT -> DENY (core1 lost, downgrade path)` | 车到位**直接**走降级，不空等 3s |
| G4-H 计数 | 出口模式：`core0.conf` 改 `count_mode = exit`（热加载），重复"开闸+遮光恢复"10 轮 | 每轮 `[slots] passage counted (exit mode): used=N free=M` | 10 轮值单调正确、无重复；`[slots] passage clamped at limit` 在 0 处出现 |
| G4-I 阈值一致性 | `core0.conf` 改 `conf_threshold = 0.80` 保存 | `[config] hot reload done: ok`；stub `p` 显示 `thr=0.80` | Core0 与读侧同值、无陈旧值 |

**KPI 量测**（用日志自带 ms 时间戳两行相减，spec 4.1）：
遮光→开闸 ≤3s（G4-C）；远程请求→开闸 ≤500ms（G4-B）；状态刷新 ≤500ms（`p` 观察）；配置热加载 ≤500ms（G4-I）。

---

## 4. 证据与记录

1. 全程日志：`./core0_business -c core0.conf 2>&1 | tee /root/c0/g4_run.log`，或 systemd 下 `journalctl -u park-core0 -b`。
2. 把 L2/L3 结果勾进 `PhaseMd/05_第4步_Linux本地业务.md` §3 G4 清单（`[x]` / 备注真机证据）。
3. 回填 `AGENTS.md`（板端事实、踩坑、遗留），**按提交纪律不要自动 git 提交**。

## 5. 常见判读

| 现象 | 含义 / 处理 |
|---|---|
| `[rpmsg] link DOWN` 一直不 UP | M4 未加载或 `/dev/ttyRPMSG0` 不存在 → 走 `load_m4.sh start`（先 `ensure_can_clock`） |
| `[gate] open command send failed (rpmsg down?)` | 同上；业务判定没问题，是链路层未起 |
| `[biz] core1 offline at registration: skip recognition` | 心跳没了：stub 敲了 `h`，或跑了只读的 park_ui |
| stub 报 `version mismatch: shm=.. expected=3` | 两边 `park_shm.h` 不同步 → 一起重编（当前契约 v3 / 76B） |
| `[config] cannot open core0.conf ... blank whitelist` | conf 路径不对：用 `-c` 指定绝对路径 |
| 计数不动 | 计数口径 = **开闸 + 遮光恢复**成对出现；只开闸不遮光 / 只遮光不开闸都不计数（S9 同口径） |
