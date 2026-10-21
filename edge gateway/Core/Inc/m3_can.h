#ifndef M3_CAN_H
#define M3_CAN_H

#include "main.h"
#include <stdint.h>

/*==============================================================================
 * M3 · CAN 与双 CAN 网关 —— 对外接口
 *
 * 端口编号：工程里只有两个 CAN 控制器
 *   M3_PORT_CAN1 = 0  → CAN1，引脚 PA11(RX) / PA12(TX)，板载 SIT1050T 收发器
 *   M3_PORT_CAN2 = 1  → CAN2，引脚 PB12(RX) / PB13(TX)，需外接 CAN 收发器模块
 *
 * 波特率：42MHz(APB1) / ((11+2+1) * 6) = 500 kbps，采样点 (1+11)/14 = 85.7%
 *============================================================================*/

#define M3_PORT_CAN1     0u
#define M3_PORT_CAN2     1u

/* ⚠️ M3-a 回环自测开关
 *   置 1：CAN1 进入 bxCAN 内部回环模式（CAN_MODE_LOOPBACK），
 *         自己发的帧被自己收回来，TX 不驱动到外部总线。
 *         → 不需要接任何外部硬件、不需要另一个 CAN 节点就能验证
 *           波特率/滤波器/中断/队列/解析这条链路是否通。
 *   置 0：正常模式，收发走外部收发器。
 *   验完必须改回 0，否则 CAN1 永远收不到外部节点的数据。 */
#ifndef M3_TEST_LOOPBACK
#define M3_TEST_LOOPBACK   0
#endif

/* ⚠️ M3-b 对连测试开关：在正常模式下让 CAN1 周期性发一帧。
   置 1：CAN1 每 1 秒发一帧 0x123（走真实总线，需要另一个节点给 ACK）。
         → 用来验证「CAN1 发 → CAN2 收 → 路由转发」这条真实链路。
   置 0：M3 只收不发（正式运行时的行为）。
   ⚠️ 验收完必须改回 0，否则正式运行时板子会一直往总线上灌测试帧。 */
#ifndef M3_TEST_TX_ENABLE
#define M3_TEST_TX_ENABLE  0   /* M3-b 已实测通过（259 帧零丢包），已收官改回 0 */
#endif

/* ⚠️ M6：路由表模式开关（M5 附录 C 留给 M6 的第 2 项）
 *
 *   置 0（默认）：沿用"ID 段掩码映射" —— 0x1xx→0x2xx、0x3xx→0x4xx 整段转发。
 *   置 1        ：改用【显式 ID 表】 —— 只有登记在 m3_can.c 的 s_route_tbl[]
 *                 里的具体 ID 才转发，其余一律丢弃并计数。
 *
 * ⭐ 什么时候该换成表模式：
 *    段映射写起来省事，但它把"整段 ID"都放行了。真接到一条实车/实机总线上，
 *    那段里可能有你根本不认识的报文（别的 ECU 的心跳、诊断帧、NM 报文），
 *    全转过去等于把噪声灌进另一条总线，还浪费对端带宽。
 *    表模式是"默认拒绝（default-deny）"，只放行确认过的 —— 这才是网关该有的姿态。
 *    （和防火墙规则一个道理：白名单永远比黑名单安全。）
 *
 * ⚠️ 默认保持 0：当前 259 帧零丢包的验证是在段映射下做的，
 *    切成表模式会改变转发行为，必须重新跑一遍对连测试才能算数。 */
#ifndef M3_ROUTE_TABLE_ENABLE
#define M3_ROUTE_TABLE_ENABLE   0
#endif

/* 表模式的诊断：被白名单过滤掉的帧数（段映射模式下这个计数不增长） */
uint32_t M3_RouteFilteredCount(void);

/* ⚠️ 路由白名单 + ID 映射（防自激，见 m3_route 里的说明）
   CAN1 收到 0x100~0x1FF  → 映射成 0x200~0x2FF 转发到 CAN2
   CAN2 收到 0x300~0x3FF  → 映射成 0x400~0x4FF 转发到 CAN1
   其余 ID 一律不转发。
   这样即使 CAN1/CAN2 被接在同一条物理总线上，也不会出现
   「A发→B收→转回A→再收→再转」的无限自激。 */
#define M3_RT_C1_SRC   0x100u
#define M3_RT_C1_DST   0x200u
#define M3_RT_C2_SRC   0x300u
#define M3_RT_C2_DST   0x400u
#define M3_RT_MASK     0x700u   /* 标准 ID 高 3 位作为"段" */

/* 测试报文 */
/* 回环 / 对连自测用的报文 ID。
   ⚠️ 为什么是 0x301 而不是 0x123？
      路由规则按「从哪一侧收到」判定：CAN2 收到 0x3xx 才转发（→ CAN1 成 0x4xx）。
      自测帧由 CAN1 **发出**，只会被 CAN2 收到：
        - ID=0x123 → CAN2 侧规则(0x3xx)不匹配 → 正确丢弃（防自激），**看不到路由**
        - ID=0x301 → CAN2 收到后转 CAN1 成 0x401 → 打印 `C1 rx id=0x401` ✅
      而 CAN1 发的帧不会被自己接收（bxCAN 非回环自发自收不成立），所以不会自激。
      回环模式(LOOPBACK)下路由被禁用，ID 取什么值都不影响，无需改回。 */
