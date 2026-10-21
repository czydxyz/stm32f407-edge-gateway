/*==============================================================================
 * M3 · CAN 与双 CAN 网关
 *
 * 三层职责：
 *   ① 硬件层：滤波器分区 → HAL_CAN_Start → 激活 FIFO0 中断
 *   ② 中断层：ISR 收帧 → 转 M3_CanFrame → 投队列（不能 printf！）
 *   ③ 任务层：取帧 → 解析 → 路由转发到另一个 CAN → 统计 → 总线健康监控
 *
 * ⚠️ 本文件所有串口输出都用英文：ARMCC5 下 printf 字符串含中文会报 #870-D
 *    警告（见 BUG-042）。注释用中文不影响。
 *============================================================================*/

#include "m3_can.h"
#include "m4_pool.h"     /* M4：CAN 统计进统一数据池 */
#include "m4_task.h"     /* M4：存活位 + 任务登记 */
#include "can.h"          /* hcan1 / hcan2 */
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>

/*==============================================================================
 * 内部状态
 *============================================================================*/
static QueueHandle_t    s_rxq[2];          /* 每个端口一条接收队列：ISR 写，任务读 */
static M3_CanStats      s_stats;           /* 统计：ISR 和任务共享 */
static M3_CanStats      s_snapshot;        /* 对外快照（临界区内拷贝出来） */
static volatile uint8_t s_boff_latch[2];   /* BUS-OFF 锁存，防止一次 bus-off 被重复计数 */
static volatile uint8_t s_epvf_latch[2];   /* 错误被动锁存（BUG-062）：进/出各报一次，别每 5s 刷屏 */
static uint32_t         s_last_rx[2];      /* M4：上一次同步到数据池时的 rx 计数，用于判"在线" */

/* ⭐ BUG-065：统计行的降频依据。
   M3 收官（M3_TEST_TX_ENABLE=0）之后总线上没有流量，
   但统计行还是每 5 s 雷打不动打两行 `rx=0 tx=0 drop=0 ...`，
   一夜下来几万行全 0 —— 和 BUG-062/063 是同一个毛病：
   把"周期性轮询"当成了"事件"来打日志。
   改成：有变化才打；完全空闲时每 30 s 打一行心跳（证明任务还活着就行）。 */
static M3_CanStats      s_last_print;      /* 上次真正打印时的统计快照 */
static uint8_t          s_idle_rounds;     /* 连续"无变化"的统计轮数（1 轮 = 5 s） */

static void m3_hw_init(void);
static void m3_route(const M3_CanFrame *f);
static void m3_bus_check(void);
static void CANx_CommonInit(CAN_HandleTypeDef *hcan, uint8_t port);

/*==============================================================================
 * ① 硬件初始化：CubeMX 没做完的活，全在这里补
 *============================================================================*/

/* CAN1 / CAN2 共用的参数补丁。
   ⚠️ 为什么要重新调一次 HAL_CAN_Init()？
      因为 CubeMX 6.17 的 .ioc 里改了 AutoRetransmission，生成代码里仍是 DISABLE
      （和 AutoBusOff 同一个毛病，见 can.c 里的 ABOM 补丁）。
      HAL_CAN_Init() 内部会主动请求进入初始化模式（置 INRQ 等 INAK），
      所以只要还没 Start，随时可以重新调用它改参数；
      ⚠️ 一旦 HAL_CAN_Start() 之后再调，会把总线踢下线一小会儿，别那么干。 */
static void CANx_CommonInit(CAN_HandleTypeDef *hcan, uint8_t port)
{
    (void)port;

    /* AutoRetransmission（自动重发）
       = DISABLE：仲裁失败或出错就丢这一帧，软件得自己重发。
       = ENABLE ：硬件自动重发直到成功 —— 网关要的就是这个。
       ⚠️ 副作用：总线上没有其它节点（没人给 ACK）时，硬件会一直重发，
          TEC 每 8 帧涨一次，涨到 256 就 BUS-OFF。
          所以"单节点裸发"看到 BUS-OFF 是正常物理现象，不是代码 bug（见 BUG-047）。 */
    hcan->Init.AutoRetransmission = ENABLE;

#if M3_TEST_LOOPBACK
    /* M3-a 回环自测：只有 CAN1 切回环 */
    if (hcan->Instance == CAN1)
    {
        hcan->Init.Mode = CAN_MODE_LOOPBACK;
    }
#endif

    if (HAL_CAN_Init(hcan) != HAL_OK)
    {
        printf("[can] !! HAL_CAN_Init failed on port\r\n");
    }
}

