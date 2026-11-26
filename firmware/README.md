# 加油计时器（ESP32-S3-Touch-LCD-4.3C）

赛车进站加油计时：**预设倒计时 → 传感器被压缩触发开始 → 最后 3 秒蜂鸣器满音量 + 红色闪烁 → 归零结束**。

屏幕 800×480，主数字用七段数码管控件拼出来（任意大小都锐利，不依赖字库），副信息有环形剩余进度、传感器实时状态、上一次成绩。

---

## 1. 硬件接线

### 1.1 传感器（接触式，NPN/PNP，5–24V DC）

传感器三根线（丝印：**蓝=0V、黑=输出、棕=+V**）。

板子这边用**板载隔离输入 DI0**（CH422G 的 EXIO0，出厂就是给外部开关量用的），这样感应头与主控之间有光耦隔离，赛车环境里抗干扰最好。

**方案 A：直接接隔离输入（推荐）**

端子排从下往上的顺序（丝印）：`… 5V | 5V | DO0 | DO1 | DI COM | GND | DI0 | DI1`

NPN 常开传感器三线接法（实测 DI COM 无需跳线，板上内部已与 GND 相通）：

| 传感器线 | 接到端子 |
| --- | --- |
| 棕（+V） | **5V**（传感器标 5–24V；若按压灯亮却不触发，改用 12V） |
| 蓝（0V） | **GND** |
| 黑（输出） | **DI1**（最下面那颗） |

> **本机实测记录**：这台板子的 **DI0 通道按压无响应**（毛病在 EXIO0/对应光耦那一侧），
> **DI1 通道正常**，所以固件默认只认 DI1（`fuel_timer.c` 里 `SENSOR_USE_DI1 1`、`SENSOR_USE_DI0 0`）。
> 黑线必须接 **DI1**。以后若修好 DI0，把 `SENSOR_USE_DI0` 改成 1，两路都能用（OR）。

**方案 B：加一级光耦/继电器（传感器与板子完全电气隔离）**

用 PC817 或 5V 继电器模块：传感器黑线驱动光耦/继电器线圈，**触点两端接 DI1 与 DI COM**。
NPN、PNP、机械微动开关都通用，也不怕感应头那边引入干扰。
**不建议**直接把传感器接到 GPIO：4.3C 的 GPIO 几乎全被 RGB 屏和数据线占用（真正空闲的只有 GPIO4，那是触摸屏中断脚）。

### 1.2 扬声器

板载 ES8311 + 功放，把扬声器接到板上的扬声器端子即可。蜂鸣用的是这条通路（2 kHz 方波、音量 100、功放使能 EXIO3），是这块板能出的最大音量。

> 想外挂一个独立蜂鸣器/警灯，可以用板载**隔离输出 DO0**：
> 在 `firmware/main/buzzer.c` 里把注释掉的 `IO_EXTENSION_Output(DO0, 1/0)` 两行放开即可（5–24V 的有源蜂鸣器直接挂 DO0 和 COM）。

### 1.3 供电 / 烧录

只用板载 USB-C（原生 USB-Serial/JTAG，免按键、免驱动，实测 COM8）。

---

## 2. 操作流程

1. 上电 → 大字显示当前预设（默认 **20.0** 秒），右上角 `SENSOR OPEN`；约 1.5 秒后有一声**开机自检音**。
2. 选预设，两种方式并存：
   - 点底部快捷芯片：**10 / 15 / 20 / 25 / 30 / 45**（改列表：`ui.c` 的 `k_presets_ms[]`）
   - 或按 **[−] / [+]** 自定义：单击 ±1s，**长按 ±5s**，范围 1–99s
3. 传感器被压缩 → **立即开始倒计时**，数字转青色，**精度 0.1 秒**，环形进度同步收缩。
4. **剩 5 秒时提前打开功放预热**（不出声，只为消除唤醒延迟）。
5. **剩 3 / 2 / 1 秒各响一声**（每声 250 ms、音量拉满），数字转红，标题旁 `BEEP` 字样变橙。**不做整屏闪烁**。
6. **倒计时越过 0 后继续往下数负数**（`-0.1 → -0.2 …`，左侧点亮负号，上限 -99.9），表示已超时。
7. **松开传感器 → 立即停表**，大数字与右上角 `LAST` 显示**本次真实总时长**（可能大于预设，例：`23.5s`）。
8. 结束后 **5 秒**自动回到待触发；任何时候点 `RE-ARM` 立即复位。

界面示意：