#define M3_LB_ID           0x301u     /* 标准帧 ID：对连模式下落在 0x300~0x3FF 转发段内 */
#define M3_LB_PERIOD_MS    1000u      /* 测试发送周期 */

/* 每个端口的接收队列深度（单位：帧）。
   ⚠️ 中断里收帧、任务里处理，队列是缓冲。CAN 500kbps 满负载约 2000 帧/秒，
      32 帧只能扛住很短的突发；正式项目建议 64~128，这里受限于 20KB heap。 */
#define M3_QUEUE_LEN       32u

/* 发送超时：等多久算"没发出去"。
   正常总线有节点 ACK 时，一帧几十微秒就发完了。设 100ms 足够宽松。 */
#define M3_TX_TIMEOUT_MS   100u

/* 统计信息打印周期 */
#define M3_STAT_PERIOD_MS  5000u

/* 任务栈（单位：word，1 word = 4 字节） */
#define M3_TASK_STACK      768u    /* 字节。M4 后 printf 走 m4_log，512 偏紧，给 768 */

/*==============================================================================
 * 数据结构
 *============================================================================*/

/* 一帧 CAN 报文的"软件表示"
   ⚠️ 为什么不用 HAL 的 CAN_RxHeaderTypeDef 直接往队列里塞？
      因为它带填充、带枚举，一帧 20+ 字节；队列里存它既浪费内存又不好打印。
      统一转成这个紧凑结构，8 字节数据 + 元数据，跨模块（M4 数据池）也好用。 */
typedef struct
{
    uint32_t id;        /* 11 位标准 ID（0x000~0x7FF）或 29 位扩展 ID */
    uint8_t  ide;       /* 0 = 标准帧，1 = 扩展帧 */
    uint8_t  rtr;       /* 0 = 数据帧，1 = 远程帧（遥控帧，要数据但自己不带数据） */
    uint8_t  dlc;       /* 数据长度 0~8（CAN FD 才允许 >8，F4 的 bxCAN 是经典 CAN） */
    uint8_t  port;      /* 这帧是从哪个端口收上来的：M3_PORT_CAN1 / CAN2 */
    uint8_t  data[8];   /* 数据域，不足 8 字节的部分无意义 */
} M3_CanFrame;

/* 运行状态统计
   ⚠️ 这些数字是简历上的"量化指标"来源：连续跑 N 小时、收发多少帧、丢多少帧。
      任务里读、ISR 里写，读之前调用方要用临界区保护（见 m3_can.c）。 */
typedef struct
{
    uint32_t rx[2];         /* 收到的帧数 */
    uint32_t tx[2];         /* 成功发出的帧数 */
    uint32_t rx_drop[2];    /* 队列满 → 丢弃的帧数（⭐ 丢帧率就靠它） */
    uint32_t tx_fail[2];    /* 发送失败次数（无空闲 mailbox / 超时未 ACK） */
    uint32_t busoff[2];     /* 总线关闭（BUS-OFF）次数 */
    uint8_t  tec[2];        /* 发送错误计数器 0~255，≥256 进 BUS-OFF */
    uint8_t  rec[2];        /* 接收错误计数器 0~255 */
} M3_CanStats;

/*==============================================================================
 * API
 *============================================================================*/

/* 初始化 CAN 硬件 + 建队列 + 建任务。
   ⚠️ 必须在调度器启动之后调用（内部要用 osThreadNew、中断要用 FromISR API），
      所以放在 freertos.c 的 RTOS_THREADS 区，不要放 main() 里。 */
void M3_CanStart(void);

/* 发送一帧。
   port    : M3_PORT_CAN1 / M3_PORT_CAN2
   id      : ≤0x7FF 走标准帧，>0x7FF 自动走扩展帧
   data/len: 数据域，len 最大 8
   timeout : 超时 ms（0 表示只投递不等待）
   返回    : 0 成功；-1 参数非法；-2 三个发送邮箱全忙；-3 超时未发出（多半没节点 ACK）
   ⚠️ 只能在任务上下文调用（内部有 osDelay），中断里不能用。 */
int  M3_CanSend(uint8_t port, uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms);

/* 从接收队列取一帧（阻塞等待）。
   返回：0 取到；-1 超时；-2 端口非法
   注：M3 内部任务已经在消费队列了，这个函数主要给 M4 数据池/应用层复用。 */
int  M3_CanRecv(uint8_t port, M3_CanFrame *out, uint32_t timeout_ms);

/* 取统计快照（内部会加临界区，返回的是拷贝到静态缓冲区的指针） */
const M3_CanStats *M3_CanGetStats(void);

#endif /* M3_CAN_H */
