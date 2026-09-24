# STM32F103C8T6 Blue Pill LED 与 USB CDC 调试复盘

日期：2026-09-25  
平台：STM32F103C8T6 Blue Pill  
软件栈：STM32 HAL、FreeRTOS、USB Device CDC、Shell  
下载工具：ST-Link V2 + OpenOCD

## 1. 最终结论

本次问题由两条独立链路组成：

1. LED 不闪烁的根因是 FreeRTOS Cortex-M NVIC 优先级位数配置错误，调度器在启动断言中停止，LED 任务没有运行。
2. USB 串口问题由 USB 重新枚举、CDC 异步发送缓冲区生命周期、`USBD_BUSY` 未处理以及欢迎信息发送时机不正确等问题共同造成。

最终状态：

- PC13 LED 正常闪烁。
- FreeRTOS 调度器和任务正常运行。
- Blue Pill Micro USB 成功枚举为 `0483:5740`。
- 主机创建 `/dev/ttyACM0`。
- Shell、GPIO、ADC、Flash 配置保存功能正常。
- 支持 `disconnect`、`exit` 和 ANSI Up/Down 命令历史。
- USB 重新连接或终端打开后可以看到欢迎语和 `stm32> ` 提示符。

## 2. LED 问题

### 2.1 硬件事实

Blue Pill 的板载 LED 使用 PC13，常见板卡为低电平点亮：

```text
PC13 = 0：LED 亮
PC13 = 1：LED 灭
```

通过直接寄存器控制验证了 ST-Link、MCU、PC13 和 LED 硬件均正常。因此 LED 不闪烁不是 LED 损坏，也不是下载失败。

### 2.2 根因

原始 FreeRTOS 配置使用了：

```c
#define configPRIO_BITS 4
```

但实际连接的 STM32F103 芯片只实现了 3 个 NVIC 优先级位。运行时通过写入并读回 NVIC 优先级寄存器验证：

```text
写入：0xff
读回：0xe0
```

这说明实际优先级位数为 3 位。

FreeRTOS 启动时进入 `configASSERT()` 死循环，表现为：

- `main()` 可以运行。
- GPIO 初始化可以执行。
- `osKernelStart()` 之后任务不运行。
- LED 任务不闪烁。
- USB Shell 任务也无法正常工作。

### 2.3 修复

在 `Core/Inc/FreeRTOSConfig.h` 中修正为：

```c
#define configPRIO_BITS 3
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 4
```

修复后，FreeRTOS 调度器正常启动，PC13 LED 按约 500 ms 周期闪烁。OpenOCD 读取 GPIOC ODR 可以观察到 `0x0000` 与 `0x2000` 之间周期变化。

## 3. USB CDC 串口问题

### 3.1 Blue Pill 不需要 CH340

Blue Pill 的 Micro USB 直接连接 STM32 USB FS：

```text
PA11：USB_DM
PA12：USB_DP
```

当前串口是 STM32 固件实现的 USB CDC ACM 虚拟串口，不是 USART，也不是 ST-Link VCP。因此不需要 CH340。

### 3.2 USB 初始化和主机枚举是不同阶段

早期检查发现 USB PCD、USB 时钟、中断和 USB 控制器均已启动，但主机没有创建 `/dev/ttyACM0`。这说明：

```text
USB 软件初始化成功
≠
主机已经完成 USB 枚举
```

还需要分别验证 USB 48 MHz 时钟、D+ 上拉、主机 reset、USB configured 状态和终端打开状态。

### 3.3 USB 重枚举问题

Blue Pill 板上的 D+ 上拉可能在 MCU 复位或 ST-Link 下载期间保持有效。主机可能在 USB 固件尚未准备好时就开始枚举，随后不再自动重试。

解决方法是在 USB 初始化前：

1. 将 PA12 临时配置为推挽输出。
2. 将 PA12 拉低约 20 ms。
3. 释放 PA12。
4. 再初始化 USB Device CDC。

这样可以人为产生一次 USB detach/reattach。实现位于：

```text
USB_DEVICE/App/usb_device.c
```

修复后主机成功识别：

```text
0483:5740 STMicroelectronics Virtual COM Port
/dev/ttyACM0
```

### 3.4 烧录地址问题

BIN 文件烧录时必须明确指定 Flash 地址。错误命令：

```text
program build/stm32f103-console.bin verify reset exit
```

正确命令：

```text
program build/stm32f103-console.bin 0x08000000 verify reset exit
```

未指定地址导致过一次校验失败假象，实际是 OpenOCD 将 BIN 解释到了错误地址。

### 3.5 CDC 发送丢字节问题

USB 枚举成功后，曾经出现：

```text
gpio read PC13
```

被回显成类似：

```text
goeaP3
```

根因是两个异步发送错误：

1. 逐字符发送时忽略了 `USBD_BUSY`。
2. 直接把 Shell 任务中的局部变量地址交给异步 USB 发送。

例如：

```c
uint8_t ch;
CDC_Transmit_FS(&ch, 1);
```

函数返回不代表 USB 传输已经完成；下一次循环可能已经覆盖了 `ch`。