static void m3_hw_init(void)
{
    CAN_FilterTypeDef fc;

    /* ---------- ① 参数补丁 ---------- */
    CANx_CommonInit(&hcan1, M3_PORT_CAN1);
    CANx_CommonInit(&hcan2, M3_PORT_CAN2);

    /* ---------- ② 滤波器配置（⭐ 双 CAN 最容易踩的坑在这里）----------
       F4 的 bxCAN 有 28 个滤波器组（bank 0~27），CAN1 和 CAN2 **共用**这一组硬件。
       谁用哪几组由 CAN1->FMR 寄存器的 C2SB[5:0]（SlaveStartFilterBank）切开：
         bank 0 .. (C2SB-1)  → 归 CAN1
         bank C2SB .. 27     → 归 CAN2
       HAL 里只有「用 hcan1 调 HAL_CAN_ConfigFilter」才会写这个字段。
       ⚠️ 所以两条铁律：
         ① 必须先配 CAN1（顺带把 SlaveStartFilterBank 写进去），再配 CAN2；
         ② CAN2 的 FilterBank 必须 ≥ SlaveStartFilterBank，否则 CAN2 的过滤器
            会写进 CAN1 的地盘，表现为「CAN1 突然收不到帧」或「CAN2 收不到帧」，
            极难排查（见 BUG-045）。 */

    fc.FilterMode           = CAN_FILTERMODE_IDMASK;   /* 掩码模式 */
    fc.FilterScale          = CAN_FILTERSCALE_32BIT;   /* 32 位宽 */
    fc.FilterIdHigh         = 0x0000;                  /* ID 高 16 位 */
    fc.FilterIdLow          = 0x0000;                  /* ID 低 16 位 */
    fc.FilterMaskIdHigh     = 0x0000;                  /* 掩码高 16 位 */
    fc.FilterMaskIdLow      = 0x0000;                  /* 掩码低 16 位 */
    /* ⚠️ 掩码全 0 = 不过滤，什么都收。
          这是**调试期**配置（回环自测、双 CAN 对连都要能收到任意 ID）。
          正式项目要按 ID 段过滤，例如只收 0x100~0x1FF：
              FilterIdHigh     = (uint16_t)(0x100 << 5);   // 标准 ID 要左移 5 位
              FilterIdLow      = 0x0000;
              FilterMaskIdHigh = (uint16_t)(0x700 << 5);   // 高 3 位必须匹配
              FilterMaskIdLow  = 0x0000;
          掩码位为 1 = 这一位必须和 ID 一致，为 0 = 不关心。 */
    fc.FilterFIFOAssignment = CAN_FILTER_FIFO0;        /* 命中后放进 FIFO0 */
    fc.FilterActivation     = ENABLE;
    fc.FilterBank           = 0;                       /* CAN1 用 bank 0 */
    fc.SlaveStartFilterBank = 14;                      /* ⭐ CAN2 从 bank 14 开始 */
    if (HAL_CAN_ConfigFilter(&hcan1, &fc) != HAL_OK)
    {
        printf("[can] !! CAN1 filter config failed\r\n");
    }

    fc.FilterBank = 14;                                /* CAN2 用 bank 14 */
    if (HAL_CAN_ConfigFilter(&hcan2, &fc) != HAL_OK)
    {
        printf("[can] !! CAN2 filter config failed\r\n");
    }

    /* ---------- ③ Start（CubeMX 不生成，不写就完全没反应）----------
       和 M1 的 HAL_ETH_Start_IT() 是同一类坑：CubeMX 只生成 MX_CANx_Init()
       让你填参数，Start / ConfigFilter / ActivateNotification 全要自己调。 */
    if (HAL_CAN_Start(&hcan1) != HAL_OK)
    {
        printf("[can] !! CAN1 start failed\r\n");
    }
    if (HAL_CAN_Start(&hcan2) != HAL_OK)
    {
        printf("[can] !! CAN2 start failed\r\n");
    }

    /* ---------- ④ 激活中断 ----------
       ⚠️ 不开 RX_FIFO0_MSG_PENDING，HAL_CAN_RxFifo0MsgPendingCallback 永远不会被调用，
          表现是"能发出去但一帧都收不到"。 */
    HAL_CAN_ActivateNotification(&hcan1,
        CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_TX_MAILBOX_EMPTY | CAN_IT_BUSOFF);
    HAL_CAN_ActivateNotification(&hcan2,
        CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_TX_MAILBOX_EMPTY | CAN_IT_BUSOFF);
}

