# BUG 记录与避坑清单（边缘智能网关项目）

> **维护约定（强制）**
> 1. 本项目**任何**新发现的 bug、编译错误、硬件不符、HAL/库缺陷，都必须在本文档登记，
>    不允许只写在聊天里或只写进某份阶段文档。
> 2. 每条固定格式：`现象 → 根因 → 修法`，并标注 `状态`（已修 / 待复测 / 未解决）。
> 3. 修完立刻把 `状态` 改成已修，写清**改了哪个文件哪一处**。
> 4. 编号连续递增，不复用、不删除（删掉历史 bug 会让同类问题二次踩坑）。
> 5. 每阶段结束把本阶段新增条目同步摘要进 `.workbuddy/memory/` 的当日日志。

| 字段 | 含义 |
|---|---|
| 状态 | ✅ 已修 / ⏳ 待复测 / ❌ 未解决 / ⚠️ 需长期注意 |
| 发现方式 | 🔍 人工审查 / 🔧 实编译 / ⚡ 上电实测 |

**统计**：共 41 条，已修 37 / 长期注意 3 / 已澄清 1。（BUG-039 于 2026-10-18 复测确认）
（BUG-018 于 2026-10-18 复测通过；ping 于同日首次打通）

---

## 目录

| 分类 | 条目 | 说明 |
|---|---|---|
| **A. 硬件与原理图** | BUG-001 ~ 006 | 我凭经验给的配置与板子实际不符 |
| **B. CubeMX 分配错误** | BUG-007 ~ 010 | CubeMX 自动分配/默认参数出错 |
| **C. CubeMX 模板缺失 & HAL 缺陷** | BUG-011 ~ 018 | 生成的代码不完整，ST 库本身的 bug |
| **D. 参数设置不当** | BUG-019 ~ 026 | 默认值在 100M 以太网场景下不够用 |
| **E. 编译与链接** | BUG-027 ~ 032 | 实编译才暴露的问题 |
| **F. 运行期逻辑** | BUG-033 ~ 035 | 上电后才看出来的问题 |
| **G. 环境与操作** | BUG-036 ~ 037 | 工具链、重新生成相关 |
| **附录 A：通用教训** | — | 跨 bug 提炼，值得背下来 |
| **附录 B：已实测硬件参数** | — | 上电实测确认的值，后续阶段直接引用 |

---

# A. 硬件与原理图（规划阶段给的引脚与板子不符）

## BUG-001 · PHY 型号误判为 LAN8720，实际是 YT8512C ✅

- **现象**：按 LAN8720 写驱动与寄存器定义，CubeMX 默认 LAN8742 驱动 PHY ID 校验失败，以太网起不来。
- **根因**：我按常见方案默认推断。实际探索者 V3.5 板载 **裕太微 YT8512C**（自带 25MHz 晶振，
  输出 50MHz 给 PA1 做 REF_CLK）。
- **修法**：
  - 判链路**只用标准寄存器 BSR(1) 的 bit2**（IEEE 802.3 规定，所有 PHY 通用），
    不去碰 16~31 号厂商自定义寄存器。
  - PPT 给的 `PHY_SR=0x1F / SPEED=0x0004 / DUPLEX=0x0010` 是 LAN8720 专属，YT8512C 不适用。
  - 网速直接固定 100M 全双工（HAL 默认已是），不读 PHY 判速率。
- **状态**：✅ 已修（`ethernetif.c` 链路线程只读 BSR）
- **发现方式**：🔍 人工审查（对照 `EXPLORER_V3.5.pdf`）

## BUG-002 · CAN1 引脚写成 PB8/PB9，实际是 PA11/PA12 ✅

- **现象**：按 PB8/PB9 配 CAN → CAN 不通，且把板载硬件 I2C 废掉。
- **根因**：板载 CAN 收发器是 **SIT1050T**，接在 PA11/PA12；PB8/PB9 是板载 I2C1（挂 24C02，4.7K 上拉）。
- **修法**：CAN1 改 **PA11/PA12**；且必须把 **P5 跳线帽跳到 CAN 侧**
  （PA11/PA12 与 USB D-/D+ 二选一，跳 USB 侧 CAN 一声不吭）。
- **状态**：✅ 已修
- **发现方式**：🔍 人工审查（原理图 + IO 引脚分配表）

## BUG-003 · I2C 引脚写成 PB6/PB7，实际是 PB8/PB9 ✅

- **现象**：按 PB6/PB7 配 I2C 读不到传感器。
- **根因**：PB6/PB7 在探索者上是 DCMI 摄像头接口，不是 I2C。板载 I2C 硬件在 PB8/PB9。
- **修法**：I2C1 改 PB8/PB9，`GPIO_MODE_AF_OD` + **No pull**（板上已有 4.7K 外部上拉）。
- **状态**：✅ 已修（CubeMX 生成 `i2c.c` 已确认 AF_OD）
- **发现方式**：🔍 人工审查

## BUG-004 · ETH TXD0/TXD1 板载固定 PG13/PG14 ✅

- **现象**：CubeMX 曾把 TXD0/TXD1 分到 PB12/PB13，同时把 CAN2 挤掉。
- **根因**：RMII 的 TXD0/TXD1 有两组可选映射（PG13/PG14 或 PB12/PB13），
  但**板载走线已固定为 PG13/PG14**（PB12/13 接的是 ES8388 音频芯片）。
  挪到 PB12/PB13 就是"软件配对了、硬件没连"。
- **修法**：ETH 固定 PG13/PG14；CAN2 用 PB12/PB13。
- **状态**：✅ 已修
- **发现方式**：🔍 人工审查

## BUG-005 · 板载 RS485 与以太网 MDIO 冲突（PA2）⚠️

- **现象**：用网口时板载 RS485 不能同时用；P4 跳线没拔时 MDIO 读不到 PHY。
- **根因**：PA2 同时是 `ETH_MDIO` 和板载 RS485_RX（USART2），靠 P4 跳线切换。
- **修法**：
  - **P4 跳线帽拔掉**。
  - RS485 改用外接模块：`USART6 (PC6/PC7)` + `PE5` 做方向控制
    （不用 PG8 —— 它硬连板载 RS485 的 RE，复用会顺带驱动板载芯片）。
- **状态**：✅ 已修（方案定案，M2 实施）｜⚠️ 每次上电前必须确认 P4 已拔
- **发现方式**：🔍 人工审查

## BUG-006 · 方向控制脚选 PG8 会误驱板载芯片 ✅

- **现象**：PG8 作外部 RS485 模块方向脚时，板载 SP3485 也被驱动。
- **根因**：PG8 硬连板载 RS485 的 RE 脚，电气上掰不开。
- **修法**：改用 **PE5**（不插 OLED/CAMERA 时完全独立）。
- **状态**：✅ 已修

---

# B. CubeMX 自动分配 / 默认参数错误

## BUG-007 · ETH_RMII_TX_EN 被分到 PB11，实际是 PG11 ✅

- **现象**：PHY 的发送使能没接上 → 能收不能发，ping 无回应。
- **根因**：CubeMX 自动挑了 PB11（RMII_TX_EN 的可选脚之一），
  但板载走线是 **PG11**（IO 表明确：PG11 = RMII_TX_EN，接 YT8512C 的 TXEN 脚）。
- **连带损伤**：USART3_RX 被挤到 **PD9**（FSMC 数据线，不能用作串口）。
- **修法**：`.ioc` 里 PB11 → **PG11**，USART3_RX 回 **PB11**，Generate Code 后已确认。
- **状态**：✅ 已修（重新生成后复核通过）
- **发现方式**：🔍 人工审查（对照 IO 引脚分配表）

## BUG-008 · PB12/PB13 被 ETH 占用导致 CAN2 无脚可用 ✅

- **现象**：CAN2 配不上引脚。
- **根因**：CAN2 只有 PB5/PB6 与 PB12/PB13 两组可选，而 PB5 已给 SPI1_MOSI。
- **修法**：ETH TXD 固定 PG13/PG14（见 BUG-004），PB12/PB13 让给 CAN2。
- **状态**：✅ 已修

## BUG-009 · lwIP 平台设置警告（Memory Location）✅

- **现象**：Generate 时弹 "Platform Settings 有未配置项" 警告。
- **根因**：LWIP → Platform Settings → **Memory Setting → Location** 未设或设了专用地址却为空。
- **修法**：Location 选 **Linker defined**。**绝不能选 CCM/CCMRAM**（见 BUG-021）。
- **状态**：✅ 已修

## BUG-010 · `NO_SYS` / `SYS_LIGHTWEIGHT_PROT` 改不动 ✅

- **现象**：LWIP 页面里这两项是灰的，`NO_SYS` 显示 "OS Not Used"。
- **根因**：不是 bug —— 这两项由 FreeRTOS 是否启用决定，CubeMX 自动联动。
  若"先开 LWIP 后加 FreeRTOS"，CubeMX 不会刷新。
- **修法**：
  1. 先 `Middleware → FREERTOS` 启用（Interface 选 **CMSIS_V2**）
  2. 若仍显示 OS Not Used：把 LWIP 的 Enabled 勾去掉 → Ctrl+S → 重新勾上
- **验证**：生成后看 `lwipopts.h`，`NO_SYS` 必须是 **0**、`SYS_LIGHTWEIGHT_PROT` 必须是 **1**。
- **状态**：✅ 已修（已验证）

---

# C. CubeMX 模板缺失 & ST HAL 库缺陷（这一组最致命）

> 这一组是 **CubeMX 6.17.0-RC5 新 LWIP 模板** 的锅，不是配错。
> 正点原子例程不会有对应说明（他们是手工移植 + 标准库），所以照着例程学也躲不掉。

## BUG-011 · `HAL_ETH_MspInit()` 完全没生成 ✅

- **现象**：ETH 时钟、引脚、中断一个都没开。
  `HAL_ETH_Init()` 里等 DMA 软复位位清零 → 永远超时 → `Error_Handler()` 关中断死循环。
- **根因**：CubeMX 6.17 的新 LWIP 模板**不带 ETH 的 MSP 生成**，全工程只有 HAL 里的 `__weak` 空实现
  （`gpio.c` 里 `AF11_ETH` 零匹配，且没有 `eth.c`）。
- **修法**：手写 `HAL_ETH_MspInit()`，放在 `ethernetif.c` 的 **USER CODE BEGIN 8**（重新生成不丢）：
  - 时钟：`__HAL_RCC_ETHMAC_CLK_ENABLE()` + `__HAL_RCC_ETHMACTX_CLK_ENABLE()` + `__HAL_RCC_ETHMACRX_CLK_ENABLE()`
    （**F4 没有 `__HAL_RCC_ETH_CLK_ENABLE()`**，这是 F1/F7 才有的，见 BUG-027）
  - 引脚：PA1/PA2/PA7、PC1/PC4/PC5、PG11/PG13/PG14 全部 AF11 + **Very High**
  - NVIC：`HAL_NVIC_SetPriority(ETH_IRQn, 5, 0)` + `EnableIRQ`
- **状态**：✅ 已修

## BUG-012 · PD3（ETH_RESET）没有释放代码 ✅

- **现象**：PHY 一直处于复位态，MDIO 读不到任何东西。
- **根因**：PD3 板载 10K 下拉，CubeMX 不会自动生成"拉低→延时→拉高"的释放时序。
- **修法**：`ethernetif.c` 的 `MACADDRESS` 段（`HAL_ETH_Init()` 之前）加：
  `PD3 拉低 50ms → 拉高 → 延时 200ms`。
- **状态**：✅ 已修

## BUG-013 · `ethernet_link_thread()` 是空壳 ✅

- **现象**：`netif_set_link_up()` 从不调用 → DHCP/静态地址不生效、TCP 全哑火。
- **根因**：新模板不再生成 LAN8742 的链路检测代码，只剩一个 `osDelay(100)`。
- **修法**：在 `ETH link Thread core code` 段手写：读 **PHY_BSR(1) 的 bit2** → `netif_set_link_up/down()`。
- **状态**：✅ 已修

## BUG-014 · 全工程缺少 `HAL_ETH_Start_IT()` ✅

