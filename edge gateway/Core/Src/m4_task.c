/*==============================================================================
 * M4 · 系统监控任务实现
 *============================================================================*/

#include "m4_task.h"
#include "m4_pool.h"
#include "m4_log.h"
#include "m5_wdg.h"        /* M5：M5_WdgKick() 裁决喂狗 */
#include "m5_blackbox.h"   /* M5：M5_ParamSetConfirmed() App 确认字 */
#include "shared/flash_layout.h"   /* APP_CONFIRM_SEC */
#include "m5_fan.h"        /* M5：M5_FanUpdate() PWM 调速 */

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include "main.h"

/*==============================================================================
 * 任务登记表（供栈水线打印）
 *============================================================================*/
#define M4_REG_MAX   8

typedef struct {
    const char   *name;
    TaskHandle_t  handle;
} m4_reg_t;

static m4_reg_t  s_reg[M4_REG_MAX];
static uint8_t   s_reg_n;

static osEventFlagsId_t s_alive = NULL;

/* ⭐ 存活位的 volatile 镜像 —— 为什么必须有这个"第二份"：
      WWDG 的 EWI 中断回调（HAL_WWDG_EarlyWakeupCallback）里要读存活位来裁决，
      但 osEventFlagsGet() 是【线程级 API】，在 ISR 里调会进断言/临界区，不安全。
      单字节/单字的 volatile 读写在 Cortex-M 上是原子的，
      所以任务侧"置位时顺手更新镜像"，ISR 侧"只读镜像" —— 两边都不用关中断。 */
static volatile uint32_t s_alive_mirror;

void M4_TaskRegister(const char *name, TaskHandle_t handle)
{
    if (s_reg_n >= M4_REG_MAX) return;

    s_reg[s_reg_n].name   = name;
    s_reg[s_reg_n].handle = handle;
    s_reg_n++;
}

/* ⭐ 每个存活位最后一次报到的时刻（BUG-061 引入，替代"位图+清零"） */
static volatile uint32_t s_last[4];

void M4_AliveSet(uint32_t bit)
{
    uint32_t now = HAL_GetTick();
    uint8_t  i;
    static const uint32_t bits[4] = {
        M4_ALIVE_NET, M4_ALIVE_MODBUS, M4_ALIVE_CAN, M4_ALIVE_LOG
    };

    s_alive_mirror |= bit;          /* ⭐ 先更镜像，ISR 立刻可见 */

    /* ⭐ 关键：记下"最后一次报到"的时刻。
       看门狗裁决读的是这个时间戳，而不是"位图有没有被清零"。
       ⚠️ 必须在事件组之前更新：事件组是线程级 API，而时间戳谁都能读。 */
    for (i = 0u; i < 4u; i++)
    {
        if (bit & bits[i])
        {
            s_last[i] = now;
        }
    }

    if (s_alive != NULL)
    {
        osEventFlagsSet(s_alive, bit);
    }
}

uint32_t M4_AliveMissing(uint32_t window_ms)
{
    uint32_t now  = HAL_GetTick();
    uint32_t miss = 0u;
    uint8_t  i;
    static const uint32_t bits[4] = {
        M4_ALIVE_NET, M4_ALIVE_MODBUS, M4_ALIVE_CAN, M4_ALIVE_LOG
    };

    for (i = 0u; i < 4u; i++)
    {
        uint32_t last = s_last[i];

        /* ⚠️ last==0 = 这个任务【从来没报到过】，同样算失联。
           ⚠️ 用无符号减法 (now - last)：uint32 回绕时结果依然正确，
              不要写成 (now > last + window_ms) 那种朴素比较。 */
        if ((last == 0u) || ((now - last) > window_ms))
        {
            miss |= bits[i];
        }
    }
    return miss;
}

uint32_t M4_AliveGet(void)
{
    /* ⭐ 读镜像而不是 osEventFlagsGet()：ISR 安全 + 更快 */
    return (uint32_t)s_alive_mirror;
}

