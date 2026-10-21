# 边缘智能网关（STM32F407 + FreeRTOS）

以太网 / 双 CAN / RS485 三网融合的边缘网关，带看门狗裁决、链路冗余与防砖 OTA。

一句话定位：**把现场总线（CAN、RS485）的数据收上来，通过以太网可靠地送出去，
并且在断网、死机、升级失败时都能自己兜住。**

---

## 架构

```
                    ┌──────────────────────────────────────┐
   RS485 (USART6)   │                                      │
   Modbus RTU 主站 ─┼──► M2  采集                          │
                    │      │                               │
   CAN1 (PA11/12) ──┼─► M3 双 CAN ──► 路由转发 ─┐          │
   CAN2 (PB12/13) ──┼─►      │                  │          │
                    │        ▼                  ▼          │
                    │   ┌─────────────────────────────┐    │
                    │   │  M4 统一数据池（互斥量+快照）│    │
                    │   └─────────────────────────────┘    │
                    │        │            ▲                │
                    │        ▼            │                │
                    │   M5 链路层 ────────┘                │
                    │   主：ETH (RMII, 192.168.1.30)       │
                    │   备：ESP8266 (USART3 AT)            │
                    │   降级：W25Q128 缓存补传             │
                    │                                      │
                    │   M5 看门狗：IWDG 裁决喂狗            │
                    │   M5 OTA：Bank A/B 双分区             │
                    │   M5 PWM：TIM3 CH1 (PA6) 风扇调速     │
                    └──────────────────────────────────────┘
```

任务（FreeRTOS）：`m1_cmd` / `m1_rpt` / `m3_can` / `m4_mon` / `m4_log` / `defaultTask`

---

## 实测数据

全部来自串口输出或工具打印，可复现。

| 类别 | 指标 | 实测值 |
|---|---|---|
| OTA | 传输 | 101132 B / **9.8 s ≈ 10.3 KB/s**（单次） |
| OTA | 完整性 | 板端累计 CRC = 本地 CRC32，零误差 |
| OTA | 防砖-整体 CRC | 故意给错 → `CRC_ERR`，拒绝切换、不复位 |
| OTA | 防砖-分块 CRC | 改坏一块 → `CRC_ERR`，received 停原地 |
| OTA | 断点续传 | `offset=50400` 跳过擦除继续 |
| CAN | 零丢包 | `C1 tx=259 / C2 rx=259`，`drop=0` |
| CAN | 总线健康 | `tec=0 rec=0 bo=0`，全程无 error-passive |
| 风扇 | 温度曲线 | 8 档全对，4 轮循环逐字节一致 |
| 资源 | 栈水线（栈 1536 B） | `m1_cmd 1184 B / m1_rpt 832 B / m3_can 404 B` |
| 资源 | Heap | 空闲 8328 B / 历史最低 8128 B / `log_drop=0` |
| 资源 | 代码体积 | `Code=102064 RO=1608 RW=644 ZI=104448` |
| 启动 | 自检 | 30 s 打出 `app confirmed` |

---

## 快速复现

### 硬件（正点原子 探索者 STM32F407ZGT6）

| 接口 | 引脚 | 备注 |
|---|---|---|
| ETH RMII | 板载 LAN8720/YT8512C | 静态 IP `192.168.1.30/24`，PC 设 `192.168.1.10` |
| CAN1 | PA11 / PA12 | P5 跳线帽跳 CAN 侧 |
| CAN2 | PB12 / PB13 | 需外接收发器模块，两端各 120 Ω |
| RS485 | USART6 PC6/PC7 + PE5 方向 | **P4 跳线帽必须拔掉** |
| ESP8266 | USART3 PB10/PB11 | P2 跳线跳 ATK-MODULE 侧，VCC 并 100 µF+0.1 µF |
| 风扇 PWM | TIM3 CH1 = PA6，1 kHz | 3.3 V IO，需 MOS 驱动 |
| W25Q128 | SPI1 PB3/4/5 + PB14 片选 | PB3/4/5 是 JTAG 脚，CubeMX 的 Debug 要设 **SWD** |

### 编译与烧录

```bash
"D:/Keil5/UV4/UV4.exe" -j0 -b "edge gateway.uvprojx" -o build.log
```

⚠️ **烧录顺序**：先 Bootloader（0x08000000）再 App（0x08010000），
Keil 必须选 **Erase Sectors**，**不能 Erase Full Chip**（会擦掉 Bootloader）。

### 验证

```bash
# OTA 下发（--block 上限 244，默认 240）
python tools/m5_ota_host.py --file app.bin
python tools/m5_ota_host.py --file app.bin --bad-total-crc   # 应拒绝
python tools/m5_ota_host.py --file app.bin --corrupt          # 应回 CRC_ERR
python tools/m5_ota_host.py --file app.bin --resume           # 断点续传

# 上位机监听 + 去重验证
python tools/m4_host_test.py --listen --ack
```

端口：TCP **5000** 命令（含 OTA）、**5001** 上报、备用链路 **5002**。

---

## 目录结构

```
Core/
  Inc/  Src/
    m1_net.*     以太网 lwIP（TCP Server 5000 + Client 5001）
    m2_*         RS485 + Modbus RTU 主站
    m3_can.*     双 CAN 驱动 + 路由
    m4_*         日志环形缓冲 / 统一数据池 / 协议帧 / 监控任务
    m5_*         看门狗裁决 / 黑匣子 / 链路冗余 / ESP8266 / OTA / 风扇 PWM
    m6_store.*   断网补传缓存（W25Q128）
    shared/flash_layout.h   ⭐ Flash 分区的唯一来源（Bootloader 与 App 共用）
Bootloader/      独立 Keil 工程，32 KB
tools/           上位机工具（OTA 下发、协议测试）
```

### Flash 分区

```
0x08000000  Bootloader   S0+S1    32 KB   永不自更新
0x08008000  参数/黑匣子   S2+S3    32 KB   复位计数/死因/OTA 状态
0x08010000  Bank A (App) S4~S7   448 KB   当前运行
0x08080000  Bank B (OTA) S8~S11  512 KB   新固件暂存
```

---

## 已知限制（如实列出）

| 限制 | 真实原因 | 正确表述 |
|---|---|---|
| 没有"自动回滚上一版本" | Bank A 已被新固件覆盖 | "校验失败拒绝切换 + 启动失败进安全模式"，**不是回滚** |
| WWDG 在 OTA 期间不可用 | 擦 Flash 时 CPU stall 超过窗口期（硬件约束） | "OTA 期间由 IWDG 兜底" |
| 超温保护不是 10 ms 级 | 走 Modbus 轮询，约 100 ms~1 s | "采集周期内触发保护" |
| 断网补传没有掉电保护 | 无掉电检测电路 | "每条带 CRC32，半写损坏自动跳过" |
| Bootloader 不支持自更新 | 刻意设计（改它变砖风险最高） | 明说这是安全决策 |
| Modbus（M2）未联调 | 线材未到位 | 代码完成，待实测 |

---

## 开发记录

- `BUG记录与避坑清单.md` —— 67 条实测踩坑（含日志刷屏、看门狗误判、协议字段宽度等），
  每条都按「现象 → 定位 → 根因 → 修法」记录，是本项目最有参考价值的部分。

各里程碑（M1 以太网/lwIP、M2 RS485+Modbus RTU、M3 双 CAN、M4 系统集成、
M5 可靠性与冗余 OTA）的教学讲义属学习资料，未随仓库发布。