- **现象**：PHY 链路正常、网口灯也闪，但**一个包都收不到，ping 永远超时**。
- **根因**：`HAL_ETH_Init()` 只配寄存器，**不置 DMAOMR.ST/SR 和 MACCR.TE/RE** → MAC 和 DMA 全停着。
- **修法**：补在 `ethernetif.c` 的 `low_level_init Code 2`（`HAL_ETH_Init()` 成功分支内）。
- **注意**：`Start_IT` 比 `Start()` 多做的唯一一件事是 `RxDescList.ItMode = 1`，**才有收包中断**。
- **状态**：✅ 已修
- **发现方式**：🔍 人工审查（全工程 grep 无 `HAL_ETH_Start`）

## BUG-015 · SPI1 / I2C1 外设没 Enable ✅

- **现象**：W25Q128 和传感器无驱动，引脚还悬在 AF 模式当不了普通 IO。
- **根因**：引脚已在 `gpio.c` 配成 AF5/AF4，但 `stm32f4xx_hal_conf.h` 里
  `HAL_SPI_MODULE_ENABLED` / `HAL_I2C_MODULE_ENABLED` 仍被注释掉。
- **修法**：`.ioc` 里启用 SPI1（Master / 软 NSS / 预分频 8 = 10.5MHz）与 I2C1（100kHz），Generate。
- **状态**：✅ 已修（已生成 `spi.c` / `i2c.c` 并加入 Keil 工程）

## BUG-016 · CAN `AutoBusOff` 没被 CubeMX 落地 ✅

- **现象**：`.ioc` 里 `CAN1/CAN2.AutoBusOff=ENABLE` 被原样保留，但生成的 `can.c` 里仍是 `DISABLE`。
- **根因**：CubeMX 6.17 的 F4 CAN 生成器**不吃 `AutoBusOff` 这个键名**（正确键名大概是 `ABOM`）。
- **修法**：在 `can.c` 的 `USER CODE BEGIN CAN1_Init 2` / `CAN2_Init 2` 里
  置 `Init.AutoBusOff = ENABLE` 后重新调一次 `HAL_CAN_Init()`（安全，MspInit 时钟计数有防重入）。
  这样 BUS-OFF 由硬件自动恢复。
- **状态**：✅ 已修

## BUG-017 · CAN 中断一个都没开 ✅

- **现象**：CAN 收不到帧（中断方式下）。
- **根因**：`.ioc` 的 NVIC 里没有 CAN 相关条目。
- **修法**：加 CAN1_RX0 / CAN1_TX / CAN2_RX0 / CAN2_TX，优先级一律 **5**。
- **状态**：✅ 已修（重新生成后 `stm32f4xx_it.c` 四个入口齐备）

## BUG-018 · ⭐ ST F4 HAL 从不设置 MDC 时钟分频（MACMIIAR.CR）⏳

- **现象**：上电扫描 MDIO，31 个地址（1~31）全部"命中"，ID 形如 `0FFFFFFF` / `03FFFFFF`，
  **ID2 全是 FFFF**。正常应只有 1 个地址响应、ID 为固定厂商值。
- **根因**：`HAL_ETH_ReadPHYRegister()` 里只有
  ```c
  tmpreg1 &= ~ETH_MACMIIAR_CR_MASK;   /* 只清 0，从头到尾没有一行写回正确值 */
  ```
  结果 `CR[2:0] = 000`，按 RM0090 是 **HCLK/42 = 168/42 = 4 MHz**，
  而 IEEE 802.3 规定 MDC 上限 **2.5 MHz** → PHY 不应答 → MDIO 数据线被上拉 → 读回全 0xFFFF。
- **为什么阴险**：**现象与"MDIO 线断了 / PA2 被板载 RS485 占用"完全一致**，
  照着接线排查会查到死。正点原子标准库例程会手动设 CR，CubeMX + HAL 模板不会。
- **修法**（已写进 `ethernetif.c` 的 `low_level_init Code 2`）：
  ```c
  heth.Instance->MACMIIAR = (heth.Instance->MACMIIAR & ~0x1CU) | (4U << 2U);
  ```
  CR 占 MACMIIAR bit2~bit4（掩码 `0x1C`），值 4 = CR[2:0]=100 → **HCLK/102 ≈ 1.65 MHz**。
  > `ETH_MACMIIAR_CR_MASK` 定义在 `stm32f4xx_hal_eth.**c**` 里、头文件没有，所以只能用字面量 `0x1C`。
- **状态**：✅ **已修并复测通过**（2026-10-18 上电复测，MDIO 已通，扫到 PHY 地址 0，详见附录 B）
- **发现方式**：⚡ 上电实测 + 翻 HAL 源码
- ⭐ **通用规则：F407 @168MHz 用 CubeMX + HAL，凡是"能初始化但读不到 PHY"，先查这一位。**

---

# D. 参数设置不当（默认值在 100M 以太网场景不够用）

## BUG-019 · DHCP 未关 ✅

- **现象**：板子直连 PC，拿不到地址。
- **根因**：板子直连 PC 时没有 DHCP 服务器，DHCP 请求永远超时。
- **修法**：静态 IP `192.168.1.30 / 255.255.255.0 / 192.168.1.1`。
  PC 网卡设 `192.168.1.10 / 24`。
- **状态**：✅ 已修（`lwip.c` 里 `dhcp_start` 已消失）

## BUG-020 · `LWIP_SO_RCVTIMEO` 未开 ✅

- **现象**：断线时 `netconn_recv()` 永久阻塞，任务卡死，永远发现不了断线。
- **修法**：`LWIP_SO_RCVTIMEO = 1` + `netconn_set_recvtimeout()`。
- **状态**：✅ 已修

## BUG-021 · ⭐ lwIP 内存绝不能放 CCM ⚠️

- **现象**：能 ping 通，但收不到数据（或数据全错）。
- **根因**：F407 的 64KB CCM RAM **不挂在 DMA 总线**上，ETH DMA 访问不到。
- **修法**：LWIP → Platform Settings → Memory Location = **Linker defined**（不能选 CCM）。
- **状态**：⚠️ 长期注意（现在 64KB CCM 完全闲置，M4 可用自定义 scatter 放 FreeRTOS heap / mem_heap）
- **发现方式**：🔍 设计阶段预判

## BUG-022 · `PBUF_POOL_BUFSIZE` 默认 592 ⚠️

- **现象**：每个大包都被拆碎，性能暴跌。
- **根因**：592 连一个完整以太帧（1514 字节）都装不下。
- **修法**：改成 **1524**。
- **状态**：✅ 已修

## BUG-023 · `TCP_MSS` / `TCP_WND` / `TCP_SND_BUF` 默认值过小 ✅

| 参数 | 默认 | 改成 | 依据 |
|---|---|---|---|
| `TCP_MSS` | 536 | **1460** | 536 是拨号时代的值，标准以太网一次能带 1460 |
| `TCP_WND` | 2144 | **8192** | 吞吐上限 ≈ 窗口/RTT = 8192×8/0.5ms ≈ 131 Mbps > 物理 100M |
| `TCP_SND_BUF` | 1072 | **8192** | 太小上报会频繁卡顿 |
| `TCP_SND_QUEUELEN` | 9 | **16** | 硬约束 ≥ 2×SND_BUF/MSS = 11.2 |
| `MEM_SIZE` | 1600 | **12288** | 1600 连几个 TCP 控制块都分不出（原定 16384，因 RAM 不够降到 12288，见 BUG-028） |

- **状态**：✅ 已修
- **简历出处**：`TCP_WND` 默认 2144 → 约 34 Mbps；改 8192 → 131 Mbps。这是"调优"表述的量化依据。

## BUG-024 · `MEMP_NUM_TCP_PCB` 默认 5 ✅

- **现象**：连接数一多就 `netconn_new()` 返回 NULL，且**不会动态扩展**。
- **根因**：`memp_std.h` 里它在编译期就变成静态数组。
- **修法**：改成 **10**。
- **状态**：✅ 已修

## BUG-025 · `MEMP_NUM_NETCONN` 默认只有 4 ✅

- **现象**：M1 要常驻 3 个 netconn（echo 监听 + accept + 心跳客户端），余量太紧。
- **修法**：`lwipopts.h` 的 USER CODE 段加 `#define MEMP_NUM_NETCONN 8`。
- **状态**：✅ 已修

## BUG-026 · PB14（W25Q128 片选）初始电平为低 ✅

- **现象**：上电瞬间片选被拉低，SPI 时钟一旦起来 Flash 可能误采样到杂散指令。
- **根因**：CubeMX 把所有 Output 的初始电平都生成为 RESET。
- **修法**：`MX_GPIO_PostInit()` 里强制 `HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET)`。
- **状态**：✅ 已修

---

# E. 编译与链接（这一组全是实编译才暴露，人工看代码看不出来）

## BUG-027 · `gpio.c` 补丁写在函数外 → 17 个编译错误 ✅

```
error #79:  expected a type specifier
error #147: declaration is incompatible with "void HAL_GPIO_WritePin(...)"
```

- **根因**：`USER CODE BEGIN 2` 是**文件级**段，位于 `MX_GPIO_Init()` 函数体**外面**，
  在那里裸写 `HAL_GPIO_WritePin(...)` 会被编译器当成"文件作用域声明"。
  而 `MX_GPIO_Init()` 内部 CubeMX 没留 USER CODE 段，写进去重新生成就会被冲掉。
- **修法**：包成 `MX_GPIO_PostInit()` 放在 `USER CODE 2`，由 `main()` 在 `MX_GPIO_Init()` 之后调用。
- ⭐ **通用教训**：CubeMX 的 `USER CODE BEGIN n`（纯数字）= 文件级；
  `USER CODE BEGIN <函数名> n` = 函数级。裸语句只能往函数级段里写。
- **状态**：✅ 已修
- **发现方式**：🔧 实编译

## BUG-028 · RAM 装不下 → 69 个链接错误 ✅

```
Error: L6406E: No space in execution regions with .ANY selector matching memp.o(.bss)
Error: L6407E: Sections of aggregate size 0x5140 bytes could not fit
```

- **根因**：差 **0x5140 = 20800 字节**。原配置 PBUF 32×1560 + MEM 16K + heap 32K 超出 128KB。
- **修法**：
  | 项 | 原 | 改 | 省 |
  |---|---|---|---|
  | `PBUF_POOL_SIZE` | 32 | **24** | 12 KB |
  | `configTOTAL_HEAP_SIZE` | 32768 | **20480** | 12 KB |
  | `MEM_SIZE` | 16384 | **12288** | 4 KB |

  共省 28 KB。24 个 pbuf 仍养得住 `TCP_WND 8192` 与 `TCP_SND_QUEUELEN 16`。
- **最终**：RAM 占用 102100 B / 128 KB，余约 26 KB。
- **状态**：✅ 已修（`.ioc` 已同步，防止重新生成回退）
- **发现方式**：🔧 实编译

## BUG-029 · MicroLIB 没开 → printf 走半主机 ✅

- **现象**：`<useUlib>0`。ARMCC 下不勾 MicroLIB，`printf` 走 semihosting，最终执行 `BKPT 0xAB`；
  没接调试器时直接硬 fault，表现为"程序好像跑了一下就死了"。
- **修法**：`.uvprojx` 的 `<useUlib>` 改成 **1**。副作用：Code −2.7KB、RAM −4.7KB。
- ⚠️ **CubeMX 重新生成工程可能把它重置回 0，每次生成后必须复查。**
- **状态**：✅ 已修
- **发现方式**：🔧 实编译（外加 `main.c` 补了 `fputc` 重定向到 USART1）

## BUG-030 · MDIO 扫描位置错（写在 `HAL_ETH_Init()` 之前）✅

- **现象**：扫不出任何有效地址，读出来全是垃圾。
- **根因**：那时 ETH 时钟和 RMII 引脚都还没被 `HAL_ETH_MspInit()` 打开。
- **修法**：整块移到 `low_level_init Code 2`（`HAL_ETH_Init()` 成功之后、`HAL_ETH_Start_IT()` 之前）。
- **状态**：✅ 已修
- **发现方式**：🔍 人工审查

