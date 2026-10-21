/*==============================================================================
 * M4 · 日志子系统实现（见 m4_log.h 顶部说明）
 *============================================================================*/

#include "m4_log.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#include "usart.h"      /* huart1 */
#include "main.h"
#include "m4_task.h"    /* M4：存活位（M5 事件组喂狗会用它） */

/*==============================================================================
 * 环形缓冲
 *
 * 约定：head 是写指针（生产者 = fputc），tail 是读指针（消费者 = 发送任务）。
 *       留一个字节不用，用来区分"满"和"空"：
 *         head == tail        → 空
 *         (head+1)%N == tail  → 满
 *============================================================================*/
static uint8_t            s_buf[M4_LOG_BUF_SIZE];
static volatile uint16_t  s_head;
static volatile uint16_t  s_tail;
static volatile uint32_t  s_dropped;
static volatile int       s_ready;

/*==============================================================================
 * 内部：往缓冲塞一个字节
 *============================================================================*/
static void log_push(uint8_t c)
{
    uint16_t next;

    /* 调度器还没跑（比如在 main() 早期或 MX_FREERTOS_Init 里打的日志）：
       这时不能用临界区（taskENTER_CRITICAL 依赖调度器状态），
       直接用阻塞发送兜底 —— 反正此时也不存在多任务竞争。 */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        HAL_UART_Transmit(&huart1, &c, 1, 100);
        return;
    }

    if (!s_ready)
    {
        /* 发送任务还没建起来，同样走兜底 */
        HAL_UART_Transmit(&huart1, &c, 1, 100);
        return;
    }

    next = (uint16_t)((s_head + 1u) % M4_LOG_BUF_SIZE);

    /* ⚠️ 满了就丢。绝不能在这里等 —— 等的话 printf 就又变成"慢操作"了，
       而且如果是在持锁的临界区里 printf，等待 = 死锁风险。 */
    if (next == s_tail)
    {
        s_dropped++;
        return;
    }

    /* 临界区只包住"改指针 + 写字节"这 3 行，是微秒级的，
       不会像"关中断包住整行 printf"那样堵住 ISR 几毫秒。 */
    taskENTER_CRITICAL();
    s_buf[s_head] = c;
    s_head = next;
    taskEXIT_CRITICAL();
}

void M4_LogPutc(char c)
{
    log_push((uint8_t)c);
}

uint32_t M4_LogDropped(void)
{
    return (uint32_t)s_dropped;
}

int M4_LogReady(void)
{
    return s_ready;
}

/*==============================================================================
 * 发送任务
 *
 * ⚠️ 优先级必须低于业务任务（BelowNormal）：
 *    打印不该抢占采集和通信。缓冲吸收了突发，晚几毫秒发出去没人关心。
 *============================================================================*/
static void m4_log_task(void *argument)
{
    uint8_t  out[M4_LOG_CHUNK];

    (void)argument;

    s_ready = 1;

    for (;;)
    {
        uint16_t n = 0;

        /* ⭐ 存活位：每转一圈就置一次。
           本任务只要还能循环，就说明日志通路没卡死 —— 这正是"存活"的定义。
           ⚠️ 之前漏了这一行，导致 monitor 一直报 "alive missing ... LOG"（实测确认）。 */
        M4_AliveSet(M4_ALIVE_LOG);

        /* 取数据时也要短暂临界区：head/tail 都是 volatile，
           但"读一个字节 + 改 tail"必须原子，否则和生产者竞争会重复读或漏读。 */
        while (n < (uint16_t)sizeof(out))
        {
            uint8_t c;
            taskENTER_CRITICAL();
            if (s_tail == s_head)
            {
                taskEXIT_CRITICAL();
                break;
            }
            c = s_buf[s_tail];
            s_tail = (uint16_t)((s_tail + 1u) % M4_LOG_BUF_SIZE);
            taskEXIT_CRITICAL();

            out[n++] = c;
        }

        if (n > 0)
        {
            /* 阻塞发送是安全的：这是独立任务，慢一点只影响日志，不影响业务。
               ⚠️ 但不要无限等（HAL_MAX_DELAY）—— 串口被拔掉/异常时不能把任务卡死，
               这里给 100ms 超时。 */
            HAL_UART_Transmit(&huart1, out, n, 100);
        }
        else
        {
            /* 没数据：让出 CPU。
               ⚠️ 不能空转 while，否则这个 BelowNormal 任务会把 CPU 吃干。 */
            osDelay(M4_LOG_POLL_MS);
        }
    }
}

/*==============================================================================
 * 启动入口
 *============================================================================*/
void M4_LogStart(void)
{
    static const osThreadAttr_t attr = {
        .name       = "m4_log",
        .stack_size = M4_LOG_TASK_STACK,
        .priority   = (osPriority_t)osPriorityBelowNormal,
    };

    s_head = 0;
    s_tail = 0;
    s_dropped = 0;

    if (osThreadNew(m4_log_task, NULL, &attr) == NULL)
    {
        /* 建不出来就保持兜底路径（直接阻塞发送），系统照常能跑，只是日志有风险 */
        s_ready = 0;
    }
}