```
FUEL STOP          BEEP        ● SENSOR CLOSED     LAST 23.5s
PIT LANE COUNTDOWN
  ⁻                                         ╭───────╮
     ██  ██  .  █   s                       │  RUN  │
     ██  ██                                 ╰───────╯
 TARGET 20.0s                           - / + 1s (hold 5s)
 [10][15][20][25][30][45]  [−] [+]  [ RE-ARM ]
 TAP PRESET • CLOSE = START • RELEASE = STOP • BEEP 3/2/1s   DI 1/1 #12
```

> 底部 `DI 1/1 #12` 是调试信息：`DI0/DI1` 原始电平 + 电平跳变次数，用于现场验线，正式使用时可以删掉。
## 3. 编译与烧录

```powershell
# 激活 ESP-IDF 环境（本机 EIM 安装，v6.1）
& C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1

cd "C:\Users\ELEMENT\OneDrive\RACE\Cliamx Racing\Design\062 fuel time system\firmware"
idf.py -p COM8 flash monitor      # monitor 里按 Ctrl+] 退出
```

首次构建会从 components.espressif.com 拉取 `lvgl 9.2.0` 与 `esp_codec_dev ~1.3.1`（需要网络）。

---

## 4. 目录结构

```
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults           # 16MB flash / 8MB OPI PSRAM / LVGL 字体
├── components/                  # 复用 Waveshare 官方 BSP（已适配 IDF 6.1 组件名）
│   ├── i2c/                     # I2C 总线（SDA8/SCL9）
│   ├── io_extension/            # CH422G：背光/触摸复位/功放/隔离 IO
│   ├── rgb_lcd_port/            # 800x480 RGB 面板
│   ├── touch/                   # GT911
│   └── lvgl_port/               # LVGL 9 移植（双缓冲 + 防撕裂）
└── main/
    ├── main.c                   # 初始化顺序
    ├── ui.c / ui.h              # 界面
    ├── seg7.c / seg7.h          # 七段数码管控件
    ├── fuel_timer.c / fuel_timer.h  # 状态机 + 传感器去抖
    └── buzzer.c / buzzer.h      # ES8311 报警音
```

参考依据（已交叉核对）：

- 板载引脚/外设：仓库 `HARDWARE_REFERENCE_ZH.md` 与 `examples/esp-idf/*` 源码
- 原理图：`hardware/schematics/ESP32-S3-Touch-LCD-4.3C-Schematics.pdf`
- 隔离 IO 用法：`examples/esp-idf/04_isolation_io`
- LVGL 移植：`examples/esp-idf/12_lvgl_transplant`
- 音频：`examples/esp-idf/11_speaker_microphone`（ES8311 初始化参数）

---

## 5. 可调参数一览

| 想改什么 | 改哪里 |
| --- | --- |
| 预设档位列表 | `main/ui.c` → `k_presets_ms[]` |
| 默认预设 | `main/fuel_timer.c` → `PRESET_DEFAULT_MS` |
| 报警起始剩余时间（默认 3000 ms，即最后 3 秒） | `main/fuel_timer.c` → `WARN_MS` |
| 每声鸣叫长度（默认 250 ms） | `main/fuel_timer.c` → `BEEP_LEN_MS` |
| 功放预热提前量（默认 5000 ms） | `main/fuel_timer.c` → `AMP_WARMUP_MS` |
| 结束后自动复位延时（默认 5s） | `main/fuel_timer.c` → `AUTO_REARM_MS` |
| 传感器有效电平（NPN=0 / PNP=1） | `main/fuel_timer.c` → `SENSOR_ACTIVE_LEVEL` |
| 按下确认时长（默认 50 ms） | `main/fuel_timer.c` → `PRESS_CONFIRM_MS` |
| 松开确认时长（默认 150 ms） | `main/fuel_timer.c` → `RELEASE_CONFIRM_MS` |
| 结束后的回弹屏蔽窗口（默认 400 ms） | `main/fuel_timer.c` → `BOUNCE_WINDOW_MS` |
| 蜂鸣音调（默认 2000 Hz） | `main/buzzer.c` → `BZ_TONE_HZ` |
| 界面配色 | `main/ui.c` → `COL_*` |

---

## 6. 备注

- 传感器规格（±0.001 mm 重复精度、3 mm 行程、IP67、NPN/PNP 可选、≤10 mA）完全够用；注意它的**输出电流只有 10 mA**，如果改用继电器方案，请选线圈电流小的模块或加三极管/MOS 驱动。
- 蜂鸣无声时依次检查：扬声器是否接好 → EXIO3 功放使能（本工程自动打开）→ `buzzer_init()` 是否有 `codec open` 报错。
- 本工程按 IDF v6.1 编译通过；Waveshare 官方例程默认 V5.5.3，如果换回 5.5.x，需要把 `components/*/CMakeLists.txt` 里的 `esp_driver_gpio` 改回 `gpio`。