/*==============================================================================
 * ② 中断层：FIFO0 收到报文回调
 *============================================================================*/

/* HAL 在 CANx_RX0_IRQHandler → HAL_CAN_IRQHandler 里检测到 FMP0≠0 时调用它。
   ⚠️ 这是**中断上下文**：
      - 不能 printf（慢、且非线程安全 → 丢字符，见 BUG-039）
      - 不能 osDelay / 阻塞
      - 只能用带 FromISR 后缀的 API */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef hdr;
    M3_CanFrame         f;
    BaseType_t          hpw = pdFALSE;
    uint8_t             port;
    QueueHandle_t       q;

    port = (hcan->Instance == CAN1) ? M3_PORT_CAN1 : M3_PORT_CAN2;
    q    = s_rxq[port];

    /* ⚠️ 用 while 把 FIFO 读空，而不是只读一帧。
          如果一帧中断里来了 3 帧，只读一帧剩下 2 帧要等下一次中断边沿才可能取走，
          突发流量下 FIFO 溢出（FOVR）就丢了。 */
    while (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &hdr, f.data) == HAL_OK)
    {
        f.ide  = (hdr.IDE == CAN_ID_EXT) ? 1u : 0u;
        f.id   = f.ide ? hdr.ExtId : hdr.StdId;
        f.rtr  = (hdr.RTR == CAN_RTR_REMOTE) ? 1u : 0u;
        f.dlc  = (uint8_t)hdr.DLC;
        f.port = port;

        /* 队列还没建好（比如中断来得太早）就别硬写，否则空指针硬 fault */
        if (q == NULL)
        {
            break;
        }

        if (xQueueSendFromISR(q, &f, &hpw) != pdPASS)
        {
            /* 队列满 → 丢帧。
               ⚠️ 这个计数器就是"丢帧率"的来源，别随手删掉。
                  任务处理太慢 / 队列太小都会让它涨。 */
            s_stats.rx_drop[port]++;
        }
        else
        {
            s_stats.rx[port]++;
        }
    }

    /* 如果有更高优先级任务被唤醒了，请求立刻切换 */
    portYIELD_FROM_ISR(hpw);
}

/* 错误回调：BUS-OFF / 错误被动 / 警告都会走到这里。
   只做计数和记录，不做任何耗时动作。恢复交给硬件 ABOM（已在 can.c 打开）。 */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    uint8_t  port = (hcan->Instance == CAN1) ? M3_PORT_CAN1 : M3_PORT_CAN2;
    uint32_t err  = HAL_CAN_GetError(hcan);

    if (err & HAL_CAN_ERROR_BOF)   /* BUS-OFF：TEC ≥ 256，节点被踢下线 */
    {
        if (s_boff_latch[port] == 0u)
        {
            s_boff_latch[port] = 1u;
            s_stats.busoff[port]++;
        }
    }
    /* HAL_CAN_GetError 返回的是累计错误标志（HAL 内部会清），这里不做清标志动作，
       否则会干扰 HAL 自己的错误管理。 */
}

/*==============================================================================
 * ③ 对外 API
 *============================================================================*/