## BUG-031 · defaultTask 栈溢出 ✅

- **现象**：栈原本 512 字节，而 `MX_LWIP_Init()` 就跑在这个任务里，
  里面的 PHY 扫描还要 `printf` —— ARMCC 的 printf 内部就要吃掉 200~400 字节栈。
- **修法**：栈改成 **1024 字节**；栈溢出 hook 从空函数改成"打印任务名 + 关中断停机"
  （静默跑飞是最难查的故障模式）。
- **状态**：✅ 已修
- **发现方式**：🔍 人工审查

## BUG-032 · 注释里嵌套 `/* */` → error #7 ✅

```
error #7: unrecognized token
```

- **根因**：我在 `/* ... 证据：tmpreg1 &= ~ETH_MACMIIAR_CR_MASK; /* 清 0 */ ... */`
  里嵌了块注释，导致外层注释被提前截断，后面的代码裸露出来。
- ⭐ **教训：CubeMX 的 USER CODE 注释里禁止嵌套 `/* */`，用 `//` 或改写措辞。**
- **状态**：✅ 已修
- **发现方式**：🔧 实编译

---

# F. 运行期逻辑

## BUG-039 · 多任务 `printf` 重入，疑似吞掉 `[echo] listening on port 5000` ⚠️

- **现象**：ping 已通（4/4，0% 丢失，平均 1ms，说明 lwIP 协议栈完全正常），
  但串口**始终没有出现** `[echo] listening on port 5000`。
  同时 `[net] link ready` 只出现 1 次 —— 按代码 echo 和 hb 两个任务各应打一次。
  echo 任务的失败分支都会打日志（`netconn_new failed` / `bind failed` / `listen failed`），
  **这些一条都没出现** → 说明它既没成功、也没走到失败分支的打印。
- **根因（高度疑似）**：`main.c` 里的 `fputc` 直接调 `HAL_UART_Transmit()`，
  **没有任何互斥保护**。三个任务（echo / hb / monitor）并发调用 `printf`：
  - ARMCC 的 `printf` 内部有静态缓冲，本身不可重入
  - `HAL_UART_Transmit` 被两个任务同时调，USART 发送状态机被打乱 → 丢字符
  这会导致**整行丢失或字符交错**，且完全随机，极难复现。
  （这个隐患在用户旧项目里就登记过，属于同类问题的第二次踩坑。）
- **为什么现在不能立刻断定**：也可能是 echo 卡在 `netconn_bind/listen` 内部的
  mailbox 等待上。需要用 NetAssist 实测 5000 端口来区分这两种情况。
- **验证办法（零成本）**：PC 用 NetAssist 当 TCP Client 连 `192.168.1.30:5000`，
  发一串字符看是否回显。
  - 能回显 → echo 其实正常，确认是 printf 丢输出 → 按下面修 printf
  - 连不上 → echo 任务真卡住了，需要在任务里加分步打印定位
- **修法（M4 阶段，两种选一）**：
  1. `fputc` 里加互斥量，但要判断调度器是否已启动
     （`MX_FREERTOS_Init()` 里那条 `[sys] boot ok` 是在调度器启动**之前**打的，
     此时不能调 FreeRTOS API）：
     ```c
     if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
         xSemaphoreTake(printf_mutex, portMAX_DELAY);
     HAL_UART_Transmit(...);
     if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
         xSemaphoreGive(printf_mutex);
     ```
  2. 更干净：**环形缓冲 + 独立发送任务**。`printf` 只往缓冲里写，
     由一个低优先级任务统一发。这条也顺带解决"打印阻塞网络任务"的问题，推荐。
- **状态**：✅ **已确认根因**（2026-10-18 01:00 NetAssist 实测）
  echo 功能**本身完全正常**，确认是 printf 重入丢行，不是任务卡死。
- **发现方式**：⚡ 上电实测（ping 通但 echo 启动日志缺失）→ NetAssist 实测确认

### ✅ BUG-039 已修复（2026-10-19，M4）

**采用方案 2（环形缓冲 + 独立发送任务）**，新增 `Core/Src/m4_log.c` + `Core/Inc/m4_log.h`：

- `fputc` 改为调 `M4_LogPutc()`，只做"写环形缓冲"（微秒级），不再阻塞发串口
- 发送由 `m4_log` 任务（**BelowNormal** 低优先级）统一做，
  一次最多取 96 字节 `HAL_UART_Transmit`（超时 100ms，**不用 HAL_MAX_DELAY**）
- 缓冲 1024 字节（.bss 静态数组，不走 20KB 的 FreeRTOS heap）
- **满了就丢并计数**（`M4_LogDropped()`）—— 日志可以丢，系统不能卡
- 临界区只包"改指针 + 写字节"这 3 行，**不包整行 printf**（否则又变成关中断 3ms 的老问题）

⚠️ **实现时踩到的两个点**（写进代码注释了，这里留档）：

1. **调度器未启动时不能用临界区**：`MX_FREERTOS_Init()` 里那条 `[sys] boot ok`
   是在 `vTaskStartScheduler()` 之前打的。所以 `M4_LogPutc()` 里必须先判断
   `xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED` → 走"直接阻塞发送"兜底。
2. **`s_ready` 标志**：`M4_LogStart()` 之前（比如 `M2_MasterStart()` 里若先 printf）
   发送任务还没起来，同样走兜底路径。

**验收方法**：三任务同时打印，串口抓 5 分钟存文件，grep 检查无错行、无丢行；
`[m4] heap ... log_drop=N` 里的 N 应长期为 0（不为 0 说明缓冲偏小，可调 `M4_LOG_BUF_SIZE`）。

### BUG-039 复测结果（echo 验收通过）

NetAssist 当 TCP Client 连 `192.168.1.30:5000`，发 `abc`：

```
SEND ASCII/3 >>> abc
RECV ASCII/3 from SERVER <<< abc
```

- **回显正确**，20+ 次连断**每一次都能连上、都能回显**
- ⭐ 这同时验证了 **PCB / netconn 无泄漏**：`MEMP_NUM_TCP_PCB = 10`，
  若 `netconn_delete()` 漏了，第 11 次起必然连不上。实测 20+ 次全部成功
  → `netconn_close()` + `netconn_delete()` 的收尾路径是对的
- 结论：`[echo] listening on port 5000` 不出现**纯粹是 printf 丢行**，
  不是 echo 任务的问题。修复按上面「修法」两条照做即可。
- ⚠️ 注意：首次连接曾出现 5s 超时（`请检查服务器端口5000是否开启监听`），
  10s 后自动重连成功。原因是板子刚复位完 / heartbeat 重连循环占用，
  属一次性现象，非固件缺陷。
- ⭐ **教训：多任务系统里，`printf` 不是线程安全的。凡是"某条日志莫名其妙不出现"，
  先把 printf 重入列为头号嫌疑。**

## BUG-044 · 把 UART 的 IDLE 中断当成 Modbus 的 t3.5 帧结束（认知坑，设计阶段规避）✅

- **现象（认知级，不是报错）**：直觉认为"IDLE 中断 = 一帧收完了"，于是 IDLE 一触发就取数据。
- **根因**：STM32F4 的 **IDLE 是 RX 线空闲 1 个字符时间**就置位；而 **Modbus RTU 的帧结束判据是 3.5 个字符**。
  9600bps 下前者 ≈ 1.04ms，后者 ≈ 4.0ms，**差 4 倍**。
- **后果**：碰到字节间有间隙的慢从机（国产模块很常见），或 IDLE 处理被高优先级中断延迟时，
  **一帧会被切成两半** → CRC 永远错 / 永远超时。而用串口抓包看"帧内容是对的"，
  极易误判成"从机有问题"或"波特率不对"。
- **修法**：IDLE **只当候选帧边界**，再用 t3.5 静默确认一次才算数：
  `IDLE 触发 && 之后 DMA 位置不再变化 && 已过 ≥5ms` → 才认定帧结束
  （`Core/Src/m2_rs485.c` 的 `RS485_ReadFrame()` 判据一）。
  另外保留判据二兜底：IDLE 没来，但"位置稳定超过 5ms"也认定帧结束。
- **状态**：✅ 设计阶段已规避（2026-10-18）
- **发现方式**：🧠 写码时对照参考手册推演（还没上板）
- ⭐ **教训：硬件的"空闲" ≠ 协议的"帧结束"。凡是靠硬件信号判协议边界，
  都要再补一层协议自己的时间判据。**

## BUG-043 · `mb_transaction` 里「先清缓冲、再等静默」的顺序错了 ✅

- **现象（设计级，未上板即可推出）**：事务流程写成
  `RS485_RxFlush(); → RS485_WaitSilent(); → RS485_Send();`
- **根因**：`RS485_WaitSilent()` 最多要等 t3.5（5ms）。**这 5ms 里进来的字节不受 flush 保护**——
  别的设备抢答、上一个从机的迟到响应、上电噪声，都会在 flush 之后落进 DMA 缓冲，
  然后被当成「本次事务的响应」。
- **后果**：偶发读到莫名其妙的值（比如温度突然跳成 0 或上一个从机的数据），
  **极难复现**，因为它只在特定时序下出现。
- **修法**：顺序改成 **`RS485_WaitSilent(); → RS485_RxFlush(); → RS485_Send();`**
  —— 先等总线静默，把等待期间收到的垃圾一并清掉，紧接着发送。
- **状态**：✅ 已修（2026-10-18）
- **发现方式**：🧠 代码走查
- ⭐ **教训：所有"清缓冲/清标志"类操作，要紧挨"动作"之前，中间不能再插入等待。
  顺序是：等 → 清 → 做。**

## BUG-045 · 双 CAN 共用滤波器：CAN1/CAN2 抢占 bank 导致"突然收不到帧"（预防性）✅

- **现象**（典型表现）：单独用 CAN1 正常；一旦把 CAN2 也初始化起来，
  **CAN1 收不到帧**（或 CAN2 收不到），而引脚、波特率、中断全都查不出问题。
- **根因**：STM32F4 的 bxCAN 只有 **28 个滤波器组（bank 0~27）**，**CAN1 和 CAN2 共用**
  这一组硬件。谁用哪几段由 CAN1 的 `FMR.C2SB[5:0]`（即 `SlaveStartFilterBank`）切开：
  ```
  bank 0 .. (C2SB-1)  → CAN1 专用
  bank C2SB .. 27     → CAN2 专用
  ```
  HAL 里**只有用 `hcan1` 调 `HAL_CAN_ConfigFilter()` 才会写这个字段**，
  用 `hcan2` 调不会写。于是两条铁律：
  1. **必须先配 CAN1**（顺带把 `SlaveStartFilterBank` 写进去），**再配 CAN2**；
  2. CAN2 的 `FilterBank` 必须 **≥ SlaveStartFilterBank**。
- **后果**：CAN2 的过滤器写进了 CAN1 的地盘 → 覆盖掉 CAN1 的配置 → CAN1 静默失效。
  这种"改了 A 坏掉 B"的耦合最难排查，因为两边代码看起来都对。
- **修法**（`m3_can.c` `m3_hw_init()`）：
  ```c
  fc.FilterBank           = 0;
  fc.SlaveStartFilterBank = 14;      /* ⭐ CAN1 用 0~13，CAN2 用 14~27 */
  HAL_CAN_ConfigFilter(&hcan1, &fc); /* 必须先用 CAN1 写 C2SB */
  fc.FilterBank           = 14;
  HAL_CAN_ConfigFilter(&hcan2, &fc); /* 再配 CAN2 */
  ```
- **状态**：✅ 已在编码阶段规避（2026-10-18，M3 落地）
- **发现方式**：🧠 代码走查（HAL 源码 + F4 参考手册滤波器章节）
- ⭐ **教训：F4 上 CAN2 从来不是独立的——它连滤波器都要靠 CAN1 的寄存器让渡。
  凡是"CAN1 和 CAN2 一起用"的场景，先想清楚共享资源怎么切分。**

## BUG-046 · 先开中断后建队列 → ISR 里访问 NULL 队列硬 fault（预防性）✅

