/*==============================================================================
 * M2 · RS485 半双工驱动实现
 *
 * 三件事：
 *   ① 方向切换（半双工最容易翻车的地方）
 *   ② DMA 循环接收（硬件一直在搬，软件只管取）
 *   ③ 帧边界判定（IDLE 中断给边界 + t3.5 静默确认）
 *============================================================================*/

#include "m2_rs485.h"
#include "cmsis_os.h"
#include <string.h>

extern UART_HandleTypeDef huart6;
extern DMA_HandleTypeDef  hdma_usart6_rx;

/* DMA 一直在往这里搬数据。软件只读不写。 */
static uint8_t g_rx_dma_buf[RS485_RX_BUF_SIZE];

/*------------------------------------------------------------------------------
 * 下面这几个是「中断写、任务读」的共享量，必须 volatile：
 * 否则编译器认为没人改它，会把读取优化成只读一次 → 永远看不到新数据。
 *----------------------------------------------------------------------------*/
static volatile uint32_t g_idle_cnt  = 0;   /* IDLE 中断累计触发次数 */
static volatile uint16_t g_idle_pos  = 0;   /* 最近一次 IDLE 时 DMA 搬到的位置 */
static volatile uint32_t g_idle_tick = 0;   /* 最近一次 IDLE 的时刻(ms) */

/* 任务侧游标 */
static uint16_t g_last_pos        = 0;      /* 已经被取走的位置 */
static uint32_t g_last_idle_cnt   = 0;      /* 已经消费过的 IDLE 次数 */
static uint16_t g_last_seen_pos   = 0;      /* 上次观察到"位置变了"时的位置 */
static uint32_t g_last_change_tick = 0;     /* 上次发现位置变化的时刻 */

/*------------------------------------------------------------------------------
 * 当前 DMA 已经搬了多少字节
 *
 * NDTR = 还剩多少个没搬。它是递减的：启动后 = 缓冲长度，搬一个减一个，
 * circular 模式下减到 0 会自动重载为缓冲长度，如此循环。
 *   已搬字节数 = 缓冲长度 − NDTR
 *----------------------------------------------------------------------------*/
static uint16_t rs485_dma_pos(void)
{
    return (uint16_t)(RS485_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart6_rx));
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void RS485_Init(void)
{
    g_last_pos         = 0;
    g_last_seen_pos    = 0;
    g_last_idle_cnt    = 0;
    g_idle_cnt         = 0;
    g_idle_pos         = 0;
    g_idle_tick        = HAL_GetTick();
    g_last_change_tick = HAL_GetTick();

    RS485_RX_EN();   /* 上电必须处于接收态，否则会一直占用总线，从机永远不敢回话 */

    /* ⚠️ 顺序：先启动 DMA，再开 IDLE。
       反过来的话，DMA 还没开始搬就来 IDLE 中断，会记录到一个错误的帧边界。 */
    HAL_UART_Receive_DMA(&huart6, g_rx_dma_buf, RS485_RX_BUF_SIZE);

    /* HAL_UART_Receive_DMA 只开 DMA 和串口的 DMAR 位，不会开 IDLE，必须自己加 */
    __HAL_UART_ENABLE_IT(&huart6, UART_IT_IDLE);
}

/*==============================================================================
 * IDLE 回调（在 USART6_IRQHandler 里被调用，运行在中断上下文）
 *
 * IDLE = 串口 RX 线上检测到「一个字符时间没有新起始位」。
 * ⚠️ 注意它是 1 个字符时间，不是 3.5 个 —— 所以 IDLE 只能当"候选帧边界"，
 *    还必须再用 t3.5 静默确认一次，否则遇到「字节间有间隙的慢从机」会把
 *    一帧切成两半（典型症状：CRC 永远错）。
 *============================================================================*/
void RS485_IdleCallback(void)
{
    g_idle_pos  = rs485_dma_pos();
    g_idle_tick = HAL_GetTick();
    g_idle_cnt++;
}

/*==============================================================================
 * 取一帧（非阻塞，返回 0 表示"还没收完"）
 *============================================================================*/