int M3_CanSend(uint8_t port, uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    CAN_HandleTypeDef *h;
    CAN_TxHeaderTypeDef hdr;
    uint32_t mailbox;
    uint32_t t0;

    if (port > M3_PORT_CAN2)
    {
        return -1;
    }
    if ((data == NULL && len > 0u) || len > 8u)
    {
        return -1;
    }

    h = (port == M3_PORT_CAN1) ? &hcan1 : &hcan2;

    /* 组帧：ID ≤ 0x7FF 用标准帧，否则扩展帧 */
    if (id <= 0x7FFu)
    {
        hdr.IDE   = CAN_ID_STD;
        hdr.StdId = id;
        hdr.ExtId = 0u;
    }
    else
    {
        hdr.IDE   = CAN_ID_EXT;
        hdr.ExtId = id;
        hdr.StdId = 0u;
    }
    hdr.RTR                 = CAN_RTR_DATA;
    hdr.DLC                 = len;
    hdr.TransmitGlobalTime  = DISABLE;

    /* 投递到发送邮箱（硬件有 3 个 mailbox）
       ⚠️ 返回 != HAL_OK 只代表「3 个邮箱都满了」，不代表失败，可以稍后重试。
          网关里正确做法是：满了就等一下再试，或者统计下来。这里直接计数返回。 */
    if (HAL_CAN_AddTxMessage(h, &hdr, (uint8_t *)data, &mailbox) != HAL_OK)
    {
        s_stats.tx_fail[port]++;
        return -2;
    }

    if (timeout_ms == 0u)
    {
        s_stats.tx[port]++;
        return 0;
    }

    /* 等这一帧真正发到总线上（别的中节点回了 ACK，硬件清掉 pending）
       ⚠️ 关键认知：CAN 是"广播 + 应答"的总线，发送方必须收到至少一个节点的 ACK
          才算发送成功。总线上只有你一个节点（或者没接终端电阻、波特率不对），
          永远等不到 ACK → mailbox 一直 pending → 这里超时返回 -3。
          ⭐ 所以 "-3" 是定位硬件问题最重要的线索，不是软件 bug（见 BUG-047）。 */
    t0 = xTaskGetTickCount();
    while (HAL_CAN_IsTxMessagePending(h, mailbox) != 0U)
    {
        if ((xTaskGetTickCount() - t0) > (timeout_ms / portTICK_PERIOD_MS))
        {
            /* ⚠️ 这里**不要**调 HAL_CAN_AbortTxRequest 强行取消：
                  已投递的邮箱取消后仍可能发出半截，反而制造错误帧。
                  让它继续等（AutoRetransmission 已开，硬件会自己重试），
                  只是这一帧在软件层面记为失败。 */
            s_stats.tx_fail[port]++;
            return -3;
        }
        osDelay(1);
    }

    s_stats.tx[port]++;
    return 0;
}

int M3_CanRecv(uint8_t port, M3_CanFrame *out, uint32_t timeout_ms)
{
    if (port > M3_PORT_CAN2 || out == NULL || s_rxq[port] == NULL)
    {
        return -2;
    }
    if (xQueueReceive(s_rxq[port], out, (timeout_ms / portTICK_PERIOD_MS)) == pdPASS)
    {
        return 0;
    }
    return -1;   /* 超时 */
}

/* 统计快照：任务读、ISR 写，必须加临界区。
   ⚠️ 为什么必须：s_stats.rx++ 在 CM4 上编译成「读-改-写」三条指令，
      拷贝 40 字节期间如果 ISR 插进来改了某个成员，拷贝出来的数据是"半新半旧"的。
      taskENTER_CRITICAL 会把中断关到 configMAX_SYSCALL 优先级以下（这里是 5），
      CAN 中断正好是 5，所以能被屏蔽住 —— 前提条件成立。 */
const M3_CanStats *M3_CanGetStats(void)
{
    taskENTER_CRITICAL();
    s_snapshot = s_stats;
    taskEXIT_CRITICAL();
    return &s_snapshot;
}

/*==============================================================================
 * ④ 路由：CAN1 ↔ CAN2 网关转发
 *============================================================================*/
/* ⭐ M6：显式 ID 路由表（M3_ROUTE_TABLE_ENABLE = 1 时生效）
 *
 * 每条规则 = "从哪个端口收到哪个 ID → 换成哪个 ID 从哪个端口发出去"。
 *
 * ⚠️ 三条设计约束（写错任何一条都会出事）：
 *   ① 映射后的 ID 【不能】再命中任何一条规则的 src_id
 *      —— 否则会自激（0x301 → 0x401，若还有一条 0x401 → xxx 就无限循环了）。
 *      段映射靠"ID 段不重叠"天然满足；表模式靠【人工保证】，所以表必须短、必须审。
 *   ② 表要【短】：查表是线性扫描，跑在 CAN 接收任务里，
 *      每条规则一次比较，8 条以内开销可忽略；几百条就该换哈希/位图了。
 *   ③ 未命中的帧【丢弃并计数】，不要"默认放行"
 *      —— 默认放行等于没有白名单，接上真实总线瞬间就会被噪声打满。
 */
#if M3_ROUTE_TABLE_ENABLE
typedef struct {
    uint8_t   src_port;
    uint16_t  src_id;
    uint8_t   dst_port;
    uint16_t  dst_id;
} m3_route_entry_t;