uint8_t M4_TaskSnapStack(uint32_t *out, uint8_t max)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0u; i < s_reg_n; i++)
    {
        if (n >= max) break;
        if (s_reg[i].handle == NULL) continue;

        out[n++] = (uint32_t)uxTaskGetStackHighWaterMark(s_reg[i].handle);
    }
    return n;
}

/*==============================================================================
 * 打印工具
 *============================================================================*/
static void print_stack_usage(void)
{
    uint8_t i;

    for (i = 0; i < s_reg_n; i++)
    {
        if (s_reg[i].handle == NULL) continue;

        /* ⚠️ 单位陷阱：uxTaskGetStackHighWaterMark 返回的是【word】数（1 word = 4 字节），
           而 CMSIS 的 attr.stack_size 单位是【字节】。所以这里要 ×4 才能和栈配置对比。 */
        uint32_t left_words = (uint32_t)uxTaskGetStackHighWaterMark(s_reg[i].handle);
        printf("[m4]   stack %-10s left=%5lu B\r\n",
               s_reg[i].name, (unsigned long)(left_words * (uint32_t)sizeof(StackType_t)));
    }
}

static void print_pool_summary(void)
{
    m4_pool_t snap;
    uint32_t  succ, rate;
    int16_t   ti, hi;

    M4_PoolGetSnapshot(&snap);

    ti = snap.temperature;
    hi = snap.humidity;

    succ = (snap.poll_total >= snap.poll_fail) ? (snap.poll_total - snap.poll_fail) : 0u;
    rate = (snap.poll_total > 0u) ? (succ * 100u / snap.poll_total) : 0u;

    /* MicroLIB 的 printf 不支持 %f，所有小数都要手工拆成整数部分 + 小数部分 */
    printf("[m4] T=%d.%dC H=%d.%d%% DI=0x%02X RL=0x%02X tgt=0x%02X | slv=%d,%d | %lu/%lu (%lu%%)\r\n",
           ti / 10, (ti < 0 ? -(ti % 10) : (ti % 10)),
           hi / 10, (hi < 0 ? -(hi % 10) : (hi % 10)),
           snap.di_status, snap.relay_status, snap.relay_target,
           snap.slave_online[0], snap.slave_online[1],
           succ, snap.poll_total, rate);

    printf("[m4] CAN rx=%lu/%lu drop=%lu/%lu bo=%u/%u | eth=%s | cmd %lu (bad %lu)\r\n",
           (unsigned long)snap.can_rx[0], (unsigned long)snap.can_rx[1],
           (unsigned long)snap.can_drop[0], (unsigned long)snap.can_drop[1],
           snap.can_busoff[0], snap.can_busoff[1],
           snap.eth_link_up ? "up" : "down",
           (unsigned long)snap.cmd_total, (unsigned long)snap.cmd_bad);
}

/*==============================================================================
 * 监控任务
 *============================================================================*/
