# OLED 驱动规范 — CAN Bus Edge Collector

> 本规范适用于 **I2C 接口 OLED** (SSD1306, 0.96 寸, 128x64)，本项目未使用 SPI 接口 OLED。
>
> 最后更新: 2026-09-06（本次修正：显示行高表述、硬件 I2C 黑屏根因、乱码成因，详见下文）

---

## 1. 硬件接口

| 项目 | 值 |
|------|-----|
| 模块 | 0.96 寸 SSD1306 OLED, I2C 接口 |
| I2C 地址 | 0x78 (7-bit: 0x3C, 写) |
| SCL | PB8 |
| SDA | PB9 |

---

## 2. GPIO 配置 (已验证)

### 2.1 引脚模式

```c
GPIO_InitTypeDef gpio;
gpio.GPIO_Pin   = GPIO_Pin_8 | GPIO_Pin_9;
gpio.GPIO_Mode  = GPIO_Mode_Out_OD;   // 开漏输出，必须
gpio.GPIO_Speed = GPIO_Speed_50MHz;
GPIO_Init(GPIOB, &gpio);
SCL_H; SDA_H;  // 上拉至高电平（空闲状态）
```

**必须使用开漏 (Open-Drain) 模式**。推挽模式 (GPIO_Mode_Out_PP) 会在 ACK 位形成主从短路电流，烧毁 IO 口或 OLED 模块。此问题已验证——推挽模式下 SDA 主从同时驱动相反电平。

I2C 的高电平由模块自带的 4.7kΩ~10kΩ 上拉电阻实现，MCU 只主动拉低。

### 2.2 硬件 I2C 外设 (AF_OD) 未通过验证

尝试使用 STM32F103 的硬件 I2C1 外设 (AF_OD 模式, 100kHz) 替代软件 GPIO 翻转，未能在本项目中点亮 OLED。

**黑屏根因（2026-09-06 已定位，原“可能是初始化时序问题”不准确）**：老版驱动 `SSD1306_Init` 的初始化命令流**只发命令字节、从不发参数字节**（如 `0x8D`(电荷泵) 后未跟 `0x14`），导致命令错位解码、内部升压被关 → 面板无 VPP 全黑，而 I2C 地址应答完全正常。该 bug 与传输方式无关（HAL I2C 与软件 I2C 都会黑，Keil 参考工程正常即因它逐字节带参数）。修正为逐字节独立发送参数齐全的平铺序列后，软件 I2C 点亮成功。当前按软件 I2C 方案使用；硬件 I2C 在修正 init 后未复测。

---

## 3. I2C 时序 (已验证)

### 3.1 延时配置

```c
static void I2C_Delay(void)
{
    volatile uint32_t i;
    for (i = 0; i < 40; i++) __NOP();
}
```

| I2C_Delay 值 | SCL 频率 | 全屏刷新时间 | 可靠性 |
|-------------|---------|-------------|--------|
| 40 NOP | ~300kHz | ~300ms | 默认, 但 EMI 环境下不稳 |
| 100 NOP | ~80kHz | ~900ms | 抗噪较好, 刷新变慢 |
| 200 NOP | ~35kHz | ~2.5s | 抗噪最好, 极慢 |

**验证**: 40 NOP 在无 EMI 干扰时稳定工作。当 CAN 总线有活动时，PA11 的电磁辐射会耦合到 PB8/PB9 线路上，导致 I2C 数据错误。

### 3.2 软件 I2C 位敲击实现

每个 I2C 字节传输: Start + 8 数据位 + 1 ACK 时钟 + Stop

```c
static void I2C_Start(void)
{
    SDA_H; I2C_Delay(); SCL_H; I2C_Delay();
    SDA_L; I2C_Delay(); SCL_L; I2C_Delay();
}

static void I2C_Stop(void)
{
    SDA_L; I2C_Delay(); SCL_H; I2C_Delay();
    SDA_H; I2C_Delay();
}

static void I2C_SendByte(uint8_t byte)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        if (byte & 0x80) SDA_H; else SDA_L;
        I2C_Delay(); SCL_H; I2C_Delay(); SCL_L; I2C_Delay();
        byte <<= 1;
    }
    SCL_H; I2C_Delay(); SCL_L; I2C_Delay();  // ACK 时钟 (不检测应答)
}
```

**注意**: 本实现不检测从机 ACK 信号。若 OLED 无响应，写操作静默失败。

---

## 4. OLED 命令和数据 (已验证)

### 4.1 写命令

```c
void OLED_WriteCommand(uint8_t Command)
{
    I2C_Start();
    I2C_SendByte(0x78);   // 地址 + 写
    I2C_SendByte(0x00);   // 命令标识
    I2C_SendByte(Command);
    I2C_Stop();
}
```

### 4.2 写数据