static const m3_route_entry_t s_route_tbl[] = {
    /* CAN2 侧上行 → 转发到 CAN1（与段映射 0x3xx→0x4xx 行为一致） */
    { M3_PORT_CAN2, 0x301u, M3_PORT_CAN1, 0x401u },
    { M3_PORT_CAN2, 0x302u, M3_PORT_CAN1, 0x402u },
    /* CAN1 侧上行 → 转发到 CAN2（与段映射 0x1xx→0x2xx 行为一致） */
    { M3_PORT_CAN1, 0x101u, M3_PORT_CAN2, 0x201u },
    { M3_PORT_CAN1, 0x102u, M3_PORT_CAN2, 0x202u },
};
#define M3_ROUTE_TBL_N   (sizeof(s_route_tbl) / sizeof(s_route_tbl[0]))
#endif

static uint32_t s_filtered;    /* 被白名单丢弃的帧数 */

uint32_t M3_RouteFilteredCount(void) { return s_filtered; }

/*==============================================================================
 * ⭐ 路由：把一条总线的帧按规则转到另一条总线
 *============================================================================*/
static void m3_route(const M3_CanFrame *f)
{
    uint32_t dst_id   = 0u;
    uint8_t  dst_port = 0u;
    uint8_t  forward  = 0u;

#if M3_TEST_LOOPBACK
    /* 回环模式：TX 不驱动到外部总线，转发毫无意义，直接禁用 */
    return;
#else
    /* ⚠️ 为什么不能"全透传"（把 CAN1 收到的原样发到 CAN2，反之亦然）？
       因为 M3-b 的测试拓扑是 **CAN1 和 CAN2 接在同一条物理总线上**。
       全透传时：CAN1 发 → CAN2 收 → 转发到 CAN1 → CAN1 又发 → CAN2 又收 …
       一帧变成无限循环，几毫秒内就把总线打满（自激/广播风暴）。
       即使是正式的两条独立总线，全透传也等于把两边的广播风暴直接打通。

       正确做法：**ID 白名单 + ID 重映射**
         - 只转发登记过的 ID 段（白名单），其余一律丢弃；
         - 转发时**改 ID**（0x1xx → 0x2xx），映射后的 ID 落在另一段里，
           不会再次命中任何转发规则 → 天然只转一跳，不可能自激。
       这也是真实 CAN 网关（比如汽车里动力总线↔车身总线）的标准做法。 */

    if (f->ide != 0u)
    {
        return;   /* 本工程只处理标准帧，扩展帧先不转发（白名单未覆盖） */
    }

#if M3_ROUTE_TABLE_ENABLE
    /* ---------- M6：显式 ID 表模式（default-deny）---------- */
    {
        uint8_t i;

        for (i = 0u; i < M3_ROUTE_TBL_N; i++)
        {
            if ((s_route_tbl[i].src_port == f->port) &&
                (s_route_tbl[i].src_id   == (uint16_t)f->id))
            {
                dst_id   = s_route_tbl[i].dst_id;
                dst_port = s_route_tbl[i].dst_port;
                forward  = 1u;
                break;
            }
        }
    }
#else
    /* ---------- 段映射模式（默认，已验证 259 帧零丢包）---------- */
    if (f->port == M3_PORT_CAN1)
    {
        if ((f->id & M3_RT_MASK) == M3_RT_C1_SRC)      /* 0x100~0x1FF */
        {
            dst_id   = (f->id & ~M3_RT_MASK) | M3_RT_C1_DST;   /* → 0x2xx */
            dst_port = M3_PORT_CAN2;
            forward  = 1u;
        }
    }
    else
    {
        if ((f->id & M3_RT_MASK) == M3_RT_C2_SRC)      /* 0x300~0x3FF */
        {
            dst_id   = (f->id & ~M3_RT_MASK) | M3_RT_C2_DST;   /* → 0x4xx */
            dst_port = M3_PORT_CAN1;
            forward  = 1u;
        }
    }
#endif

    if (forward == 0u)
    {
        /* ⭐ 被白名单拦下。计数而不是打日志：
           真实总线上绝大多数帧都会被拦（别的节点的心跳、NM、诊断帧），
           每帧 printf 会立刻刷爆串口 —— 这正是 BUG-039 的教训。
           计数让上位机能看到"过滤了多少"，又不产生日志噪声。 */
        s_filtered++;
        return;
    }

    /* 转发失败只记数，不打日志：
       对端没节点/没终端电阻时每帧都会失败，printf 会刷爆串口（叠加 BUG-039 更糟） */
    (void)M3_CanSend(dst_port, dst_id, f->data, f->dlc, M3_TX_TIMEOUT_MS);
#endif
}

