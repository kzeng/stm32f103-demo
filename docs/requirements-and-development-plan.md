# STM32F103C8T6 Blue Pill 控制台固件需求与开发计划

状态：需求已确认 v0.3  
目标平台：STM32F103C8T6（Blue Pill）  
软件栈：STM32 HAL + FreeRTOS + USB CDC

当前实施状态：已创建 STM32CubeIDE/CubeMX 工程骨架，并完成 HAL、FreeRTOS、USB CDC Shell、GPIO、LED、ADC 单次读取和 Flash 配置保存；USB CDC 已完成枚举、可靠发送和重连处理；DAC/PWM 尚未实现。

## 1. 项目目标

制作一个可通过 USB Micro-B 连接电脑的交互式控制固件。用户通过串口 Shell 输入命令，完成 GPIO、ADC、LED 的配置和读写；相关配置可保存到片内 Flash。代码通过 ST-Link V2 使用 SWD 下载，并支持调试。

## 2. 硬件事实与边界

### 2.1 USB 与 ST-Link 的职责

- Blue Pill 的 Micro USB 直接连接 STM32F103 的 USB FS 外设：PA11 为 USB_DM，PA12 为 USB_DP。
- USB 端实现 USB CDC ACM，电脑侧表现为虚拟串口。
- 该 USB CDC 是 MCU 自己实现的 Shell 串口，不是 ST-Link VCP。普通 ST-Link V2 主要提供 SWD 下载/调试；是否带 VCP 取决于具体 ST-Link 型号和接线。
- USB、SWD 和用户 GPIO 的引脚必须避免冲突。调试阶段建议保留 SWDIO、SWCLK、NRST、3V3、GND 接口。

### 2.2 DAC 范围

STM32F103C8T6 没有内部 DAC 外设。本阶段明确不支持 DAC，也不实现 PWM 模拟输出。未来如有需要，再单独评估 PWM 或外接 I²C/SPI DAC。

### 2.3 建议的默认引脚规划

实际 CubeMX 配置前应以手上的 Blue Pill 板卡丝印和外接电路复核。

| 功能 | 引脚/资源 | 说明 |
|---|---|---|
| 用户 LED | PC13 | 多数 Blue Pill 为低电平点亮，需做板级极性配置 |
| USB FS | PA11/PA12 | USB_DM/USB_DP；需要 USB 连接电阻/晶振条件按板卡确认 |
| SWD | PA13/PA14 | SWDIO/SWCLK，下载调试使用 |
| ADC 示例 | PA0/PA1/PA2 等 | ADC1 通道，输入必须限制在 0–3.3 V |
| 预留 | 由后续需求决定 | 本阶段不分配 DAC/PWM 模拟输出引脚 |

## 3. 功能需求

### 3.1 启动与 Shell

- 上电后初始化 HAL、时钟、GPIO、ADC、USB CDC 和 FreeRTOS。
- USB CDC 连接后输出欢迎信息和提示符，例如 `stm32> `。
- 支持行编辑的最小集合：回车提交、退格删除、空格分隔参数。
- 支持 `help`、`status`、错误提示和命令返回码。
- 命令解析不应阻塞 USB 接收任务；接收、解析和硬件操作之间通过 FreeRTOS 队列或线程安全缓冲区传递。

### 3.2 GPIO

建议命令形式：

```text
gpio mode PA0 input|output|pullup|pulldown
gpio write PA0 0|1
gpio read PA0
```

约束：

- 只允许访问白名单 GPIO，禁止 Shell 任意修改 USB、SWD 和已占用外设引脚。
- 对不存在或格式错误的端口/引脚返回明确错误。
- 输出模式、速度、上下拉配置要与 HAL GPIO 配置一致。

### 3.3 ADC

建议命令形式：

```text
adc read PA0
adc read PA0 raw
adc config resolution 12
```

- 第一阶段支持单次转换并返回原始值；可选返回按 3.3 V 参考电压换算的毫伏值。
- 需说明 STM32F1 ADC 的参考电压、采样时间和输入阻抗限制。
- ADC 输入严禁超过 VDDA 或低于 VSSA。

### 3.4 LED

建议命令形式：

```text
led on
led off
led toggle
led status
```

- LED 命令内部复用 GPIO 层。
- 用板级配置表达 LED 是否低电平点亮，避免把极性硬编码在 Shell 层。

### 3.5 DAC

本阶段不提供 DAC、PWM 模拟输出或相关 Shell 命令。

### 3.6 配置保存

建议命令形式：

```text
config save
config load
config reset
```

- 保存 GPIO 模式、输出状态以及后续纳入配置模型的参数。
- 使用独立 Flash 配置区域，不得覆盖程序代码区域。
- 配置结构应包含 magic、版本号、长度和校验值，启动时校验失败则使用默认配置。
- F1 Flash 擦除以页为单位；第一版采用显式保存策略，避免每次 GPIO 操作都擦写 Flash。

## 4. 软件架构建议

```text
USB CDC 接收/发送
        |
  shell_rx_queue
        |
ShellTask —— 命令解析/权限与引脚白名单
        |
硬件服务层：GPIO / ADC / LED / Config Flash
        |
STM32 HAL + FreeRTOS
```

建议任务：

- `UsbTask`：处理 USB CDC 状态、接收数据和发送缓冲；避免在中断中做字符串解析。
- `ShellTask`：组装命令行、解析参数、调用服务层并输出结果。
- `MonitorTask`：可选，仅用于心跳或状态统计；本阶段不做周期性 ADC 采样和数据导出。

第一版不建议为每个外设创建独立任务；GPIO/ADC/Flash 配置操作短小且可序列化时，由 ShellTask 调用服务层即可。Flash 擦写期间应避免并发访问配置数据。