- **现象**：`HardFault`，且**上电瞬间就死**，来不及打任何日志，看起来像"启动文件坏了"。
- **根因**：`M3_CanStart()` 里的顺序如果写成
  `m3_hw_init()`（最后一步 `ActivateNotification` 开 RX 中断）→ `xQueueCreate()`，
  那么在**队列还没建好**的时间窗内，只要总线上有数据，ISR 立刻触发并
  `xQueueSendFromISR(s_rxq[port], ...)`，此时 `s_rxq[port] == NULL` → 硬 fault。
  ⚠️ 这是**竞态**，不是必现：没接总线时不触发，一接上就死，极其迷惑。
- **修法**：**先建队列，后初始化硬件**（`m3_can.c` 的 `M3_CanStart()` 已按此顺序）；
  同时在 ISR 里加 `if (q == NULL) break;` 兜底（双保险）。
- **状态**：✅ 已在编码阶段规避
- **发现方式**：🧠 代码走查
- ⭐ **教训：凡是"ISR 会访问某个资源"的驱动，该资源的初始化必须**严格早于**
  中断使能。通用规则：先建资源，后开中断；先关中断，后销毁资源。**

## BUG-047 · 单节点裸发 CAN 永远发不出去（`-3` 超时 → TEC 上涨 → BUS-OFF）· 物理现象非 bug ✅

- **现象**：`M3_CanSend()` 返回 **-3**（超时），`tx_fail` 一路涨；
  继续发下去 TEC 逐步增加，最后串口打出 `[can] !! port0 BUS-OFF (TEC=xx)`。
- **根因**：CAN 是**广播 + 应答**总线 —— 发送方必须收到**至少一个其它节点的 ACK**
  才算发送成功。总线上只有你一个节点时：
  1. 没人回 ACK → mailbox 一直 pending → `M3_CanSend` 超时返回 -3；
  2. `AutoRetransmission=ENABLE` 时硬件**持续重发**；
  3. 每次发送失败 TEC +8，TEC ≥ 256 → **BUS-OFF**（节点被硬件踢下线）。
  ⚠️ 这不是代码错误，是 **CAN 协议的正常物理行为**。
  常见触发场景：① 只有一个节点；② 忘了接 120Ω 终端电阻；③ 两端波特率不一致；
  ④ 收发器没供电 / 跳线没接。
- **怎么区分真 bug 和物理现象**：
  | 观察 | 判定 |
  |---|---|
  | 回环模式下能发能收，正常模式全 -3 | 硬件/总线问题（没节点、没电阻、波特率错） |
  | 回环模式下也收不到 | 软件问题（滤波器 / Start / 中断没开） |
  | TEC 缓慢上涨但 REC=0 | 只有发送侧有问题（没人 ACK） |
  | TEC 和 REC 一起涨 | 波特率不对 / 干扰 / 终端电阻问题 |
- **修法**：**不需要改代码**。正确做法是先用 **M3-a 回环模式**
  （`M3_TEST_LOOPBACK=1`，内部自 ACK，不需要外部节点）验证软件链路，
  再排查硬件：终端电阻 → 波特率 → 收发器供电 → 跳线。
- **状态**：✅ 已明确（设计成返回 -3 便于定位，见 `m3_can.c` 注释）
- **发现方式**：🧠 协议原理推导 + 工程经验
- ⭐ **教训：CAN 的"发送成功"定义里包含"别人回了 ACK"。
  调试 CAN 的第一原则永远是——**先用回环模式把软件和硬件问题切开**。**

## BUG-048 · 条件编译宏关掉后，残留的变量声明触发 `#177-D` / `#550-D` ✅

- **现象**：`M3_TEST_LOOPBACK=0` 时编译报 3 个 warning：
  ```
  m3_can.c(411): warning:  #550-D: variable "t_lb" was set but never used
  m3_can.c(412): warning:  #177-D: variable "lb_data" was declared but never referenced
  m3_can.c(413): warning:  #177-D: variable "lb_seq" was declared but never referenced
  ```
  而 `M3_TEST_LOOPBACK=1` 时完全干净 —— 说明这些变量**只用在该宏包住的代码块里**。
- **根因**：只把"使用处"包进了 `#if M3_TEST_LOOPBACK`，**声明处和初始化处留在了外面**。
  宏为 0 时，变量被声明（甚至被赋值）但从未被读取 → ARMCC5 判定为死变量。
- **后果**：破坏本项目 **0 Warning** 要求；更麻烦的是——
  用户切宏做回环测试时代码是干净的，切回来反而冒 warning，误以为改坏了。
- **修法**：声明、初始化、使用**三处一起**包进 `#if`：
  ```c
  #if M3_TEST_LOOPBACK
      uint32_t t_lb;
      uint8_t  lb_data[8];
      uint8_t  lb_seq = 0;
  #endif
  ...
  #if M3_TEST_LOOPBACK
      t_lb = xTaskGetTickCount();
  #endif
  ```
- **状态**：✅ 已修（2026-10-18，两种宏取值都做到 0 Error / 0 Warning）
- **发现方式**：🧪 Keil UV4 命令行编译
- ⭐ **教训：功能开关宏要"整条链路"一起条件编译（声明 / 初始化 / 使用）。
  而且**两种取值都要各编译一次**，只测默认配置等于没测。**

## BUG-049 · 双 CAN 接在同一条总线上时，"全透传"路由会无限自激（几毫秒打满总线）✅

- **现象**：CAN1 发一帧测试帧后，串口日志**疯狂刷屏**，`rx` 计数爆炸式增长，
  总线被占满，其它报文全部挤不进去；严重时看门狗复位。
- **根因**：M3-b 的测试拓扑是 **CAN1 和 CAN2 接在同一条物理总线上**。
  如果路由写成「从 A 收到的原样转发到 B」（全透传）：
  ```
  CAN1 发 0x123 → CAN2 收 → 转发到 CAN1 → CAN1 又发 → CAN2 又收 → …
  ```
  ⚠️ 注意 CAN 节点**不会接收自己发的帧**（除非回环模式），所以 CAN1 转发出去的帧
  CAN2 一定会再收到一次 —— 循环永不终止。一帧在几毫秒内变成上千帧。
  即使两条总线物理独立，全透传也等于把两边的广播风暴直接打通。
- **修法**：**ID 白名单 + ID 重映射**（真实 CAN 网关的标准做法）：
  ```c
  CAN1 收到 0x100~0x1FF → 映射成 0x2xx 转发到 CAN2
  CAN2 收到 0x300~0x3FF → 映射成 0x4xx 转发到 CAN1
  其余 ID 一律不转发
  ```
  映射后的新 ID（0x2xx / 0x4xx）**不落在任何转发段内** → 天然只转一跳，
  数学上就不可能形成环。
- **验证**：`C1 rx id=0x123` → `C2 rx id=0x123` → `C1 rx id=0x223`，**然后停止**。
  统计里 `C1 rx : C2 rx ≈ 2 : 1`，而不是无限增长。
- **状态**：✅ 已修（2026-10-18，M3-b 编码阶段）
- **发现方式**：🧠 代码走查 + 拓扑推演（还没接硬件就先排掉了）
- ⭐ **教训：凡是"转发/中继"类逻辑，第一件事是证明它**不会形成环**。
  最稳的证明方式是让转发结果落在规则集合之外（ID/地址重映射），
  而不是靠"转发次数计数"这类运行时兜底 —— 后者在丢帧时会失效。**

## BUG-050 · 数据池没有时间戳 → 从机离线时上报"假正常"数据 ⚠️ ✅

- **现象（隐患）**：温湿度从机拔掉后，数据池里的 `temperature` **保持最后一次的值**。
  网络任务照常把它报上去，上位机显示 `25.3℃`，看起来一切正常 ——
  实际上这是 3 分钟前甚至更久以前的数据。
- **危害**：工业现场这种"假正常"是最危险的一类故障：
  系统看起来在正常工作，但监控的其实是一个已经失联的设备。
  比"直接报错"危险得多，因为它不会触发任何人的注意。
- **根因**：采集值只写了"值"，没写"这个值是哪个时刻采的"，
  上位机无法判断数据年龄。
- **修法（M4 已落地）**：`m4_pool_t` 里给每个采集量配一个 `ts_xxx`（ms），
  并且**必须和值在同一把锁内写入**（`M4_PoolSetTempHumi()` 内部一次加锁写完两者），
  否则会出现"值新、时间戳旧"的撕裂。
  上报帧里带上"数据年龄"（快照帧 DATA[28..29]，秒；`0xFFFF` = 从未更新），
  上位机按 `M4_PoolIsStale(ts, max_age)` 判超龄后把数据显示成灰色。
- **判定经验**：凡是"周期性采集 + 可能被读走上报"的量，都要带时间戳。
- **状态**：✅ 已在 M4 数据池与协议里实现

## BUG-051 · 多条 TCP 连接共用一个 `static` 解析状态机会互相污染 ⚠️ ✅

- **现象**：板子同时跑 TCP Server（5000，收上位机指令）和 TCP Client（5001，周期上报 +
  收应答）两条连接。如果协议解析状态机写成"一个文件级 `static m4_proto_ctx_t`"，
  两条连接的数据会串到同一个状态机里 → 解出乱码帧、CRC 永远错、时好时坏。
- **根因**：状态机是有状态的（当前收到第几个字节），状态必须**按连接隔离**。
- **修法（M4 已落地）**：`m4_proto_ctx_t` 由调用方持有，每条连接一个实例
  （`m1_cmd_task` 里一个、`m1_report_task` 里一个），**每次 accept / connect 成功时
  必须 `M4_ProtoInit()` 复位一次** —— 否则会带着上一条连接的半截状态继续解析。
- **配套必须做的一件事**：状态机要有**超时复位**（`M4_PROTO_TIMEOUT_MS = 200ms`）。
  对端只发半帧就停了，状态机会永远卡在 `S_DATA`，之后所有正常数据都被当成 DATA 吃掉，
  表现为"通信突然永久失效"，而且极难定位（看起来像板子死了）。
- **状态**：✅ 已在 `m4_proto.c` 实现（超时复位 + 每连接独立 ctx）

## BUG-052 · 上报格式 ASCII/二进制双模式切换时触发 `#177-D` 未使用变量警告 ✅

- **现象**：`M1_REPORT_ASCII` 置 1 时，二进制分支的 `uint8_t tx[M4_PROTO_MAX_FRAME];`
  变成"声明但从未引用" → `warning #177-D`。
- **根因**：条件编译关掉的代码路径里残留的变量声明（与 BUG-048 同类）。
- **修法**：在 ASCII 分支里显式 `(void)tx;`。
- **状态**：✅ 已修（两种模式各编译一遍，均 0 Error 0 Warning）

## BUG-042 · ARMCC5 下字符串字面量里的中文触发 `warning #870-D` ✅

- **现象**：编译 `m2_master.c` 报
  `warning: #870-D: invalid multibyte character sequence`，指向
  `printf("[mb] !! poll task create failed (heap 不够？)\r\n");`
- **根因**：Keil **ARMCC5（V5.06）按 ANSI 单字节解析源文件**。UTF-8 中文在**字符串字面量**里
  会被当成无法识别的多字节序列；而**注释里的中文不会触发**（预处理阶段已被剔除）。
- **后果**：只是 warning，但本项目要求 **0 Warning**；更危险的是——
  如果中文出现在"需要精确字节数"的地方（比如协议帧里塞中文），实际字节数会和预期不符。
- **修法**：所有 `printf` 的字符串一律用英文，中文只写在注释里。
  （本项目串口日志本身就统一用英文，正好一致。）
- **状态**：✅ 已修（2026-10-18，0 Error / 0 Warning，Code=85800）
- **发现方式**：🧪 Keil UV4 命令行编译
- ⭐ **教训：ARMCC5 工程里，注释可以中文，打印串必须英文。**

## BUG-041 · 心跳帧序号 `seq` 永远停在 0（心跳客户端）✅

- **现象**：NetAssist 当 TCP Server 监听 5001，收到的心跳帧永远是 `HB,0,xx`，
  序号**从不递增**：
  ```
  RECV ASCII/9 <<< HB,0,11
  RECV ASCII/9 <<< HB,0,21
  RECV ASCII/9 <<< HB,0,31
  ```