/*==============================================================================
 * ⑤ 总线健康监控：TEC / REC / BUS-OFF
 *============================================================================*/
static void m3_bus_check(void)
{
    uint8_t p;
    CAN_HandleTypeDef *h;

    for (p = 0; p < 2u; p++)
    {
        h = (p == M3_PORT_CAN1) ? &hcan1 : &hcan2;

        /* ESR 位分布（F4 参考手册）：
             bit 0    : EWGF 错误警告（TEC 或 REC ≥ 96）
             bit 1    : EPVF 错误被动（TEC 或 REC ≥ 128）
             bit 2    : BOFF 总线关闭（TEC ≥ 256）
             bit 16-23: TEC 发送错误计数
             bit 24-31: REC 接收错误计数
           ⚠️ BOFF 位要靠"进/出"沿来计数，别每周期 +1，否则一次 bus-off 被算成几千次。 */
        uint32_t esr = h->Instance->ESR;

        s_stats.tec[p] = (uint8_t)((esr >> 16) & 0xFFu);
        s_stats.rec[p] = (uint8_t)((esr >> 24) & 0xFFu);

        if ((esr & CAN_ESR_BOFF) != 0u)
        {
            if (s_boff_latch[p] == 0u)
            {
                s_boff_latch[p] = 1u;
                s_stats.busoff[p]++;
                printf("[can] !! port%d BUS-OFF (TEC=%d)\r\n", p, s_stats.tec[p]);
            }
        }
        else
        {
            if (s_boff_latch[p] != 0u)
            {
                s_boff_latch[p] = 0u;
                /* ABOM 已开，硬件自动恢复；这里只上报一次，方便统计 MTTR */
                printf("[can] port%d recovered from BUS-OFF\r\n", p);
            }
        }

        /* 错误被动是 BUS-OFF 前最后一个预警，值得单独提示：
           到了这一步说明总线已经有严重问题（波特率不对 / 终端电阻缺失 / 强干扰）。 */
        if ((esr & CAN_ESR_EPVF) != 0u)
        {
            /* ⭐ 用进/出沿锁存，只报【状态变化】（BUG-062）。
               原写法是"每 5 秒最多一次"—— 而本函数就是 5 秒调一次，
               等于每 5 秒刷一条。总线断线时这条会和统计行混在一起刷一晚上，
               把真正重要的异常（BUS-OFF、拒喂）淹掉。
               看齐 BUS-OFF 的做法：进来报一次，恢复再报一次，中间保持沉默。 */
            if (s_epvf_latch[p] == 0u)
            {
                s_epvf_latch[p] = 1u;
                printf("[can] ! port%d error-passive TEC=%d REC=%d\r\n",
                       p, s_stats.tec[p], s_stats.rec[p]);
            }
        }
        else
        {
            if (s_epvf_latch[p] != 0u)
            {
                s_epvf_latch[p] = 0u;
                printf("[can] port%d back to error-active (TEC=%d REC=%d)\r\n",
                       p, s_stats.tec[p], s_stats.rec[p]);
            }
        }
    }
}

/*==============================================================================
 * ⑥ 主任务
 *============================================================================*/
/* ⭐ BUG-065：这一轮的统计相对【上次打印时】有没有变化。
   ⚠️ 比较对象是 s_last_print（上次打印的快照），不是"上一轮"——
      否则在持续有流量的场景里每轮都有变化，等于没降频。
      空闲场景才是主要矛盾：rx/tx/drop/fail/bo/tec 全都不动，一行都不该打。 */
static int m3_stats_changed(const M3_CanStats *cur)
{
    uint8_t p;

    for (p = 0u; p < M4_CAN_PORT_NUM; p++)
    {
        if (cur->rx[p]      != s_last_print.rx[p])      return 1;
        if (cur->tx[p]      != s_last_print.tx[p])      return 1;
        if (cur->rx_drop[p] != s_last_print.rx_drop[p]) return 1;
        if (cur->tx_fail[p] != s_last_print.tx_fail[p]) return 1;
        if (cur->busoff[p]  != s_last_print.busoff[p])  return 1;
        if (cur->tec[p]     != s_last_print.tec[p])     return 1;
    }
    return 0;
}