```c
void OLED_WriteData(uint8_t Data)
{
    I2C_Start();
    I2C_SendByte(0x78);   // 地址 + 写
    I2C_SendByte(0x40);   // 数据标识
    I2C_SendByte(Data);
    I2C_Stop();
}
```

### 4.3 光标位置

SSD1306 页地址模式: 128x64 = 8 页 x 128 列, 每页 8 像素高

```c
void OLED_SetCursor(uint8_t Y, uint8_t X)
{
    OLED_WriteCommand(0xB0 | Y);                         // 页 (0-7)
    OLED_WriteCommand(0x10 | ((X & 0xF0) >> 4));         // 列高 4 位
    OLED_WriteCommand(0x00 | (X & 0x0F));                // 列低 4 位
}
```

### 4.4 字符显示

每个 8x16 字符使用 2 页 (上半 + 下半各 8 字节字形数据)：

```c
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)
{
    OLED_SetCursor((Line-1)*2, (Column-1)*8);
    for (i = 0; i < 8; i++)
        OLED_WriteData(OLED_F8x16[Char - ' '][i]);       // 上半
    OLED_SetCursor((Line-1)*2+1, (Column-1)*8);
    for (i = 0; i < 8; i++)
        OLED_WriteData(OLED_F8x16[Char - ' '][i+8]);     // 下半
}
```

显示区域: 4 行 x 16 字符（8x16 字体：**每行 16 像素高 = 2 页**，4 行共 64 行像素；每字符 8 像素宽，16 字符 = 128 像素宽）。

### 4.5 清屏

```c
void OLED_Clear(void)
{
    for (j = 0; j < 8; j++) {
        OLED_SetCursor(j, 0);
        for (i = 0; i < 128; i++)
            OLED_WriteData(0x00);
    }
}
```

---

## 5. 初始化序列 (已验证)

```c
void OLED_Init(void)
{
    // 上电延时
    for (i = 0; i < 1000; i++) for (j = 0; j < 1000; j++);

    OLED_I2C_Init();

    OLED_WriteCommand(0xAE);  // 关闭显示
    OLED_WriteCommand(0xD5); OLED_WriteCommand(0x80);
    OLED_WriteCommand(0xA8); OLED_WriteCommand(0x3F);
    OLED_WriteCommand(0xD3); OLED_WriteCommand(0x00);
    OLED_WriteCommand(0x40);
    OLED_WriteCommand(0xA1);
    OLED_WriteCommand(0xC8);
    OLED_WriteCommand(0xDA); OLED_WriteCommand(0x12);
    OLED_WriteCommand(0x81); OLED_WriteCommand(0xCF);
    OLED_WriteCommand(0xD9); OLED_WriteCommand(0xF1);
    OLED_WriteCommand(0xDB); OLED_WriteCommand(0x30);
    OLED_WriteCommand(0xA4);
    OLED_WriteCommand(0xA6);
    OLED_WriteCommand(0x8D); OLED_WriteCommand(0x14);
    OLED_WriteCommand(0xAF);  // 开启显示

    OLED_Clear();
}
```

---

## 6. 已知问题 (已验证)

### 6.1 EMI 干扰

OLED 的 I2C 线 (PB8/PB9) 受 CAN RX (PA11) 高速翻转时的电磁辐射干扰。表现:
- CAN_H/L 正常连接: OLED 显示错乱 (新旧内容叠加)，Housekeep 刷新卡住
- CAN_H/L 断开或反接: OLED 正常
- PA11 与收发器断开: OLED 正常

**结论（2026-09-06 修正）**: CAN 活动时的高速信号（PA11）EMI 耦合是**硬件层面**的干扰源，确实需要物理隔离；但后续实测发现**另一软件成因同样会造成“渐进乱码 + 碎像素落到第 4 行”**：软件 I2C 长事务（整页 128B bit-bang，~1.7ms@300kHz）在 FreeRTOS 下执行时被任务切换/中断抢占，若落在 Stop/ACK 危险窗口，从机会丢字节、页指针漂移。**该成因可由软件修复**——`ssd1306.c` 的 `SSD1306_WriteCmd/SSD1306_WriteData` 已用 `__disable_irq()/__enable_irq()` 临界区保证单条事务原子（单事务 <2ms，页间仍可调度）。加上临界区后若仍乱码，再按 EMI 做物理隔离/降速。

### 6.2 sprintf 字符串长度限制

`g_oled_line[4][17]` 每个缓冲区 16 字符 + 1 个 null。sprintf 输出超过 16 字符会溢出到相邻内存。已验证溢出会导致相邻变量 (如 g_eth_was_connected) 被破坏。必须确保所有格式化字符串 <= 16 字符。

### 6.3 任务栈占用

在 FreeRTOS Housekeep 任务中调用 OLED 刷新 (含 sprintf + I2C 操作)，任务栈需 >=384 words。192 words 已验证会爆栈。