uint16_t RS485_ReadFrame(uint8_t *out, uint16_t maxlen)
{
    uint16_t used = rs485_dma_pos();
    uint32_t now  = HAL_GetTick();
    uint16_t end_pos = 0;
    uint8_t  got_end = 0;

    /* 有新字节进来 → 记下时刻，用于下面的静默计时 */
    if (used != g_last_seen_pos)
    {
        g_last_seen_pos   = used;
        g_last_change_tick = now;
    }

    /* ---- 判据一（主）：IDLE 给出边界 + 之后又静默了 t3.5 ---- */
    if (g_idle_cnt != g_last_idle_cnt)
    {
        if ((now - g_idle_tick) >= RS485_T35_MS && used == g_idle_pos)
        {
            end_pos = g_idle_pos;
            got_end = 1;
        }
    }

    /* ---- 判据二（兜底）：IDLE 根本没来，但位置已稳定超过 t3.5 ---- */
    if (!got_end && (used != g_last_pos) && ((now - g_last_change_tick) >= RS485_T35_MS))
    {
        end_pos = used;
        got_end = 1;
    }

    if (!got_end) return 0;

    g_last_idle_cnt = g_idle_cnt;

    /* ---- 从循环缓冲取出 [g_last_pos, end_pos) ---- */
    uint16_t len = (end_pos >= g_last_pos)
                 ? (uint16_t)(end_pos - g_last_pos)
                 : (uint16_t)(RS485_RX_BUF_SIZE - g_last_pos + end_pos);

    if (len == 0) { g_last_pos = end_pos; return 0; }

    /* 调用者给的缓冲不够大时截断（多余字节当作丢弃，游标照样推到 end_pos） */
    if (len > maxlen) len = maxlen;

    if ((uint32_t)g_last_pos + len <= RS485_RX_BUF_SIZE)
    {
        memcpy(out, &g_rx_dma_buf[g_last_pos], len);
    }
    else
    {
        uint16_t first = (uint16_t)(RS485_RX_BUF_SIZE - g_last_pos);
        memcpy(out,                 &g_rx_dma_buf[g_last_pos], first);
        memcpy(out + first,         &g_rx_dma_buf[0],          (uint16_t)(len - first));
    }

    g_last_pos = end_pos;
    return len;
}

/*==============================================================================
 * 丢弃当前已收到的所有字节
 * 每次发请求前必须调用：否则上一次残留的字节会被当成这一次响应的开头。
 *============================================================================*/
void RS485_RxFlush(void)
{
    uint16_t used = rs485_dma_pos();

    g_last_pos         = used;
    g_last_seen_pos    = used;
    g_last_change_tick = HAL_GetTick();
    g_last_idle_cnt    = g_idle_cnt;   /* 连 IDLE 计数一起拉平，避免误消费旧边界 */
}

/*==============================================================================
 * 等总线静默 ≥ t3.5
 * 主机连续发两帧之间必须留这个间隔，否则从机会把两帧当成一帧。
 *============================================================================*/
void RS485_WaitSilent(void)
{
    uint32_t t0 = HAL_GetTick();

    for (;;)
    {
        uint16_t used = rs485_dma_pos();
        if (used != g_last_seen_pos)
        {
            g_last_seen_pos    = used;
            g_last_change_tick = HAL_GetTick();
            t0                 = HAL_GetTick();
        }

        if ((HAL_GetTick() - g_last_change_tick) >= RS485_T35_MS) break;
        if ((HAL_GetTick() - t0) > 50) break;   /* 兜底退出，任何情况都不死等 */

        osDelay(1);
    }
}

/*==============================================================================
 * 阻塞发送（Modbus 帧都很短，8~20 字节，阻塞发送最简单也最不容易出错）
 *
 * ⚠️ 本函数最核心的一行是等 TC 那一行。
 *    TXE = 发送数据寄存器空 → 只说明"最后一个字节已经被搬进移位寄存器"，
 *          此时字节还在往外发，最后一两个位根本没上总线。
 *    TC  = 发送完成           → 说明"移位寄存器也空了"，字节已经全部发完。
 *    如果在 TXE 时就切回接收态，从机收到的是被截断的帧 → 不响应或 CRC 错。
 *    这种错误是间歇性的（跟波特率、中断时机有关），极难排查。
 *============================================================================*/
void RS485_Send(const uint8_t *buf, uint16_t len)
{
    RS485_TX_EN();                                          /* ① 进入发送态 */

    HAL_UART_Transmit(&huart6, (uint8_t *)buf, len, 500);   /* ② 发数据 */

    /* ③ 等最后一个字节真正发出去（HAL_UART_Transmit 内部也会等，这里是双保险：
          不同 HAL 版本行为不一致，宁可多等一次也不要少等） */
    while (__HAL_UART_GET_FLAG(&huart6, UART_FLAG_TC) == RESET) { }
    __HAL_UART_CLEAR_FLAG(&huart6, UART_FLAG_TC);

    RS485_RX_EN();                                          /* ④ 立刻回到接收态 */
}