static void m3_can_task(void *argument)
{
    M3_CanFrame f;
    uint32_t    t_stat;
    const M3_CanStats *st;

#if (M3_TEST_LOOPBACK || M3_TEST_TX_ENABLE)
    /* 周期发测试帧：
       - M3_TEST_LOOPBACK=1 → 走内部回环，自己发自己收（M3-a）
       - M3_TEST_TX_ENABLE=1 → 走真实总线，CAN1 发、CAN2 收（M3-b） */
    uint32_t    t_lb;
    uint8_t     lb_data[8];
    uint8_t     lb_seq = 0;
#endif

    (void)argument;

    printf("[can] task start, 500 kbps, loopback=%d\r\n", M3_TEST_LOOPBACK);

    t_stat = xTaskGetTickCount();
#if (M3_TEST_LOOPBACK || M3_TEST_TX_ENABLE)
    t_lb   = xTaskGetTickCount();
#endif

    for (;;)
    {
        /* ⭐ M5 · 存活位：必须在【循环开头无条件置位】（BUG-058）
           ⚠️ 之前放在下面"5 秒统计"分支里，造成了本项目到目前为止最严重的一次事故：
                monitor 每 5 s 清零一次存活位，CAN 也是每 5 s 才置一次，
                两个 5 s 周期一旦错开，就必然出现"已被清零、CAN 还没来得及置位"
                的检查窗口 → 误判 missing 0x04 CAN → 拒喂 IWDG → 复位；
                重启后接线没变、时序也没变 → 再次拒喂 → 【无限复位】，板子永远起不来。

           记住存活位的语义：它只表示"这个任务还在转"，
           不代表"CAN 链路是通的"。链路通不通由数据池的 online 位表达（rx 有增长），
           两者绝不能混为一谈 —— 混了就会出现"总线没接 = 系统判死"的荒谬结果。 */
        M4_AliveSet(M4_ALIVE_CAN);

        /* ---------- 收 CAN1 ----------
           阻塞 200ms：既能及时处理，又留出时间给下面检查 CAN2 和打统计 */
        if (xQueueReceive(s_rxq[M3_PORT_CAN1], &f, (200u / portTICK_PERIOD_MS)) == pdPASS)
        {
            printf("[can] C1 rx id=0x%03lX dlc=%d d=", (unsigned long)f.id, f.dlc);
            {
                uint8_t i;
                for (i = 0; i < f.dlc; i++)
                {
                    printf("%02X ", f.data[i]);
                }
            }
            printf("\r\n");
            m3_route(&f);
        }

        /* ---------- 收 CAN2（非阻塞，顺带处理）---------- */
        if (xQueueReceive(s_rxq[M3_PORT_CAN2], &f, 0u) == pdPASS)
        {
            printf("[can] C2 rx id=0x%03lX dlc=%d d=", (unsigned long)f.id, f.dlc);
            {
                uint8_t i;
                for (i = 0; i < f.dlc; i++)
                {
                    printf("%02X ", f.data[i]);
                }
            }
            printf("\r\n");
            m3_route(&f);
        }

        /* ---------- 周期发测试帧（M3-a 回环 / M3-b 对连）---------- */
#if (M3_TEST_LOOPBACK || M3_TEST_TX_ENABLE)
        if ((xTaskGetTickCount() - t_lb) >= (M3_LB_PERIOD_MS / portTICK_PERIOD_MS))
        {
            uint8_t i;
            t_lb = xTaskGetTickCount();
            for (i = 0; i < 8u; i++)
            {
                lb_data[i] = lb_seq + i;
            }
            printf("[can] test tx id=0x%03lX seq=%d -> %d\r\n",
                   (unsigned long)M3_LB_ID, lb_seq,
                   M3_CanSend(M3_PORT_CAN1, M3_LB_ID, lb_data, 8u, M3_TX_TIMEOUT_MS));
            lb_seq++;
        }
#endif

        /* ---------- 5 秒统计 + 总线体检 ---------- */
        if ((xTaskGetTickCount() - t_stat) >= (M3_STAT_PERIOD_MS / portTICK_PERIOD_MS))
        {
            t_stat = xTaskGetTickCount();
            m3_bus_check();
            st = M3_CanGetStats();

            /* ---- M4：把 CAN 统计写进统一数据池，供上位机查询 ----
               online 判据：这个 5s 窗口内 rx 计数有增长 = 总线上有节点在发帧。
               ⭐ 这是"CAN 链路活着"最直接的证据，比看 TEC 直观得多。 */
            for (uint8_t p = 0; p < M4_CAN_PORT_NUM; p++)
            {
                uint8_t online = (st->rx[p] != s_last_rx[p]) ? 1u : 0u;
                s_last_rx[p] = st->rx[p];
                M4_PoolSetCanStats(p, st->rx[p], st->rx_drop[p],
                                   (uint8_t)st->busoff[p], online);
            }
            /* ⚠️ M4_AliveSet(M4_ALIVE_CAN) 【已移到循环开头】—— 不要加回这里 */

            /* ---------- ⭐ BUG-065：统计行降频 ----------
               总线空闲时每 5 s 两行全 0 是纯噪声（M3 收官后就是这种状态）。
               判据：这一轮的统计和上次【打印时】的快照比，有任何一项变了才打。
               ⚠️ 为什么还要留心跳：完全不打，日志里就再也看不到 CAN 的痕迹，
                  万一 m3_can 任务真的卡死了也发现不了。
                  30 s 一行足够区分"任务还活着但总线没流量"和"任务死了"。 */
            if (m3_stats_changed(st))
            {
                s_idle_rounds = 0u;
                s_last_print  = *st;      /* 整块拷贝，下一轮拿它当基准 */

                printf("[can] C1 rx=%lu tx=%lu drop=%lu fail=%lu bo=%lu tec=%d\r\n",
                       (unsigned long)st->rx[0], (unsigned long)st->tx[0],
                       (unsigned long)st->rx_drop[0], (unsigned long)st->tx_fail[0],
                       (unsigned long)st->busoff[0], st->tec[0]);
                printf("[can] C2 rx=%lu tx=%lu drop=%lu fail=%lu bo=%lu tec=%d\r\n",
                       (unsigned long)st->rx[1], (unsigned long)st->tx[1],
                       (unsigned long)st->rx_drop[1], (unsigned long)st->tx_fail[1],
                       (unsigned long)st->busoff[1], st->tec[1]);
            }
            else if (++s_idle_rounds >= 6u)          /* 6 轮 × 5 s = 30 s */
            {
                s_idle_rounds = 0u;
                printf("[can] idle 30s: C1 rx=%lu tx=%lu | C2 rx=%lu tx=%lu (no traffic)\r\n",
                       (unsigned long)st->rx[0], (unsigned long)st->tx[0],
                       (unsigned long)st->rx[1], (unsigned long)st->tx[1]);
            }
        }
    }
}