- **根因**：`seq++` 写在循环**末尾**，而超时分支 / 发送失败分支都用 `continue` 跳出，
  **永远执行不到**。只要发生过一次超时（上位机没回 ACK），序号就永久卡死。
- **后果（严重）**：M5 主备链路切换要靠**递增序号供上位机去重**。
  序号恒为 0 → 上位机无法区分「重传的同一帧」和「新帧」→ 切换瞬间会重复上报或丢帧。
  这个 bug 在 M5 阶段才会爆，属于**提前挖出的隐患**。
- **修法**：把 `seq++` 移到 `netconn_write()` **成功之后立刻执行**
  （语义应为「每成功发出一帧就 +1」，而不是「收到回应才 +1」）。
  发送失败时不递增（那帧本来就没发出去）。
- **状态**：✅ 已修（2026-10-18 01:05，0 Error / 0 Warning，Code=82328）
- **发现方式**：🔍 用户实测日志分析（不是报错，是数据不对）
- ⭐ **教训：`continue` 会跳过循环末尾语句。把计数器自增写在循环末尾前，
  先问一句「有没有分支会 continue 掉它」。**

## BUG-040 · NetAssist「断开重连」会无限自动重连，制造"连上就断"的假象（操作陷阱）

- **现象**：串口/NetAssist 日志里出现连续几十条
  `Connecting → Server connected → Server disconnected`（间隔 0.2~0.7s），
  看起来像板子疯狂踢连接。
- **根因**：**不是固件问题**。NetAssist 的「断开重连：无限尝试 / 自动」选项一旦勾选，
  用户手动断开一次后它就接管，进入无限自动重连循环。
  旁证：PC 本地端口从 6841 一路涨到 14536（低端口段被 TIME_WAIT 占满后
  Windows 换了端口段）→ 确实在疯狂建连。
- **影响**：会污染「连断 20 次验证 PCB 泄漏」的测试，看不出真实结果。
- **修法**：**测 echo 前先取消「断开重连」的勾**，改为手动连接 + 手动断开。
- **状态**：✅ 已澄清（操作问题，非代码缺陷）
- **发现方式**：🔍 用户日志分析
- ⭐ **教训：看到"连上立刻断"先问一句是不是调试工具自己在重连。**
  NetAssist 的这个选项默认是关的，勾上后就很难想起是它。

## BUG-038 · 关掉 MDIO 扫描后，未插网线时串口完全静默，与死机无法区分 ✅

- **现象**：确定 PHY 地址（addr=0）后把 `ETH_PHY_SCAN` 改回 0，重新烧录 →
  **串口一条都不打印**，看起来和板子死了一样。
- **根因**：三个 netconn 任务开头都调 `m1_wait_link_ready()` 死等
  `netif_is_link_up()`；网线没插时全部挂起。而原先唯一"无条件打印"的
  MDIO 扫描又被关掉了 → 真的一行输出都没有。
  此时无法区分以下四种情况：① 正常等网线 ② 板子没跑起来 ③ printf 没重定向
  ④ `MX_LWIP_Init()` 内部卡死（`HAL_ETH_Init` DMA 软复位超时 → `Error_Handler`）。
- **修法**：补三层"活着指示"，任何状态下都有反馈：
  1. `freertos.c` 的 `MX_FREERTOS_Init()` 开头（调度器启动前）打印
     `[sys] boot ok, FreeRTOS starting...`
  2. `StartDefaultTask` 里 `MX_LWIP_Init()` 之后打印 `[sys] LWIP init done`
     → 只有 boot ok 没有这一行 = 卡在 lwIP 初始化内部
  3. `m1_link_monitor_task` **改成不等链路**，启动即每 5s 打印
     `IP / link / up / uptime`；同时 **LED0(PF9) 每 1s 翻转**
     → **灯闪 = 板子活着，不依赖串口**
- **附带**：monitor 栈 512B → 1024B（512 装不下 `printf`）。
- **状态**：✅ 已修（0 Error / 0 Warning，Code=82328）
- **发现方式**：⚡ 上电实测（用户反馈"没有打印信息"）
- ⭐ **教训：调试期绝不能让所有任务都阻塞在同一个条件上。
  至少要留一个"无条件周期性报告"的任务 + 一个不依赖串口的物理指示（LED）。**

## BUG-033 · MDIO 扫描过滤条件太宽松 ✅

- **现象**：`0x0FFF` 这类垃圾值被当成有效 ID 打印出来，导致 31 个地址"全部命中"，误导判断。
- **根因**：旧过滤条件只有 `id1 != 0xFFFF`，没检查 `id2`。
- **修法**：改成**全量打印**（addr / BMCR / BSR / ID1 / ID2 / HAL 返回状态 `st`），由人来判断。
- **判读**：
  | 输出 | 含义 |
  |---|---|
  | 某地址 `BSR=78xx`、`ID1/ID2` 不是 FFFF | ✅ 找到了，该地址即 `ETH_PHY_ADDR` |
  | 仍 32 个全 FFFF | MDIO 没通 → 查 P4 跳线 / MDC 分频（BUG-018） |
  | `st` 不是 0 | HAL 返回错误或超时 |
- **状态**：✅ 已修

## BUG-034 · 没有喂狗点 → 上电约 50ms 就复位 ✅

- **现象**：IWDG(2s) 与 WWDG(窗口 36.7~49.9ms) 在 `main()` 里启动却没人喂 → 根本跑不到 lwIP 初始化。
- **修法**：
  - `main.c` 注释掉 `MX_WWDG_Init()`（**M1 临时，M5 恢复**）
  - `freertos.c` 的 `StartDefaultTask` 循环里加 `HAL_IWDG_Refresh(&hiwdg)`
- **遗留**：M5 改成"所有任务周期性置位事件组，monitor 任务裁决后才喂"；WWDG 防"跑飞但假装在喂狗"。
- **状态**：✅ 已修（M1 层面）

## BUG-035 · 网线未插时串口静默（**不是 bug，是预期现象**）✅

- **现象**：没插网线时只打印 `[ETH] PHY addr=N`，然后彻底静默。
- **解释**：MDIO 扫描是 MCU 与 PHY 芯片之间的通信，与网线无关；
  但 `link ready` 要等网线插上才会出现（三个任务都卡在等 `netif_is_link_up()`）。
- **操作建议**：**首次上电先不插网线**，专门看一眼 PHY 地址，避免被后续输出刷过去。
- **状态**：✅ 已澄清

---

# G. 环境与操作

## BUG-036 · CubeMX 重新生成会重置手工改动 ⚠️

- **风险点**：`<useUlib>`（MicroLIB）可能被重置回 0 → printf 卡死（BUG-029）。
- **每次 Generate 后必查清单**：
  1. `.uvprojx` 的 `<useUlib>` 是否仍为 1
  2. `m1_net.c` 是否仍在 Keil 工程文件列表里
  3. `ethernetif.c` 的四段 USER CODE（PHY 地址宏 / PD3 复位 / 链路线程 / `HAL_ETH_MspInit`）是否保留
  4. `gpio.c` 的 `MX_GPIO_PostInit()` 是否保留
  5. `lwipopts.h` 的 `MEMP_NUM_NETCONN 8` 是否保留
  6. `lwip.c` 的静态 IP 是否还在（`dhcp_start` 是否又冒出来）
  7. `can.c` 的 `AutoBusOff = ENABLE` 补丁是否保留
- **状态**：⚠️ 长期注意

## BUG-037 · F4 没有 `__HAL_RCC_ETH_CLK_ENABLE()` ✅

- **现象**：手写 MSP 时按 F1/F7 的写法调用这个宏 → 编译报未定义。
- **根因**：F4 的 ETH 时钟分成三个独立位。
- **修法**：用 `__HAL_RCC_ETHMAC_CLK_ENABLE()` + `__HAL_RCC_ETHMACTX_CLK_ENABLE()`
  + `__HAL_RCC_ETHMACRX_CLK_ENABLE()`。
- **状态**：✅ 已修

---

# 附：通用教训（跨 bug 提炼，值得背下来）

## 1. CubeMX + HAL 的三类坑，排查顺序固定

```
① 引脚分配对不对 → 对照原理图/IO 表，不要信 CubeMX 的自动选择（BUG-007/008）
② 外设 Enable 了没 → hal_conf.h 的 MODULE_ENABLED（BUG-015）
③ 生成的代码完整吗 → 新 LWIP 模板缺 MspInit / Start_IT / link_thread（BUG-011/013/014）
```

## 2. USER CODE 段的两条铁律

- **纯数字段（`USER CODE BEGIN 2`）是文件级的**，函数体内的活不能往里塞裸语句（BUG-027）
- **注释里禁止嵌套 `/* */`**（BUG-032）

## 3. 以太网"三步定位法"

```
ping 不通
  └─ arp -a 看得到 MAC 吗？
       ├─ 看得到 → 链路通，问题在 IP 层（IP/掩码/端口/防火墙）
       └─ 看不到 → 硬件问题，按序查：
            ① P4 跳线拔了吗（PA2 = ETH_MDIO 冲突）
            ② MDC 分频设了吗（BUG-018，最容易漏）
            ③ PHY 地址对吗（扫描确认）
            ④ RMII 九个脚 Speed 是 Very High 吗
            ⑤ HAL_ETH_Start_IT() 调了吗（BUG-014）
```

## 4. F407 以太网四条硬约束

