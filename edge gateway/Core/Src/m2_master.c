/*==============================================================================
 * M2 · Modbus 主站调度层实现
 *
 * 核心设计（这三条是能写进简历的东西）：
 *   ① 三态状态机 ONLINE → SUSPECT → OFFLINE，避免"偶发一次 CRC 错"就判离线
 *   ② 离线降频：坏设备从 100ms 改成 1s 探一次，否则它每次吃掉 200ms 超时，
 *      会把整条总线的吞吐拖垮
 *   ③ 脏位捎带：上位机的写指令不抢占总线，写进数据池后由轮询节拍带走，
 *      写完必须回读确认才清脏位
 *
 * ⚠️ M4 改动：
 *   - 数据池改用 M4 的 g_pool（m4_pool.h），锁由 m4_pool 统一提供
 *   - 每次采集都写入【时间戳】，上位机据此判断数据是否过期
 *   - 调度改用 vTaskDelayUntil，消除周期累积漂移
 *   - 每轮向 M4 存活事件组报到（M5 接看门狗用）
 *============================================================================*/

#include "m2_master.h"
#include "m2_modbus.h"
#include "m2_rs485.h"
#include "m4_pool.h"
#include "m4_task.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"      /* xPortGetFreeHeapSize / vTaskDelayUntil */
#include <stdio.h>

/*==============================================================================
 * 从机状态机
 *============================================================================*/
typedef enum {
    SLAVE_ONLINE = 0,   /* 正常 */
    SLAVE_SUSPECT,      /* 怀疑：失败 1~2 次，可能是干扰，继续按正常周期试 */
    SLAVE_OFFLINE,      /* 确认离线：降频探测 */
} slave_state_t;

typedef struct {
    uint8_t       addr;
    slave_state_t state;
    uint8_t       fail_cnt;
    uint32_t      last_poll;
    uint32_t      period;
} slave_t;

static slave_t g_slaves[MB_SLAVE_NUM] = {
    { MB_SLAVE_TEMP_HUMI, SLAVE_ONLINE, 0, 0, M2_POLL_PERIOD_MS },
    { MB_SLAVE_IO,        SLAVE_ONLINE, 0, 0, M2_POLL_PERIOD_MS },
};

/*==============================================================================
 * 数据池访问（M4 起全部转发到 m4_pool）
 *============================================================================*/
void M2_PoolLock(void)   { M4_PoolLock(); }
void M2_PoolUnlock(void) { M4_PoolUnlock(); }

void M2_GetSnapshot(m2_datapool_t *out)
{
    M4_PoolGetSnapshot(out);
}

void M2_SetRelayTarget(uint8_t bitmap)
{
    M4_PoolSetRelayTarget(bitmap);
}

/*==============================================================================
 * 打印节流
 *   M4 起 printf 走 m4_log 的环形缓冲（不再丢行），但仍然要节流：
 *   9600 的 Modbus 每 100ms 一轮，全打出来串口会被刷爆，人也看不过来。
 *============================================================================*/
static uint32_t g_last_dump = 0;
#define M2_DUMP_INTERVAL_MS  5000

/*==============================================================================
 * 温湿度从机：读 2 个寄存器（温度、湿度，各放大 10 倍）
 *============================================================================*/
static mb_status_t poll_temp_humi(uint8_t addr)
{
    uint16_t regs[2] = {0, 0};

    mb_status_t st = (M2_TEMP_FC == MB_FC_READ_INPUT)
                   ? MB_ReadInputRegs(addr, 0x0000, 2, regs, M2_RESP_TIMEOUT_MS)
                   : MB_ReadHoldingRegs(addr, 0x0000, 2, regs, M2_RESP_TIMEOUT_MS);

    if (st == MB_OK)
    {
        /* ⭐ 一次调用同时写值和时间戳 —— 保证两者是一致的（同一把锁内） */
        M4_PoolSetTempHumi((int16_t)regs[0], (int16_t)regs[1]);
    }
    return st;
}

/*==============================================================================
 * IO 从机：① 读 8 路 DI  ② 有脏位就写 8 路继电器  ③ 写完回读确认
 *============================================================================*/