/*==============================================================================
 * 启动入口（freertos.c 的 RTOS_THREADS 区调用）
 *============================================================================*/
void M3_CanStart(void)
{
    osThreadAttr_t attr;

    /* CAN 在线判据用的上一次 rx 计数（本文件静态，M4 数据池同步时更新） */
    s_last_rx[0] = 0;
    s_last_rx[1] = 0;

    /* ⚠️ 顺序不能反：先建队列，再初始化硬件开中断。
          m3_hw_init() 里最后一步是 ActivateNotification，
          一旦开了 RX 中断，如果总线上正好有数据，ISR 会立刻跑起来访问 s_rxq。
          队列此时还没建（NULL），xQueueSendFromISR(NULL) → 硬 fault。
          （ISR 里我加了 NULL 兜底，但靠兜底不如靠顺序 —— 见 BUG-046） */
    s_rxq[M3_PORT_CAN1] = xQueueCreate(M3_QUEUE_LEN, (UBaseType_t)sizeof(M3_CanFrame));
    s_rxq[M3_PORT_CAN2] = xQueueCreate(M3_QUEUE_LEN, (UBaseType_t)sizeof(M3_CanFrame));

    if ((s_rxq[M3_PORT_CAN1] == NULL) || (s_rxq[M3_PORT_CAN2] == NULL))
    {
        printf("[can] !! queue create failed (heap too small?)\r\n");
        return;
    }

    m3_hw_init();

    attr.name       = "m3_can";
    attr.attr_bits  = 0u;
    attr.cb_mem     = NULL;
    attr.cb_size    = 0u;
    attr.stack_mem  = NULL;
    attr.stack_size = M3_TASK_STACK;
    attr.priority   = (osPriority_t)osPriorityNormal;
    attr.tz_module  = 0u;
    attr.reserved   = 0u;

    {
        osThreadId_t tid = osThreadNew(m3_can_task, NULL, &attr);

        if (tid == NULL)
        {
            printf("[can] !! task create failed (heap too small?)\r\n");
        }
        else
        {
            M4_TaskRegister("m3_can", (TaskHandle_t)tid);   /* 供监控任务查栈余量 */
        }
    }

    printf("[can] heap free after M3: %lu bytes\r\n",
           (unsigned long)xPortGetFreeHeapSize());
}
