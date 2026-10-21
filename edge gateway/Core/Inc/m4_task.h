/*==============================================================================
 * M4 · 系统监控任务 + 存活事件组
 *
 * 两件事：
 *   ① 体检：周期性打印 heap 剩余 / 历史最低 / 各任务栈余量 / 数据池摘要
 *      —— M4-g 长稳验收和"栈余量 >30%"的数字全靠它
 *   ② 存活位（alive bits）：每个任务定期"报到"，monitor 汇总。
 *      M5 阶段把这里接上看门狗：全部报到才喂 IWDG，某一个没报到就不喂
 *      —— 这才是看门狗该有的样子（单任务喂狗是形同虚设的）
 *============================================================================*/

#ifndef __M4_TASK_H
#define __M4_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"       /* TaskHandle_t */

/*==============================================================================
 * 存活位定义（每个任务占一位）
 *============================================================================*/
#define M4_ALIVE_NET      (1u << 0)   /* 网络任务（命令服务 + 上报） */
#define M4_ALIVE_MODBUS   (1u << 1)   /* Modbus 轮询 */
#define M4_ALIVE_CAN      (1u << 2)   /* CAN 收发 */
#define M4_ALIVE_LOG      (1u << 3)   /* 日志任务 */
#define M4_ALIVE_ALL      (M4_ALIVE_NET | M4_ALIVE_MODBUS | M4_ALIVE_CAN | M4_ALIVE_LOG)

/* ⚠️ 存活判定窗口 与 monitor 周期【不是一回事】，别混：
     · M4_TASK_PERIOD_MS  = 1000：monitor 跑的节奏 = 【喂狗裁决周期】（M5 用它调 M5_WdgKick）
     · M4_ALIVE_WINDOW_MS = 5000：任务只要在 5 s 内报到过就算活着
   为什么窗口要比周期长：m1_report 这类任务自己就是 3 s 一个循环，
   若按 1 s 窗口要求它报到，它必然被误判失联。

   ⭐⭐ 判定方式已从【位图 + 清零】改成【时间戳】（BUG-061）：
       旧做法：monitor 每 5 s 读位图 → 打印 → 清零，然后同一轮末尾调 M5_WdgKick()。
       致命处：Kick 读到的是【刚刚被自己清零的空位图】→ 必然拒喂一次。
       实测日志就是 [wdg] !! missing 0x0D (streak 1) 每 5 s 刷一条，
       靠"下一次 kick 位图又满了"才侥幸没累加到 3 —— 纯粹运气，不是设计。

       新做法：每个任务报到时记下 HAL_GetTick()，判定时算 now-last 是否超窗口。
       好处：① 不需要"清零"这个破坏性动作，彻底消除清零/置位的竞态；
             ② Kick 什么时候读都读到同一份事实，与 monitor 的节奏无关；
             ③ 天然容忍 3 s 周期的任务（旧做法要小心错开周期）。 */
#define M4_ALIVE_WINDOW_MS   5000u
#define M4_TASK_PERIOD_MS    1000u    /* 5 s → 1 s（M5：喂狗裁决周期） */

/* 存活窗口折算成 monitor 的轮数（5000/1000 = 5） */
#define M4_ALIVE_WINDOW_TICKS  (M4_ALIVE_WINDOW_MS / M4_TASK_PERIOD_MS)

/*==============================================================================
 * API
 *============================================================================*/

/* 启动监控任务（freertos.c 里调用） */
void M4_TaskStart(void);

/* 任务自我登记（供 monitor 打印栈余量）。最多登记 8 个，满了就忽略。 */
void M4_TaskRegister(const char *name, TaskHandle_t handle);

/* 各任务在自己的循环里调用，报告"我还活着" */
void M4_AliveSet(uint32_t bit);

/* 查询当前存活位图。
   ⭐ ISR 安全：内部只读一个 volatile 镜像，不调 osEventFlagsGet()
      （后者是线程级 API，WWDG 的 EWI 中断回调里会调这个函数）。 */
uint32_t M4_AliveGet(void);

/* ⭐ 查询"窗口内没报到的任务位"（M5 看门狗裁决用这个，不要用 M4_AliveGet 反推）。
   window_ms：超时判据，通常传 M4_ALIVE_WINDOW_MS。
   ⚠️ ISR 安全：只读 volatile 时间戳数组 + HAL_GetTick()，不碰任何 RTOS 对象，
      所以 WWDG 的 EWI 中断回调里也能调。 */
uint32_t M4_AliveMissing(uint32_t window_ms);

/* 采集各登记任务的栈水线（单位 word，×4 才是字节）。
   返回实际填写的个数。供 M5 黑匣子在复位前记录现场。 */
uint8_t M4_TaskSnapStack(uint32_t *out, uint8_t max);

#endif /* __M4_TASK_H */