static mb_status_t poll_io(uint8_t addr)
{
    uint8_t   bits[8];
    mb_status_t st;

    /* ---- ① 读 DI（02 读离散输入）---- */
    st = MB_ReadDiscreteInputs(addr, 0x0000, 8, bits, M2_RESP_TIMEOUT_MS);
    if (st == MB_OK)
    {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) if (bits[i]) v |= (uint8_t)(1u << i);
        M4_PoolSetDI(v);
    }

    /* ---- ② 脏位：把上位机的写指令捎带下去 ---- */
    uint8_t target, dirty;
    target = M4_PoolGetRelayTarget(&dirty);

    if (dirty)
    {
        uint8_t want[8];
        for (int i = 0; i < 8; i++) want[i] = (uint8_t)((target >> i) & 0x01);

        /* 用 0F 一次写 8 路，比 05 逐路写省 8 倍总线时间 */
        mb_status_t wst = MB_WriteMultiCoils(addr, 0x0000, 8, want, M2_RESP_TIMEOUT_MS);

        if (wst == MB_OK)
        {
            /* ---- ③ 回读确认：写完必须读回来比对，一致才清脏位 ---- */
            uint8_t cur_bits[8];
            mb_status_t rst = MB_ReadCoils(addr, 0x0000, 8, cur_bits, M2_RESP_TIMEOUT_MS);

            if (rst == MB_OK)
            {
                uint8_t cur = 0;
                for (int i = 0; i < 8; i++) if (cur_bits[i]) cur |= (uint8_t)(1u << i);

                M4_PoolSetRelayActual(cur);
                if (cur == target)
                {
                    M4_PoolClearRelayDirty();   /* 目标达成才清脏位 */
                }
            }
        }
        else
        {
            printf("[mb] relay write failed: %s\r\n", MB_StrError(wst));
        }
    }

    return st;
}

/*==============================================================================
 * 状态机更新
 *============================================================================*/
static void slave_update(slave_t *s, mb_status_t st)
{
    M4_PoolAddPoll((st == MB_OK) ? 1u : 0u);

    if (st == MB_OK)
    {
        s->fail_cnt = 0;
        if (s->state != SLAVE_ONLINE)
            printf("[mb] slave %d back ONLINE\r\n", s->addr);
        s->state  = SLAVE_ONLINE;
        s->period = M2_POLL_PERIOD_MS;
    }
    else
    {
        s->fail_cnt++;

        if (s->fail_cnt >= M2_FAIL_THRESHOLD)
        {
            if (s->state != SLAVE_OFFLINE)
                printf("[mb] slave %d OFFLINE (%s, exc=0x%02X)\r\n",
                       s->addr, MB_StrError(st), MB_LastException());
            s->state  = SLAVE_OFFLINE;
            s->period = M2_OFFLINE_PERIOD_MS;   /* ⭐ 降频 */
        }
        else
        {
            s->state = SLAVE_SUSPECT;
        }
    }
}

/*==============================================================================
 * 单次轮询某个从机
 *============================================================================*/
static void poll_one_slave(slave_t *s)
{
    mb_status_t st = (s->addr == MB_SLAVE_TEMP_HUMI) ? poll_temp_humi(s->addr)
                                                     : poll_io(s->addr);
    slave_update(s, st);
}

/*==============================================================================
 * 在线状态同步进数据池
 *============================================================================*/
static void sync_online_flags(void)
{
    for (int i = 0; i < MB_SLAVE_NUM; i++)
    {
        M4_PoolSetSlaveOnline((uint8_t)i,
                              (g_slaves[i].state == SLAVE_ONLINE) ? 1u : 0u);
    }
}

/*==============================================================================
 * 状态汇总打印（整数拆分，不用 %f —— MicroLIB 的 printf 不支持浮点）
 *============================================================================*/
void M2_DumpStatus(void)
{
    m2_datapool_t dp;
    M2_GetSnapshot(&dp);

    int16_t ti = dp.temperature;
    int16_t hi = dp.humidity;

    uint32_t succ = (dp.poll_total >= dp.poll_fail) ? (dp.poll_total - dp.poll_fail) : 0;
    uint32_t rate = (dp.poll_total > 0) ? (succ * 100u / dp.poll_total) : 0;

    printf("[mb] T=%d.%dC H=%d.%d%% DI=0x%02X RL=0x%02X | online=%d,%d | %lu/%lu ok (%lu%%)\r\n",
           ti / 10, (ti < 0 ? -(ti % 10) : (ti % 10)),
           hi / 10, (hi < 0 ? -(hi % 10) : (hi % 10)),
           dp.di_status, dp.relay_status,
           dp.slave_online[0], dp.slave_online[1],
           succ, dp.poll_total, rate);
}

/*==============================================================================
 * 轮询任务
 *============================================================================*/