## 5. 工具链与工程方式

推荐初始流程：

1. 使用 STM32CubeIDE 内置的 CubeMX 工程向导创建 STM32F103C8Tx 工程。
2. 选择 STM32 HAL，启用 FreeRTOS（CMSIS-RTOS2 或原生 FreeRTOS API，二选一并保持一致）。
3. 启用 USB Device FS + CDC。
4. 配置 HSE/时钟，使 USB 时钟满足 48 MHz 要求；Blue Pill 晶振质量和实际频率需要验证。
5. 使用 STM32CubeIDE 构建、下载和调试；后续可迁移到 CMake/Make + arm-none-eabi 工具链。
6. 使用 ST-Link V2 通过 SWD 下载，不把 USB CDC 当作固件下载通道。

## 6. 分阶段开发步骤

### 阶段 0：硬件确认

- 确认芯片型号、晶振、USB 连接、LED 极性和板卡供电方式。
- 用万用表确认 3.3 V 电平，所有外部输入输出均不得超出芯片绝对最大额定值。
- 预留 ST-Link SWD 接口，确认 BOOT0 可恢复启动。

### 阶段 1：最小 HAL 工程

- 创建工程并用 ST-Link 下载一个 LED 闪烁程序。
- 验证 SWD 调试、复位、时钟和基础 GPIO。
- 固定工程目录、生成代码策略和版本控制规则。

### 阶段 2：FreeRTOS

- 添加任务、堆、栈和系统时基配置。
- 创建 ShellTask 和基础队列/互斥锁。
- 用 LED 心跳验证调度器运行。
- 记录最小剩余栈空间，避免 USB 和命令解析导致栈溢出。

### 阶段 3：USB CDC

- 配置 USB Device FS CDC。
- 在电脑上确认枚举和虚拟串口设备出现。
- 实现可靠的接收环形缓冲区、发送互斥和断开重连处理。
- 输出欢迎信息与 `help`，验证长时间空闲和反复插拔。

### 阶段 4：Shell 与 GPIO/LED

- 实现命令表、参数解析、错误码和帮助文本。
- 先实现 `help`、`status`、`led`，再实现 GPIO 白名单和读写。
- 验证非法端口、冲突引脚和边界参数。

### 阶段 5：ADC

- 配置 ADC 采样时间和单次转换。
- 实现 `adc read`，用已知电压验证原始值和毫伏换算。
- 增加输入保护和超范围提示。

### 阶段 6：配置持久化

- 根据最终 Flash 容量和链接脚本确定配置页地址。
- 定义带版本、长度和 CRC/校验和的配置结构。
- 实现 `config save`、`config load`、`config reset`。
- 测试掉电、复位、非法配置和版本升级场景。

### 阶段 7：集成测试与文档冻结

- 测试 USB 插拔、复位、看门狗/异常恢复、连续命令和错误输入。
- 测量 RAM/Flash 使用量、任务栈余量和 USB 长时间运行稳定性。
- 记录最终引脚表、命令表、编译下载步骤和已知限制。

## 7. 初步验收标准

- 能通过 ST-Link V2 稳定下载和单步调试。
- 电脑能识别 USB CDC，终端能看到提示符并连续执行命令。
- `led`、`gpio`、`adc`、`config` 命令在正常输入和错误输入下均有可理解的返回。
- USB 重新插拔或 MCU 复位后能恢复 Shell，不需要重新烧录。
- 不允许 Shell 改写 USB、SWD 或其他受保护引脚。
- 文档明确说明本阶段不支持 DAC；ADC、GPIO 和 LED 配置可保存到 Flash。

## 8. 待确认问题

1. Shell 确定通过 Blue Pill Micro USB 提供的 USB CDC 虚拟串口，不使用 USART1/USART2。
2. Flash 配置第一版采用单配置页、显式保存；双页磨损均衡和更强掉电保护暂不纳入。
3. 暂不需要命令权限、密码或生产模式锁定。

## 9. 已完成的现场验证记录

截至当前版本，已在连接的 Blue Pill 和 ST-Link V2 上完成以下验证：

- ST-Link V2 可稳定下载，BIN 使用 `0x08000000` 地址烧录并通过 OpenOCD 校验。
- FreeRTOS 调度器正常运行，PC13 板载 LED 按约 500 ms 周期闪烁。
- 修正 STM32F1 的中断优先级位数配置：`configPRIO_BITS=3`，避免 FreeRTOS 启动断言阻塞。
- USB 时钟为 48 MHz，USB FS 使用 PA11/PA12；主机成功枚举 `0483:5740`，创建 `/dev/ttyACM0`。
- Shell 已验证 `help`、`status`、`gpio mode`、`gpio write`、`gpio read` 和 `config save`；连续命令回显无丢字节。
- USB 启动前会短暂拉低 PA12，强制产生 detach/reattach，解决 ST-Link 下载或 MCU 复位后主机不重新枚举的问题。
- USB CDC 发送采用独立 64 字节缓冲和 `USBD_BUSY` 重试；不得直接把短生命周期的栈变量交给异步 USB 发送。
- Shell 支持 `disconnect`、`exit` 和 ANSI Up/Down 历史命令；USB CDC ACM 主机打开端口或重新枚举后会重新输出欢迎语和 `stm32> ` 提示符。

若更换板卡后仍不能枚举，优先检查 Micro USB 数据线、D+/D− 走线、PA11/PA12 连接以及 Blue Pill 上的 D+ 1.5 kΩ 上拉电阻；不需要 CH340 芯片。

完整的 LED、FreeRTOS、USB CDC、Shell 调试根因分析和经验总结见：[调试复盘](debugging-retrospective.md)。