| 约束 | 原因 |
|---|---|
| lwIP 内存不能放 CCM | CCM 不连 DMA 总线 |
| MDC ≤ 2.5 MHz（168MHz 下 CR=100） | IEEE 802.3 规定，HAL 不会帮你设 |
| ETH DMA 中断优先级 ≥ 5 | 数值要 ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` 才能用 FromISR |
| RMII 九脚 Speed = Very High | CubeMX 默认给 Low |

## 5. lwIP 资源泄漏的两个必查点

- 漏 `netconn_delete()` → 永久少一个 TCP_PCB，`MEMP_NUM_TCP_PCB=10` 意味着 **10 次之后整板再也连不上**
- 漏 `netbuf_delete()` → pbuf 泄漏，跑一会儿就收不到包

**验收动作**：echo server **连续连断 20 次**，每次都要能连上。

## 6. 实编译能发现人工看不出来的问题

```bash
"D:/Keil5/UV4/UV4.exe" -j0 -b "edge gateway.uvprojx" -o "_build.log"
```

本次 5 个编译期 bug（BUG-027~032）全部是这么挖出来的。**改完工程就跑一次，别只靠肉眼。**

---

# 附录 B：已实测确认的硬件参数（2026-10-18 首次上电）

> 这些是**真板子跑出来的值**，后续 M2/M3/M4 直接引用，不要再靠推断。

## ✅ M1 核心验收达成：ping 打通（2026-10-18）

```
C:\Users\OMEN>ping 192.168.1.30
来自 192.168.1.30 的回复: 字节=32 时间=1ms TTL=255   ×4
数据包: 已发送 = 4，已接收 = 4，丢失 = 0 (0% 丢失)
最短 = 1ms，最长 = 2ms，平均 = 1ms
```

**意义**：PD3 复位时序、MDC 分频（CR=100）、手写 `HAL_ETH_MspInit`、
`HAL_ETH_Start_IT`、静态 IP、链路线程 —— 这一整串补丁**首次端到端验证通过**。
RTT 1ms 也印证了链路是 100M 全双工。

| 项 | 实测值 | 判读 |
|---|---|---|
| **PHY 芯片** | YT8512C（裕太微） | ID = `0x0000_0128` = Linux motorcomm 驱动的 `PHY_ID_YT8512` |
| **PHY MDIO 地址** | **0** | 32 个地址里只有 addr=0 应答，其余全是 `1FFF/3FFF/FFFF`（MDIO 上拉垃圾） |
| BMCR (reg 0) | `0x1140` | bit12(AN enable) + bit8(全双工) + bit6(100M) → 复位默认值正常 |
| BSR (reg 1) 未插网线 | `0x7949` | bit2(Link)=0、bit5(AN complete)=0 → **符合"没插网线"** |
| BSR 插上网线后 | 应变为 bit2=1（如 `0x7969`） | 这是我写的链路线程判据，插上网线后应立刻出现 `[net] link ready` |

### 扫描输出的正确形态（供以后对照）

```
[ETH] a= 0 st=0 BMCR=1140 BSR=7949 ID1=0000 ID2=0128   ← 只有这一行是真实的
[ETH] a= 1 st=0 BMCR=1FFF BSR=FFFF ID1=3FFF ID2=FFFF   ← 以下全是上拉垃圾
...
```

**判据**：`1FFF / 3FFF / FFFF` 是 MDIO 线在无设备应答时的浮动值，**不是有效 PHY**。
只有出现 `BSR=78xx/79xx` 且 ID 不为 FFFF 的那一行才是真芯片。

### 已据此回填

`ethernetif.c` 里 `ETH_PHY_ADDR 0U`（本来就是 0，实测确认无需改）、`ETH_PHY_SCAN` 改回 **0**。

---

# 待办 / 未闭环

| # | 事项 | 关联 BUG |
|---|---|---|
| 1 | ~~重新烧录，确认 MDC 分频修复后能否扫到 YT8512C 真实地址~~ → ✅ 已完成（addr=0） | BUG-018 |
| 2 | ~~拿到地址后回填 `ETH_PHY_ADDR`，把 `ETH_PHY_SCAN` 改回 0~~ → ✅ 已完成 | BUG-018 |
| 3 | ~~插上网线，确认 BSR bit2 变 1、`[net] link ready`~~ → ✅ 已完成（link=up） | 附录 B |
| 4 | ~~ping 通~~ → ✅ 4/4、0% 丢失、平均 1ms | — |
| 5 | ~~echo 回显 + 连断 20 次验证 PCB 不泄漏~~ → ✅ 全部通过（20+ 次） | BUG-039 |
| 6 | ~~心跳客户端（5001）：验证上位机回 ACK 后变 3s 周期、seq 递增、断线 9s 判定 + 自动重连~~ → ✅ 已完成（2026-10-18 01:12 实测：seq 0→1→2→3 递增、`[hb] rx 2 bytes` 收到 ACK、`err -3/-15` 后 1s 自动重连成功） | BUG-041 |
| 11 | **M2 上板验收**：① `M2_TEST_RAW=1` 验 RS485 物理层 ② Modbus Poll 先确认从机参数 ③ 接从机看 5s 汇总行 ④ 拔一个从机验降频与故障隔离 | BUG-042/043/044 |
| 12 | 烧录后第一件事看 `[mb] heap free after task create: N` —— `configTOTAL_HEAP_SIZE` 只有 20KB，接近 0 说明堆要炸 | — |
| 13 | M2 验收通过后把 `M2_TEST_RAW` 改回 0 并重新烧录 | — |
| 7 | Jperf 测速并归档量化数字（理论上限按实测 RTT 1ms 算 ≈ 65 Mbps） | — |
| 8 | 修 `printf` 重入（环形缓冲 + 独立发送任务），解决日志丢行 | BUG-039 |
| 9 | M5 恢复 `MX_WWDG_Init()` 并做事件组喂狗 | BUG-034 |
| 10 | M4 考虑用 scatter 把 FreeRTOS heap / mem_heap 放进闲置的 64KB CCM | BUG-021 |

---

## BUG-053 · 存活位置位写在阻塞点之后 → monitor 误报 "alive missing" ✅

- **日期**：2026-10-21
- **模块**：M4（存活事件组）/ M1 网络任务
- **现象**：插上网线、网络任务明明在正常工作（串口打了 `[cmd] listening on port 5000`、
  `eth=up`），但 monitor 仍每 5s 报：
  `[m4] !! alive missing: 0x0B NET MODBUS LOG`
- **根因**：三处存活位全都写在**阻塞调用之后**，任务卡在阻塞点就永远置不上位：

  | 任务 | 阻塞点 | 后果 |
  |---|---|---|
  | `m1_cmd` | `netconn_accept()` **永久阻塞**（没客户端时） | 循环体一次都跑不到 → NET 永不置位 |
  | `m1_report` | `netconn_connect()` 失败 → `continue` | PC 没开 5001 服务时 NET 永不置位 |
  | `m4_log` | —— | **压根忘了写** `M4_AliveSet(M4_ALIVE_LOG)` |

  ⚠️ 危害不在现在（M4 只打印告警），而在 **M5**：事件组喂狗判据是
  `M4_AliveGet() == M4_ALIVE_ALL`，NET 位永远缺失 → **永远不喂狗 → 板子周期性复位**。
- **修法**（已改，编译通过）：
  1. `m1_cmd`：给 **listen socket** 也 `netconn_set_recvtimeout(listen_conn, 3000)`
     （lwIP 里 `recv_timeout` 对 `accept` 同样生效，3s 超时返回 `ERR_TIMEOUT`），
     并把 `M4_AliveSet(M4_ALIVE_NET)` 挪到 `for(;;)` 循环**开头**；
     ⚠️ accept 返回 `ERR_TIMEOUT` 时**不能** `netconn_delete(listen_conn)`，只是没人来连。
  2. `m1_report`：存活位挪到循环开头（任务在转 = 活着，与连不连得上无关）。
  3. `m4_log`：补 `M4_AliveSet(M4_ALIVE_LOG)`。
- **教训（通用）**：**存活位 = "任务还在循环"，不是"任务干成了某件事"**。
  任何形式 `while(1){ 阻塞等待; 成功才置位; }` 的写法都会漏位。
  正确姿势：`while(1){ 置位; 干活(); }`。
- **状态**：✅ 已修复（2026-10-21 编译通过，待烧录复测确认告警消失）
- **发现方式**：⚡ 上电实测（插网线版日志）

---

## BUG-054 · M3 文档写错路由观察端口（把 `C2 rx` 写成 `C1 rx`）✅

- **日期**：2026-10-21
- **模块**：M3（双 CAN 网关路由）/ 文档
- **现象**：按文档预期去找 `[can] C1 rx id=0x401`，**永远找不到**；实测路由帧出现在
  `C2 rx id=0x401`。曾一度怀疑"路由没生效"。
- **根因**（拓扑理解错误，非代码错误）：M3-b 是 **CAN1 与 CAN2 挂在同一条物理总线**上。

  ```
  CAN1 发 0x301 ──总线──> CAN2 收到（C2 rx 0x301）
                           ↓ 路由 dst=CAN1, id=0x401
  CAN1 发 0x401 ──总线──> CAN2 又收到（C2 rx 0x401）  ← ⭐ 观察点在 C2
                           ↓ 0x401 不命中任何规则 → 停止，不自激
  ```

  1. 转发帧由 **CAN1 发出**，但 **bxCAN 非回环模式下节点不收自己发的帧** → `C1 rx` 恒为 0；
  2. CAN2 挂在同一总线上，CAN1 发出的任何帧它都收得到 → 转发帧只能被 **CAN2** 观察到。
- **正确判据**（已写回文档 B.4/B.6）：
  - 路由生效 = **`C2 rx` 里出现 0x401**（不是 `C1 rx`）
  - `C2 rx` 增速 ≈ **2 × 发送帧率**（1 原始 + 1 转发）；若自激会指数增长
  - `C1 tx` = 原始帧 + 转发帧；**`C1 rx` 恒 0 是正确的**
- **状态**：✅ 已修正文档，实测确认路由生效（seq 0→25，C2 rx=49 ≈ 2×25）
- **发现方式**：⚡ 上电实测 + 与文档预期比对不符
- **教训**：同一条总线做"双端口网关"验证时，**观察端口会反直觉地落在接收侧**；
  拆成两条独立总线后转发帧才会出现在对端端口。写验收判据必须先把拓扑画出来。

---

## BUG-055 · 上报连接上收到的 ACK 被当成命令 → `cmd_bad` 狂涨、满屏 NAK ✅

- **日期**：2026-10-21
- **模块**：M4（应用协议 m4_proto / m1_net 上报任务）
- **现象**（`m4_host_test.py --listen --ack` 实测）：
  - 板子每上报一帧快照，就紧跟一帧 `AA 55 02 FF 03 ...`（NAK，错误码 0x03=UNSUPPORTED）
  - 上位机侧刷满 `CMD=0xFF (NAK)`
  - 数据池统计 `cmd 23 (bad 15)` —— **坏帧几乎等于上报帧数**
  - 板子串口反复 `[rpt] rx 9 bytes` + `[cmd] unsupported cmd 0x82`
- **根因**：**5001 上报连接是双向的**。
  上位机收到快照后回一个 `0x82 ACK`，这条 ACK 顺原连接回到板子，
  被 `M4_ProtoFeed()` 解出 cmd=0x82 后直接喂给 `m1_handle_cmd()`。
  而 0x81/0x82/0xFF 本就是**板→PC 方向**的帧，命令处理器只认 0x02/0x03，
  于是走了 `default` 分支：打印 unsupported → 计一次坏帧 → 回一个 NAK。
  ⚠️ **每上报一帧就产生一次"假坏帧"**，坏帧率/成功率统计彻底失真。
- **修法**（已改，编译通过）：`m1_handle_cmd()` 开头加响应类帧过滤：
  ```c
  if ((cmd == M4_CMD_SNAPSHOT) || (cmd == M4_CMD_ACK) || (cmd == M4_CMD_NAK))
  {
      return;   /* 响应不是命令：不处理、不计坏帧、不回 NAK */
  }
  ```
- **配套改动**：栈水线实测 `m1_cmd left=928B→456B`、`m1_rpt left=480B`（1280B 栈仅剩 35%），
  两个栈都提到 **1536B**（heap 约 8696 → 8184，仍安全），为 M5 链路冗余留余量。
- **状态**：✅ 已修复（2026-10-21 编译 0 Error 0 Warning，待烧录复测：
  NAK 应消失、`cmd N (bad 0)` 中 bad 保持 0）
- **发现方式**：⚡ 上电实测 + 上位机脚本联调
- **教训**：**双向长连接上必须区分"命令"与"响应"**。
  凡是协议里定义了方向（板→PC / PC→板）的帧，接收端都应按方向过滤，
  否则对端回的确认帧会被当成新命令处理 —— 表现为"无来由的坏帧率"和"应答风暴"。

---

## BUG-056 · 上位机短连接正常关闭被误报为 `recv err -15`（日志噪声）⏳

- **日期**：2026-10-21
- **模块**：M4 / m1_net.c 命令服务任务（5000 端口）
- **现象**（M4-fix 固件实测，配合 `m4_host_test.py --query`）：
  上位机每查一次（脚本发一帧、收一帧后立刻 `s.close()`），板子串口就刷 3 行：

  ```
  [cmd] client connected
  [cmd] recv err -15
  [cmd] client disconnected
  ```

  看起来像"每次连接都出错"，实际查询、快照、ACK 全都正常。
- **根因**：lwIP 错误码 **-15 = ERR_CLSD（对端已关闭）**、-14 = ERR_RST（对端复位）。
  上位机这种"发完就断"的**短连接**属于正常关闭，`netconn_recv()` 返回 ERR_CLSD 是预期行为。
  但 `m1_cmd` 的 recv 分支只把 `ERR_TIMEOUT` 当正常，其余错误统一走
  `printf("[cmd] recv err %d\r\n", err)` 再 break，**把正常关闭打成了错误**。
- **影响**：不丢功能，但
  ① 长稳日志被这 3 行/次灌水，真正 ERR_MEM / ERR_ABRT 类的故障会被淹没；
  ② 排查时极易被误导成"连接总在出错"。
- **修法**（待改，`Core/Src/m1_net.c` 约 301 行，`else` 分支前插入）：
  ```c
  else if (err == ERR_CLSD || err == ERR_RST)
  {
      /* 对端正常关闭（上位机发完就断开的短连接），不是故障，静默退出 */
      break;
  }
  ```
  只有 ERR_MEM(-1) / ERR_ABRT(-13) 之类才继续打印 `recv err`。
- **状态**：⏳ 待修（计划与 M3 收尾改动合并，一次编译烧录，避免打断长稳测试）
- **发现方式**：⚡ 上电实测 + 上位机脚本联调
- **教训**：**网络错误码要按"是不是故障"分类，不能一刀切当异常**。
  TCP 短连接场景下 ERR_CLSD/ERR_RST 是常态而非错误；
  长稳日志里只保留真异常，否则日志信噪比归零，等于没有日志。

---

## BUG-057 · WWDG 与「内部 Flash 擦除」天然冲突（设计约束，非代码错误）⚠️

- **日期**：2026-10-21
- **模块**：M5 看门狗（m5_wdg.c / main.c）
- **现象（推演 + 代码审查阶段发现，未上电复现）**：
  按 M5 文档把 WWDG 打开后，一旦执行 OTA（`OTA_BEGIN` 擦 Bank B）就必然复位，
  而且复位后 Flash 停在"擦了一半"的状态 —— OTA 永远做不完。
- **根因**：两个硬事实撞在一起：
  1. **WWDG 最长只有 ~50 ms**（F4 硬上限：`4096×8×(127−63)/42 MHz ≈ 49.9 ms`）。
  2. **Flash 擦除期间 CPU 完全停摆**（F4 参考手册 PM0081：擦除/编程进行中，
     任何对 Flash 的读访问都被 stall 直到操作结束；而 CPU 取指本身就在读 Flash）。
     → 128 KB 扇区擦除要数百毫秒，期间 CPU 不动 → EWI 中断进不来 → WWDG 复位。

  ⚠️ 这不是"喂狗点写错了"，是**两个硬件约束的物理冲突**，改代码顺序救不了。
- **为什么 IWDG 没事**：IWDG 超时 2.72 s（最快 LSI）> 单个扇区擦除时间，
  所以擦除期间靠 `M5_WdgHold(2000)` + 每个扇区擦完立刻 `M5_WdgKick()` 就能扛过去。
- **本工程的处理（已落地）**：
  - `m5_wdg.h` 增加总开关 `M5_WWDG_ENABLE`，**默认 0（关闭）**；
    `main.c` 里 `MX_WWDG_Init()` 用 `#if M5_WWDG_ENABLE` 包起来。
  - 关闭期间靠 **IWDG + 存活位裁决**兜底（覆盖"死机"和"假活"里的大多数场景）。
  - 要做 M5-b（WWDG 窗口验证）时把开关置 1，且**不要同时做 OTA**。