static void m2_poll_task(void *argument)
{
    TickType_t prev;

    (void)argument;

    printf("[mb] poll task start, 9600/8N1, slaves=%d\r\n", MB_SLAVE_NUM);

    /* ⭐ vTaskDelayUntil 的基准时刻必须在进入循环前取一次。
       它保证"两次唤醒之间"恰好 M2_TICK_MS，把循环体耗时补偿进去；
       用 osDelay/vTaskDelay 的话实际周期 = 10ms + 循环体耗时，跑一小时能漂掉十几秒。 */
    prev = xTaskGetTickCount();

    for (;;)
    {
        uint32_t now = HAL_GetTick();

        for (int i = 0; i < MB_SLAVE_NUM; i++)
        {
            slave_t *s = &g_slaves[i];

            /* ⚠️ 用减法做时间比较，天然免疫 tick 溢出（32 位约 49 天回绕） */
            if ((now - s->last_poll) >= s->period)
            {
                s->last_poll = now;
                poll_one_slave(s);
            }
        }

        sync_online_flags();

        if ((HAL_GetTick() - g_last_dump) >= M2_DUMP_INTERVAL_MS)
        {
            g_last_dump = HAL_GetTick();
            M2_DumpStatus();
        }

        /* 向 M4 存活事件组报到：M5 阶段"全部任务报到才喂看门狗"要用 */
        M4_AliveSet(M4_ALIVE_MODBUS);

        vTaskDelayUntil(&prev, pdMS_TO_TICKS(M2_TICK_MS));
    }
}

/*==============================================================================
 * 【M2-a 硬件自检模式】RS485 物理层连通性测试
 *
 * 用法：
 *   1. m2_master.h 里 M2_TEST_RAW 改成 1，重新编译烧录
 *   2. 两个从机都断开，只把 PC 的 USB 转 485 接到板子的 A/B 上
 *   3. PC 串口助手开 9600/8N1：应每秒收到 01 02 03 04 05
 *   4. PC 随便发一串数据：板子 USART1 应打印 [mb] rx N: XX XX ...
 *   5. 验完把 M2_TEST_RAW 改回 0
 *
 * ⚠️ 这一步不通就别往下走。排查顺序：A/B 是否接反 → 波特率 → PE5 方向脚 → 模块供电
 *============================================================================*/
#if M2_TEST_RAW
static void m2_raw_test_task(void *argument)
{
    (void)argument;
    static const uint8_t probe[5] = {0x01, 0x02, 0x03, 0x04, 0x05};

    printf("[mb] RAW TEST: 9600/8N1, send 01 02 03 04 05 every 1s\r\n");

    for (;;)
    {
        RS485_WaitSilent();
        RS485_RxFlush();
        RS485_Send(probe, 5);

        uint8_t  b[64];
        uint16_t n;
        uint32_t t0 = HAL_GetTick();

        while ((HAL_GetTick() - t0) < 900)
        {
            n = RS485_ReadFrame(b, sizeof(b));
            if (n > 0)
            {
                printf("[mb] rx %u:", n);
                for (uint16_t i = 0; i < n; i++) printf(" %02X", b[i]);
                printf("\r\n");
            }
            osDelay(1);
        }
    }
}
#endif /* M2_TEST_RAW */

/*==============================================================================
 * 启动入口（freertos.c 里调用）
 *============================================================================*/
void M2_MasterStart(void)
{
    osThreadId_t tid;

    MB_Init();            /* RS485 + 总线互斥量 */
    M4_PoolInit();        /* M4 数据池互斥量（幂等，多次调用无副作用） */

    static const osThreadAttr_t attr = {
        .name       = "mbPoll",
        .stack_size = 1536u,     /* printf 已移到 m4_log，栈需求从 2KB 降到 1.5KB */
        .priority   = (osPriority_t)osPriorityNormal,
    };

    tid = osThreadNew(
#if M2_TEST_RAW
                    m2_raw_test_task,
#else
                    m2_poll_task,
#endif
                    NULL, &attr);

    if (tid == NULL)
    {
        printf("[mb] !! poll task create failed (heap out of memory?)\r\n");
    }
    else
    {
        /* 登记给 M4 监控任务，用来打印栈余量 */
        M4_TaskRegister("mbPoll", (TaskHandle_t)tid);
    }

    /* ⚠️ configTOTAL_HEAP_SIZE 只有 20KB，任务栈全从这里出。
       这行打印是自检手段：如果这个数字接近 0，说明堆快炸了。 */
    printf("[mb] heap free after task create: %lu bytes\r\n",
           (unsigned long)xPortGetFreeHeapSize());
}