修复方法：

- 增加独立的 64 字节 CDC 发送缓冲区。
- 发送前复制数据。
- 按 64 字节分包。
- 处理 `USBD_BUSY` 并重试。
- Shell 所有输出统一经过可靠发送函数。

修复后连续 GPIO、状态和配置命令均能正确往返。

### 3.6 欢迎语发送时机问题

USB 初始化完成后立即发送欢迎语，主机可能尚未进入 `USBD_STATE_CONFIGURED`，或者终端程序尚未打开 `/dev/ttyACM0`，因此欢迎语会丢失。

现在的处理方式是：

- 等待 USB 进入 configured 状态。
- 监听 CDC 控制线状态，识别终端打开动作。
- 主机重新枚举或打开端口时重新输出欢迎语。

输出内容为：

```text
STM32F103C8T6 console
Type 'help' for commands.
stm32>
```

## 4. Shell 功能实现

### 4.1 `disconnect`

`disconnect` 命令执行：

1. 发送确认信息。
2. 停止 USB CDC 和 PCD。
3. 拉低 D+。
4. 延时后重新初始化 USB。
5. 等待主机重新枚举。
6. 重新发送欢迎语和提示符。

该命令是一次 USB detach/reattach，不会重启 MCU。

### 4.2 `exit`

`exit` 关闭当前 Shell 会话，但不停止 MCU 和 USB。下一次收到输入时重新显示提示符并恢复 Shell。

### 4.3 命令历史

Shell 保存最近 8 条非空命令，支持标准 ANSI 转义序列：

```text
ESC [ A：上一条命令
ESC [ B：下一条命令
```

历史编辑会同步更新当前输入行，支持退格和再次执行。

## 5. 为什么调试过程走了弯路

### 5.1 没有一开始严格分层

早期同时混合了 GPIO、FreeRTOS、USB、Shell、ADC、Flash 和时钟配置，导致单个现象可能对应多个候选原因。

更合理的顺序应该是：

```text
直接寄存器 GPIO
    ↓
HAL GPIO
    ↓
HAL + 时钟
    ↓
FreeRTOS 单任务
    ↓
USB CDC 枚举
    ↓
CDC 收发
    ↓
Shell
    ↓
GPIO/ADC/Flash 命令
```

### 5.2 对 NVIC 优先级位数做了想当然的假设

不能只依赖 CMSIS 头文件中的默认值。实际芯片、兼容芯片、CMSIS 版本和 HAL 配置可能存在差异，必须用运行时寄存器读回值验证实际实现位数。

### 5.3 没有区分 USB 初始化和 USB 枚举

`USBD_Init()` 成功只说明固件栈初始化成功，不代表主机已经枚举成功。USB 调试必须同时检查：

- USB 48 MHz 时钟。
- PA11/PA12 连接。
- D+ 上拉。
- USB reset 和 configured 状态。
- 主机 `lsusb` 结果。
- `/dev/ttyACM*` 是否出现。
- 终端是否真正打开设备。

### 5.4 没有尽早处理异步通信的缓冲区所有权

异步 USB 发送必须明确：谁拥有缓冲区、缓冲区何时可以复用、发送忙时如何排队。不能把栈变量直接传给异步外设。

### 5.5 主机重枚举和旧终端状态制造了额外假象

USB 重新枚举后，旧的 `/dev/ttyACM0` 文件描述符和终端参数可能已经失效。测试 `disconnect` 后应重新打开设备，并设置：

```bash
stty -F /dev/ttyACM0 115200 raw -echo -ixon -ixoff -crtscts
```

否则主机端旧数据、终端状态或旧句柄可能表现为乱码和伪命令，容易误判为 MCU Shell 再次损坏。

## 6. 经验教训

1. 先验证硬件，再逐层加入软件复杂度。
2. 使用寄存器、调试变量和 OpenOCD 读取结果证明任务是否真正运行。
3. 运行时寄存器值优先于配置文件和默认宏的假设。
4. USB 软件初始化、主机枚举、CDC configured 和终端打开必须分别验证。
5. 所有异步发送都必须保证缓冲区生命周期，并正确处理 BUSY 状态。
6. 每次 OpenOCD halt/read 后都要确保目标最终 resume。
7. BIN 文件烧录必须显式指定 `0x08000000`。
8. 测试结束后恢复 Flash 配置，避免测试 GPIO 状态污染正式配置。
9. 调试代码应尽量简单、可观察，验证完成后再清理或保留为低成本诊断能力。

## 7. 最终验证记录

已完成以下验证：

- OpenOCD 烧录并校验通过。
- PC13 LED 周期闪烁。
- USB 枚举为 `0483:5740`。
- `/dev/ttyACM0` 正常出现。
- 欢迎语和 `stm32> ` 正常显示。
- `help`、`status` 正常。
- GPIO mode/write/read 正常。
- `config save` 正常。
- `exit` 会话关闭和恢复正常。
- Up 历史命令恢复并重新执行正常。
- `disconnect` detach/reattach 和重新枚举正常。

当前明确不支持 DAC 和 PWM 模拟输出。