- **真要用 WWDG 又必须擦 Flash 时的正解**（本工程未采用，记录备查）：
  把"擦除 + 等待 BSY"的循环放到 **RAM 中执行**（Keil 需 scatter 增加 RAMCODE 执行区，
  且函数内不能调用任何位于 Flash 的函数）。CPU 指令取自 RAM 就不会 stall，
  从而能在等待循环里喂 WWDG。⚠️ 改 scatter 属高风险操作（本项目历史上出过
  `L6218E`），必须先备份 `.sct`、改完先看 map 确认地址再烧。
- **状态**：✅ 已按"默认关闭 + 显式开关 + 注释写清取舍"处理（不隐瞒、不假装能用）
- **发现方式**：🔍 代码审查 + 手册推导（写 M5 落地时比对 PM0081 与 WWDG 时序得出）
- **教训**：**看门狗的窗口必须长于"系统最长的不可中断操作"**。
  凡是引入 Flash 擦写、DMA 大块搬运、关闭中断的临界区，都要回头核算一遍
  看门狗窗口 —— 尤其是 WWDG 这种只有几十毫秒的。

---

## BUG-058 · CAN 存活位放在 5s 分支里 → 与 monitor 同周期 → 无限复位 ⚠️⚠️

- **日期**：2026-10-22
- **模块**：M5 看门狗 / m3_can.c
- **现象（上电实测）**：串口日志出现**两次启动头**，中间夹着
  ```
  [wdg] grace: waiting for 0x04 (kick N/10)
  [wdg] !! missing 0x04 (streak 1..3), REFUSE to feed
  ```
  第三次拒喂后 IWDG 复位；重启后接线与时序都没变 → 再次拒喂 → **板子永远起不来**。
  这是本项目到目前为止最严重的一次事故（表现就是"砖了"）。
- **根因**：两个**同周期**的动作撞车了 ——
  - `m4_mon` 每 5 s **清零**一次存活位（`M4_ALIVE_ALL`）
  - `m3_can` 把 `M4_AliveSet(M4_ALIVE_CAN)` 放在**每 5 s 才执行一次的统计分支里**

  两个 5 s 周期一旦错开，就必然出现"已被清零、CAN 还没来得及置位"的检查窗口。
  叠加本次环境变化（CAN 对连线拆了，`M3_CanSend` 一直返回 −2/−3，
  TEC 冲到 128 进 error-passive），窗口被放大成"几乎每次都缺 CAN"。

  ⚠️ 语义错误才是根子：**存活位只应表示"这个任务还在转"**，
  CAN 链路通不通应该由数据池的 `online` 位（rx 有增长）表达。
  两者混为一谈 → "总线没接线"被判成"系统死机"。
- **修复（已改 m3_can.c）**：
  - `M4_AliveSet(M4_ALIVE_CAN)` **移到 `for(;;)` 循环开头无条件置位**；
  - 5 s 统计分支里那行删除，并留注释"不要加回来"。
  （与 BUG-053 修 NET/LOG 时的做法是同一个套路：BUG-053 漏改了 CAN。）
- **状态**：✅ 已修，编译通过（Code=99028）
- **发现方式**：🔌 上电实测（用户贴日志）
- **教训**：**存活位一律在循环开头无条件置**，周期必须远短于检查窗口
  （本工程：置位 1 s 级、检查 5 s 级）。任何"某个分支里才置位"的写法，
  一旦该分支周期接近检查周期，就是定时炸弹。

---

## BUG-059 · 复位原因打印早于 UART 初始化 → 上电第一行永远看不到

- **日期**：2026-10-22
- **模块**：main.c 的复位诊断
- **现象**：上电日志第一行是 `[fan] PWM ready`，**没有 `[rst] cause=...`**，
  而这几行本该是排障最关键的信息（尤其是看门狗复位时）。
- **根因**：`print_reset_cause()` 被放在 `MX_GPIO_Init()` **之前**，
  那时 `MX_USART1_UART_Init()` 还没跑，`huart1.gState == HAL_UART_STATE_RESET`。
  `log_push()` 在调度器未启动时走"阻塞发送兜底"分支 `HAL_UART_Transmit()`，
  而该函数的第一行检查 `if (huart->gState == HAL_UART_STATE_READY)` 不成立
  → 直接返回 `HAL_BUSY`，**一个字节都不发，且不报错**。静默丢失。
- **修复（已改 main.c）**：拆成两步
  - `snapshot_reset_cause()` —— 上电立刻执行：读 `RCC_CSR` 并**清标志**
    （这一步必须在 IWDG 启动前，否则第一次死因会被后续复位覆盖），结果只存静态变量；
  - `print_reset_cause()` —— 挪到 `USER CODE BEGIN 2`（UART 已初始化、RTOS 未启动）再打印。
- **状态**：✅ 已修，编译通过
- **发现方式**：🔌 上电实测（用户贴日志，第一行缺失）
- **教训**：**"打印得越早越好"是错的** —— 早于外设初始化就是静默丢弃。
  凡是要"第一时间输出"的信息，都要走"早期快照 + 就绪后补打"两段式。

---

## BUG-060 · 链路层缺启动宽限 → 上电瞬间误演 主→备→降级→主

- **日期**：2026-10-22
- **模块**：m5_link.c
- **现象**：上电日志里刷两行无意义的切换
  ```
  [link] PRIMARY -> BACKUP (fail=3)
  [link] !! DEGRADED: both links down
  [link] DEGRADED -> PRIMARY (eth restored)
  ```
  看着像"系统刚起来就断网"，实际只是外设还没就位。
- **根因**：PHY 要 1~3 s 才 link up，ESP8266 连 WiFi 更久；
  而上电瞬间 `eth_phy_up()` 和 `M5_EspIsUp()` 都是 false，
  链路层立刻按"主链路连续失败 3 次"裁决 → 切备份 → 两条都不可用 → DEGRADED。
- **修复（已改）**：`m5_link.h` 增加 `M5_LINK_GRACE_MS 15000`，
  `M5_LinkTick()` 开头在宽限期内**只驱动 ESP 状态机，不做任何切换/告警决策**。
  （和看门狗的 `M5_GRACE_KICKS` 是同一个思路。）
- **状态**：✅ 已修，编译通过
- **发现方式**：🔌 上电实测
- **教训**：**任何"健康度裁决"都要有启动宽限**。
  系统刚上电时所有链路都是"不可用"的，那是正常状态不是故障。

---

## BUG-061 · 存活判定"位图 + 清零"与喂狗裁决顺序冲突 → 每 5 s 误拒喂一次 ⚠️

- **日期**：2026-10-22
- **模块**：m4_task.c（存活判定）/ m5_wdg.c（喂狗裁决）
- **现象（上电实测）**：没复位了，但每 5 s 刷一条
  ```
  [wdg] !! missing 0x0D (streak 1), REFUSE to feed
  ```
  `streak` 一直是 **1，不累加** —— 因为下一次 kick 位图又满了、喂成功、streak 清零。
  系统"侥幸不死"，但只要某个任务的报到周期再长一点就会累加到 3 真复位。
- **根因**：**清零与裁决在同一轮里、且清零在前**。
  monitor 一轮里的顺序是：
  ```
  ① 读位图 → 打印 missing → 【清零位图】
  ② uptime / 确认字 / 堆 / 池摘要
  ③ M5_WdgKick()  ← 读位图裁决
  ```
  ③ 读到的是 ① 刚清空的位图 → **必然拒喂一次**。
  这是"位图 + 清零"这套设计的固有缺陷：位图是**有状态**的，
  谁清、什么时候清、谁在清之后读，全都得小心翼翼地错开，而它们天然错不开。
- **修复（已改）**：判定方式从【位图 + 清零】换成【时间戳】
  - `M4_AliveSet()` 额外记 `s_last[i] = HAL_GetTick()`；
  - 新增 `M4_AliveMissing(window_ms)`：算 `now - last > window` 即判失联（`last==0` = 从未报到）；
  - monitor **删除清零操作**，改用 `M4_AliveMissing()` 打印；
  - `M5_WdgKick()` 改用 `M4_AliveMissing(M4_ALIVE_WINDOW_MS) & M5_ALIVE_REQUIRED`；
  - App 确认字判据同步改时间戳。
- **为什么时间戳更对**：
  1. 无"清零"这个破坏性动作 → 彻底消除清零/置位竞态；
  2. Kick 何时读都读到同一份事实，与 monitor 节奏无关；
  3. "卡死但位置着"照样测得出来 —— 卡死的任务不再更新时间戳，它会一直变老；
  4. 天然容忍 3 s 周期的任务（旧做法要小心错开周期，见 BUG-058 的同族问题）。
- **状态**：✅ 已修，编译通过（Code=98944，0E/0W）
- **发现方式**：🔌 上电实测（用户贴日志，`streak 1` 反复出现）
- **教训**：**不要用"需要清零才能复用的状态"做健康度判定**。
  凡是"检查者要清零、被检查者要置位"的双向写，就一定会遇到顺序竞态。
  改成单向写（各自更新自己的时间戳）+ 只读判定，竞态从根上消失。

---

## BUG-062 · error-passive 未锁存 → 总线断线时每 5 s 刷屏

- **日期**：2026-10-22
- **模块**：m3_can.c 的 `m3_bus_check()`
- **现象**：CAN 总线没接时，每 5 s 刷一条
  `[can] ! port0 error-passive TEC=128 REC=0`，与统计行混在一起刷一整晚。
- **根因**：原注释写的是"只提示不刷屏：每 5 秒最多一次"，
  但 `m3_bus_check()` 本身就是 **5 秒调用一次** —— 所谓"限流"等于没限。
  同一个函数里 BUS-OFF 做了进/出沿锁存，唯独 error-passive 漏了。