static void m4_monitor_task(void *argument)
{
    uint32_t round = 0;
    uint8_t  confirmed = 0u;      /* M5：App 是否已写过确认字 */

    (void)argument;

    printf("[m4] monitor start, heap=%lu\r\n",
           (unsigned long)xPortGetFreeHeapSize());

    for (;;)
    {
        osDelay(M4_TASK_PERIOD_MS);
        round++;

        /* ---------- ① 存活位检查 ----------
           ⚠️ 只在每 M4_ALIVE_WINDOW_TICKS(5) 轮的那一轮检查并清零：
              monitor 是 1 s 一轮（喂狗裁决需要），但存活窗口是 5 s。
              若每 1 s 就清零，m1_report（自身 3 s 一循环）会被误判失联。
           ⚠️ 前两轮跳过：任务刚创建还没报到，此时检查必然全部 missing，
              会打出一条"系统一上来就有任务死了"的误报。 */
        if ((round > 1u) && ((round % M4_ALIVE_WINDOW_TICKS) == 0u))
        {
            /* ⭐ 用"时间戳超时"判定，不再读位图（BUG-061） */
            uint32_t miss = M4_AliveMissing(M4_ALIVE_WINDOW_MS);

            if (miss != 0u)
            {
                /* 有任务超过 5s 没报到 —— M5 的 M5_WdgKick() 会据此拒喂看门狗 */
                printf("[m4] !! alive missing: 0x%02lX", (unsigned long)miss);
                if (miss & M4_ALIVE_NET)    printf(" NET");
                if (miss & M4_ALIVE_MODBUS) printf(" MODBUS");
                if (miss & M4_ALIVE_CAN)    printf(" CAN");
                if (miss & M4_ALIVE_LOG)    printf(" LOG");
                printf("\r\n");
            }

            /* ⭐⭐ 【已删除清零操作】（BUG-061）。
               旧逻辑在这里清零位图，而本轮末尾就要调 M5_WdgKick()，
               Kick 读到的是刚被清空的位图 → 每 5 s 必然误拒喂一次。
               改成时间戳判定后，"卡死但位置着"照样能测出来：
               卡死的任务不会再调 M4_AliveSet()，它的时间戳会一直变老，
               超过窗口就被判失联 —— 比清零更可靠，还没有竞态。 */
        }

        /* ---------- ② uptime ---------- */
        M4_PoolSetUptime((uint32_t)(osKernelGetTickCount() / 1000u));

        /* ---------- ③ M5：App 确认字 ----------
           ⭐ 跑满 30 s 且自检通过（各任务都报到过）才写确认字。
              Bootloader 靠它区分"新固件能跑"和"新固件启动即挂"：
              没确认就复位 → reset_count 累加到 3 → 自动回滚。
           ⚠️ 确认的判据不能只是"启动了"：很多固件是"能启动但某个功能坏"。
              所以用存活位齐全作为判据。 */
        if ((confirmed == 0u) &&
            ((osKernelGetTickCount() / 1000u) >= APP_CONFIRM_SEC))
        {
            uint32_t miss = M4_AliveMissing(M4_ALIVE_WINDOW_MS) & M5_ALIVE_REQUIRED;

            if (miss == 0u)
            {
                if (M5_ParamSetConfirmed(1u) == 0)
                {
                    confirmed = 1u;
                    printf("[m4] app confirmed (uptime>=%us, all alive)\r\n",
                           (unsigned)APP_CONFIRM_SEC);
                }
            }
        }

        /* ---------- ④ M5：PWM 风扇调速（温度 → 占空比，带滞回） ---------- */
        M5_FanUpdate();

        /* ---------- ⑤ 内存与日志健康：每 5 s 一次 ---------- */
        if ((round % 5u) == 0u)
        {
            printf("[m4] heap free=%lu  min_ever=%lu  log_drop=%lu\r\n",
                   (unsigned long)xPortGetFreeHeapSize(),
                   (unsigned long)xPortGetMinimumEverFreeHeapSize(),
                   (unsigned long)M4_LogDropped());

            /* ---------- ⑥ 数据池摘要 ---------- */
            print_pool_summary();
        }

        /* ---------- ⑦ 栈余量：每 30s 一次（30 轮），避免刷屏 ---------- */
        if ((round % 30u) == 0u)
        {
            print_stack_usage();
        }

        /* ---------- ⑧ ⭐ M5 喂狗裁决：每 1 s 一次，全齐才喂 ---------- */
        M5_WdgKick();
    }
}

/*==============================================================================
 * 启动入口
 *============================================================================*/
void M4_TaskStart(void)
{
    /* ⚠️ 栈 1024 → 1280：
       ① 周期从 5 s 改成 1 s 后这条路走得更频繁
       ② 里面要调 M5_ParamSetConfirmed()（读擦写内部 Flash，局部缓冲）
       ③ print_pool_summary() 的 printf 在 ARMCC 下本身就要吃 200~400 B */
    static const osThreadAttr_t attr = {
        .name       = "m4_mon",
        .stack_size = 1280u,
        .priority   = (osPriority_t)osPriorityBelowNormal,
    };

    s_alive = osEventFlagsNew(NULL);
    if (s_alive == NULL)
    {
        printf("[m4] !! event flags create failed\r\n");
    }

    if (osThreadNew(m4_monitor_task, NULL, &attr) == NULL)
    {
        printf("[m4] !! monitor task create failed\r\n");
    }
}
