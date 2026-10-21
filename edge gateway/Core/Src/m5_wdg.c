/*==============================================================================
 * M5 · 看门狗裁决（IWDG 事件组裁决 + WWDG EWI 窗口喂狗）
 *============================================================================*/

#include "m5_wdg.h"
#include "m4_task.h"
#include "m5_blackbox.h"

#include "iwdg.h"       /* hiwdg —— IWDG 句柄 */
#include "wwdg.h"       /* hwwdg —— WWDG 句柄 */
#include "stm32f4xx_hal.h"

#include <stdio.h>

/*==============================================================================
 * 内部状态
 *============================================================================*/
static uint32_t s_kick_cnt;         /* 裁决次数（用于启动宽限判断） */
static uint32_t s_miss_streak;      /* 连续缺位次数 */
static uint32_t s_hold_until;       /* hold 宽限截止时刻（HAL_GetTick 域），0 = 无 */
static uint32_t s_refuse_cnt;       /* 累计拒喂次数（诊断用） */

/*==============================================================================
 * 初始化
 *============================================================================*/
void M5_WdgInit(void)
{
    s_kick_cnt    = 0u;
    s_miss_streak = 0u;
    s_hold_until  = 0u;
    s_refuse_cnt  = 0u;
}

/*==============================================================================
 * hold 宽限
 *============================================================================*/
void M5_WdgHold(uint32_t hold_ms)
{
    uint32_t end = HAL_GetTick() + hold_ms;

    /* 只在新的 deadline 更晚时才更新，避免短宽限把长宽限覆盖掉。
       ⚠️ 用有符号差值比较，正确处理 HAL_GetTick() 的 49 天回绕。 */
    if ((s_hold_until == 0u) || ((int32_t)(end - s_hold_until) > 0))
    {
        s_hold_until = end;
    }
}

int M5_WdgIsHolding(void)
{
    if (s_hold_until == 0u) return 0;
    return ((int32_t)(s_hold_until - (uint32_t)HAL_GetTick()) > 0) ? 1 : 0;
}

/*==============================================================================
 * IWDG 裁决喂狗 —— ⭐ 全工程唯一的 HAL_IWDG_Refresh() 调用点
 *============================================================================*/
void M5_WdgKick(void)
{
    uint32_t alive;
    uint32_t miss;

    s_kick_cnt++;

    /* ⭐ 用"时间戳超时"判定（BUG-061），不要用 M4_AliveGet() 反推：
       位图是"曾经报到过"的累积，不清零就永远为真，清零又会踩到
       "清零后立刻裁决 = 读到空位图"的坑。时间戳才是无歧义的事实。 */
    alive = M4_AliveGet();
    miss  = M4_AliveMissing(M4_ALIVE_WINDOW_MS) & M5_ALIVE_REQUIRED;

    /* ---------- ① 全齐 → 正常喂狗 ---------- */
    if (miss == 0u)
    {
        s_miss_streak = 0u;
        HAL_IWDG_Refresh(&hiwdg);          /* ⭐ 唯一允许的喂狗点 */
        return;
    }

    /* ---------- ② 处于 hold 宽限（正在擦 Flash 等有界操作）→ 照喂 ---------- */
    if (M5_WdgIsHolding())
    {
        printf("[wdg] hold: missing 0x%02lX but holding, feed anyway\r\n",
               (unsigned long)miss);
        HAL_IWDG_Refresh(&hiwdg);
        return;
    }

    /* ---------- ③ 启动宽限期内 → 只提示，仍然喂 ----------
       ⚠️ 为什么必须有：上电时 lwIP 初始化要几百 ms，各任务还没来得及报到。
          没有宽限期 → 系统一上电就拒喂 → 永远起不来。 */
    if (s_kick_cnt <= M5_GRACE_KICKS)
    {
        printf("[wdg] grace: waiting for 0x%02lX (kick %lu/%u)\r\n",
               (unsigned long)miss, (unsigned long)s_kick_cnt,
               (unsigned)M5_GRACE_KICKS);
        HAL_IWDG_Refresh(&hiwdg);
        return;
    }

    /* ---------- ④ 宽限期后仍缺位 → 拒喂，让它复位 ---------- */
    s_miss_streak++;
    s_refuse_cnt++;

    printf("[wdg] !! missing 0x%02lX (streak %lu), REFUSE to feed\r\n",
           (unsigned long)miss, (unsigned long)s_miss_streak);

    /* ⭐ 死因只写一次：streak==1 时写。
       写 Flash 要擦扇区（慢 + 有寿命），而复位是必然发生的，写一次足够；
       每秒重复写既拖慢停机过程又磨损 Flash。 */
    if (s_miss_streak == 1u)
    {
        M5_BlackboxWrite(miss, alive);
    }

    /* 不喂狗 → 最多再过 T_timeout（最快 2.72 s）就复位。
       期间 monitor 继续跑，把栈/堆信息打出来 —— 这些日志是排障的关键。 */
}

/*==============================================================================
 * WWDG EWI 回调 —— 真正的"节奏监护"
 *
 * ⚠️ 这是【中断上下文】：EWI 触发时距复位只剩 1 个 tick（0.78 ms）。
 *    所以这里只能做"读一个 volatile 变量 + 一次 Refresh"，
 *    ⛔ 绝不能 printf / 写 Flash / 调任何 RTOS 阻塞 API。
 *
 * ⚠️ 为什么喂狗时刻天然合法：
 *    EWI 在 counter == 0x40 (=64) 时触发，而喂狗合法条件是 counter ≤ Window。
 *    本工程 Window = 80 → 64 ≤ 80 ✅
 *    ⚠️ 因此 Window 必须 ≥ 0x40(64)。把 Window 改小到 48 会导致"一喂就复位"，
 *       而且窗口永远打不开（counter 还没降到 48 就已经在 49.9 ms 复位了）。
 *============================================================================*/
void HAL_WWDG_EarlyWakeupCallback(WWDG_HandleTypeDef *hwwdg_arg)
{
    uint32_t alive;

    /* ⚠️ 这里读的是 m4_task.c 里维护的 volatile 镜像，不是 osEventFlagsGet()。
       osEventFlagsGet() 是线程级 API，在 ISR 里调用不安全（会进断言/临界区）。
       镜像是单字读写，在 Cortex-M 上天然原子。 */
    /* ⭐ 启动宽限：上电后前 10 s 无条件续命。
       ⚠️ 没有这段会【无限重启】：WWDG 在 main 里就启动了，而各任务要几百毫秒
          才陆续起来。EWI 第一次触发（约 49 ms）时存活位必然是空的 → 不喂 →
          50 ms 就复位 → 再上电 → 再复位，板子永远起不来。
       IWDG 那边有 s_kick_cnt <= M5_GRACE_KICKS 做同样的事，这里必须配套。 */
    if (HAL_GetTick() < 10000u)
    {
        HAL_WWDG_Refresh(hwwdg_arg);
        return;
    }

    alive = M4_AliveGet();

    if (((M5_ALIVE_REQUIRED & (~alive)) == 0u) || M5_WdgIsHolding())
    {
        HAL_WWDG_Refresh(hwwdg_arg);    /* 全部报到 → 续命 */
    }
    /* 否则什么都不做 → 0.78 ms 后 WWDG 复位。
       ⚠️ 注意：WWDG 复位比 IWDG 快得多，专门用来抓"关中断死循环"这类
          IWDG 来不及反应的故障。 */
}