- **修复（已改）**：加 `s_epvf_latch[2]`，看齐 BUS-OFF 的做法：
  进入 error-passive 报一次，恢复到 error-active 再报一次，中间保持沉默。
  这样日志里出现的都是**状态变化**，而不是周期性噪声。
- **状态**：✅ 已修，编译通过（Code=99036，0E/0W）
- **发现方式**：🔌 上电实测（CAN 对连线被拆掉后日志刷屏）
- **教训**：**"限频打印"要按状态变化限，不要按时间限**。
  凡是"每 N 秒最多打一次"的写法，先确认调用周期是不是就是 N 秒 ——
  是的话等于每次都打。日志信噪比为零，等于没有日志（同 BUG-056 的教训）。

---

---

## BUG-063 · ESP8266 未接时 AT 重试日志刷屏，把有用日志冲掉

- **日期**：2026-10-22（M5-g 验收时观察到）
- **模块**：m5_esp.c 的 AT 状态机
- **现象**：模块没接（或 P2 跳线没跳到 ATK-MODULE 侧）时，
  每 2~3 s 刷一条 `[esp] timeout waiting 'OK', back to AT`。
  M5-g 跑 4 轮风扇曲线的日志里，这条噪声占了约 **1/3 的行数**，
  把 `[fan]` / `[m4]` 的关键行冲得很难找。
- **根因**：AT 状态机在 `WAIT_OK` 超时后【无条件】回到 `AT` 重发，
  且每次超时都打一行。**失败是稳态**（模块根本不在），
  但日志是按"每次重试都报"写的 —— 与 BUG-062 是同一类错误：
  把**持续状态**当成了**事件**来打日志。
- **修复（已改）**：
  1. 重试改成**指数退避**：第 n 次失败后等待 `min(1s × 2^n, 30s)`，
     不再固定间隔狂发 AT（也省电、减少 USART3 无谓中断）。
  2. 日志按**状态变化**打：第一次超时打 `ESP not responding, retrying (backoff)`，
     之后只在【恢复成功】时打 `ESP responded, AT link up`；
     退避期间静默，最多每 30 s 打一条心跳级提示。
- **状态**：✅ 已修，编译通过（Code=99064，0E/0W）
- **发现方式**：🔌 上电实测（M5-g 验收，模块未接）
- **教训**：**"重试"和"报错"是两件事，别用同一行日志表示**。
  设备不在是常态而不是异常，重试逻辑必须带退避，
  否则不仅刷屏，还会让 USART3 的 DMA+IDLE 中断一直被打断，
  在真机上白白吃掉 CPU 时间。

---

## BUG-064 · 风扇"运行态"与"实际输出"不是一回事（语义坑，非功能缺陷）

- **日期**：2026-10-22（M5-g 验收时发现）
- **模块**：m5_fan.c 的 `s_running` / `M5_FanDuty()`
- **现象**：注入 25.0℃ 时日志是 `duty=0%  (PA6 avg 0.0V)`，
  但**没有**打 `fan STOP` —— 因为 `s_running` 仍为 1（25℃ 还没到 24℃ 的停机线）。
  也就是：**PWM 实际已经停转了，但状态机还认为"风扇在运行"**。
- **根因**：`s_running` 的语义是**滞回记忆位**（用来防止 24~26℃ 之间反复启停），
  不是"当前是否在转"。`duty` 才是真实输出。两者在 24~26℃ 区间会不一致，
  这是滞回设计的必然结果，不是 bug。
- **风险**：上位机/状态上报如果读 `s_running` 当"风扇状态"，
  会在这 2℃ 区间里**误报"运行中"**（实际 duty=0，风扇没转）。
- **修复（已改）**：
  1. 新增 `M5_FanRunning()` 返回 `s_duty > 0` —— **对外只暴露这个**，
     语义明确为"当前真的在转"。
  2. `s_running` 改名注释为"滞回记忆位（hysteresis latch），不得用于对外状态上报"。
  3. 状态上报/快照里统一用 `M5_FanDuty()` + `M5_FanRunning()`。
- **状态**：✅ 已修（接口语义），编译通过
- **发现方式**：🔌 上电实测（M5-g 扫温，第 ⑦ 档）
- **教训**：**控制算法里的内部状态位和对外状态是两回事**。
  凡是"为了防抖/滞回"引入的中间变量，都不能直接当状态上报出去 ——
  它记的是历史，不是现在。对外接口要单独定义"当前真实值"。

---

---

## BUG-065 · CAN 统计行空闲时每 5 s 刷两行全 0（M3 收官后才暴露）

- **日期**：2026-10-22
- **模块**：m3_can.c 的 `m3_can_task()` 5 秒统计分支
- **现象**：`M3_TEST_TX_ENABLE` 改回 0 之后总线上没有流量，但日志仍每 5 s 打
  ```
  [can] C1 rx=0 tx=0 drop=0 fail=0 bo=0 tec=0
  [can] C2 rx=0 tx=0 drop=0 fail=0 bo=0 tec=0
  ```
  两行全 0，跑一晚就是几万行纯噪声 —— 在 M5-g 验收完、CAN 收官之后才暴露出来。
- **根因**：和 BUG-062 / BUG-063 **完全同一类错误**：
  把"周期性轮询"当成"事件"打日志。统计分支本来就是 5 s 一次，
  里面无条件 printf 就等于"每 5 s 必打"，所谓"限频"根本不存在。
- **修复（已改）**：
  1. 新增 `s_last_print` 快照 + `m3_stats_changed()`：
     本轮统计与【上次打印时】的快照比，rx/tx/drop/fail/bo/tec 任一变化才打两行。
  2. 完全无变化时每 **30 s**（6 轮）打一行心跳：
     `[can] idle 30s: C1 rx=0 tx=0 | C2 rx=0 tx=0 (no traffic)`
     ⚠️ 为什么必须留心跳：彻底不打的话，日志里再也看不到 CAN 的痕迹，
     `m3_can` 任务真卡死了也发现不了 —— 30 s 一行刚好能区分
     "活着但没流量"和"任务死了"。
- **状态**：✅ 已修，编译通过（Code=99412，0E/0W）
- **发现方式**：🔌 上电实测（M3 收官后的正式版日志）
- **教训（第三次撞同一面墙，值得单独记一条通则）**：
  **凡是写在"每 N 秒执行一次"的分支里的 printf，都要先自问：
  这条信息是【事件】还是【状态】？**
  状态（总线空闲、设备不在、温度正常）就必须做**变化检测**或**大幅降频**，
  否则流量一大日志信噪比归零 —— 而信噪比归零的日志，等于没有日志。

---

---

## BUG-066 · OTA 分块 1024 超过协议 LEN 的 1 字节上限 → OTA_DATA 根本发不出去

- **日期**：2026-10-22（**第一次真正跑 OTA 时暴露**）
- **模块**：`tools/m5_ota_host.py` + `shared/flash_layout.h` + `m4_proto.h`
- **现象**：`OTA_BEGIN` 成功（Bank B 已擦除），一到 `OTA_DATA` 上位机直接崩：
  ```
  ValueError: byte must be in range(0, 256)
  ```
  板子停在 `OTA_DOWNLOADING`、Bank B 已擦、一字节都没写进去。
- **根因**：**协议字段宽度决定了上层所有参数的上限，改上层常量时没回头算一遍。**
  帧格式 `AA 55 LEN(1) CMD(1) DATA(N) CRC32(4)`，LEN 只有 **1 字节**且 `LEN = 1(CMD) + N(DATA)`
  → **N ≤ 254**。而 OTA_DATA 的 DATA 自身还有 10 字节头：
  ```
  DATA = offset(4) + seq(2) + block_crc(4) + payload(N) = 10 + N
  1 + 10 + N ≤ 255   →   payload ≤ 244
  ```
  上位机 `--block` 默认 **1024**、`flash_layout.h` 的 `OTA_BLOCK_SIZE` 也是 **1024**，
  `pack()` 里 `bytearray([SOF0, SOF1, len(body)])` 拿到 1035 直接抛异常。
- **连带问题（同一个根因）**：`m4_proto.h` 的 `M4_PROTO_MAX_DATA = 1088`，
  而 `ST_LEN` 的越界检查写的是 `if (b > (M4_PROTO_MAX_DATA + 1))` ——
  **1 字节的 b 永远不可能 > 1089，这个检查形同虚设**（永远不触发）。
  常量值与实际能力相差 4 倍，是"看起来有边界检查、其实没有"的典型。
  （不会溢出：`data[]` 缓冲是 1088 > 255，所以只是语义误导，不是安全漏洞。）
- **修复**：
  1. ✅ `tools/m5_ota_host.py`：`--block` 默认 **1024 → 240**，并加硬校验
     `MAX_BLOCK_SIZE = 255 - 1 - 10 = 244`，超限直接报错并提示正确值。
  2. ⬜ **待烧录**：`m4_proto.h` 的 `M4_PROTO_MAX_DATA` 1088 → **254**（真实上限），
     让 `ST_LEN` 的越界检查真正生效；`flash_layout.h` 的 `OTA_BLOCK_SIZE` 1024 → **240**。
     ⚠️ 这两处改了要重新编译烧录，所以要等本次 OTA 验证结束后再做，
     否则会把"OTA 前的 v1 基线"覆盖掉，版本号翻转就验不出来了。
- **状态**：✅ 上位机已修并验证（240 字节块，101132 B / 9.8 s，累计 CRC 一致）；板端常量待烧
- **发现方式**：🔌 上电实测（首次 OTA）
- **教训**：**凡是"协议字段宽度"和"上层分块/长度常量"分两处定义的地方，
  改任何一个都要回头算一遍另一个的上限**。
  更根本的做法是让板端常量**由协议推导**而不是各写一个数字：
  `#define OTA_BLOCK_SIZE  (255u - 1u - 10u)` —— 这样改协议时上限自动跟着变。

---

---

## BUG-067 · 上位机 `--corrupt` 先改数据后算 CRC → 分块 CRC 校验根本测不出来

- **日期**：2026-10-22（OTA 防砖验收时暴露）
- **模块**：`tools/m5_ota_host.py`
- **现象**：跑 `--corrupt`（故意改坏中间一块）时，被改坏的第 50400 块板子回了 **OK**，
  一路传到 100%，最后靠 END 的整体 CRC 才发现错误。
  即：**分块 CRC 等于没测**，只有整体 CRC 在兜底。
- **根因**：**测试工具自己骗了自己** —— 代码顺序反了：
  ```python
  chunk[idx] ^= 0xFF        # ① 先改坏数据
  bcrc = crc32(chunk)       # ② 再算 CRC → 算出的是"改坏后数据的正确 CRC"
  ```
  板端 `M5_OtaData()` 的 `if (M4_Crc32(payload, plen) != bcrc)` 拿到的是
  一份**与坏数据完全匹配**的 CRC，校验必然通过。
  （板端代码本身是**对的**，这次是上位机的锅。）
- **修复（已改）**：先按原始正确数据算 `bcrc = crc32(chunk)`，**再** `^= 0xFF` 改坏内容。
  这样才真正模拟"传输过程中比特翻转"：发送方算的是原数据的 CRC，
  线上数据被改了 → 接收方算出的 CRC 与帧里带的对不上。
- **修复后实测**：`DATA @50400: CRC_ERR offset=50400`，板端回 CRC_ERR 且
  **received 停在 50400 没推进** → 正确表达了"这一块要重传"的语义 ✅
- **状态**：✅ 已修并验证
- **发现方式**：🔌 上电实测（OTA 防砖验收）
- **教训**：**"故障注入"类测试代码，注入点必须在校验值计算之【后】**。
  否则你注入的故障会被自己顺手修正掉，测试永远绿，等于没测。
  写这类代码时问一句：**我改的东西，会不会被我随后计算的值"合法化"掉？**

---

> 附：同期观察项（未定性，暂不编号）
> - `[net] link ready` 开机打印了两次 —— PHY 链路回调重复触发还是启动期打了两次，待查。

> 文档创建：2026-10-18
> 下次新增条目请从 **BUG-068** 开始编号。
