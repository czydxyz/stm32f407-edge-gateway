/*==============================================================================
 * M2 · Modbus 主站调度层 —— 对外接口
 *
 * ⚠️ M4 改动：数据池已统一到 M4 的 g_pool（见 m4_pool.h）。
 *    M2 不再自己维护一份 m2_datapool_t，这里的 m2_datapool_t 只是 m4_pool_t
 *    的别名，用来兼容早期代码与文档。锁也统一由 m4_pool 管理（互斥量，
 *    带优先级继承）。
 *
 * 这一层回答三个问题：
 *   ① 什么时候轮到哪个从机说话（调度）
 *   ② 从机坏了怎么办（三态状态机 + 降频）
 *   ③ 上位机想写继电器怎么办（脏位，捎带进同步节拍，绝不抢占总线）
 *============================================================================*/

#ifndef __M2_MASTER_H
#define __M2_MASTER_H

#include <stdint.h>
#include "m2_modbus.h"
#include "m4_pool.h"

/* 兼容旧名：M2 的数据池就是 M4 的统一数据池 */
typedef m4_pool_t m2_datapool_t;

/* ---------- 从机地址 ---------- */
#define MB_SLAVE_TEMP_HUMI   1      /* 温湿度变送器 */
#define MB_SLAVE_IO          2      /* 8DI + 8DO 继电器模块 */
#define MB_SLAVE_NUM         2

/* ⚠️ 温湿度用输入寄存器(04)还是保持寄存器(03)，以你的从机说明书为准。
      国产模块两种都有。读出来若是 65535 或恒为 0，先改这个宏再查别的。 */
#ifndef M2_TEMP_FC
#define M2_TEMP_FC           MB_FC_READ_INPUT
#endif

/* ⚠️ 硬件自检模式：置 1 时不跑 Modbus 轮询，改成「每 1s 发 01 02 03 04 05，
   并把收到的字节以 16 进制打印出来」。
   用途：**先不接从机**，只接 PC 的 USB 转 485，用串口助手验证 RS485 物理层通不通。
   看到字节 = 硬件通（M2-a 验收）。验完务必改回 0。 */
#ifndef M2_TEST_RAW
#define M2_TEST_RAW         0
#endif

/* ---------- 调度参数 ---------- */
#define M2_POLL_PERIOD_MS    100    /* 正常轮询周期 */
#define M2_OFFLINE_PERIOD_MS 1000   /* 离线探测周期（降频，别让坏设备拖垮总线） */
#define M2_FAIL_THRESHOLD    3      /* 连续失败几次判定离线 */
#define M2_RESP_TIMEOUT_MS   200    /* 单次事务超时 */
#define M2_TICK_MS           10     /* 调度任务节拍（用 vTaskDelayUntil 严格等距） */

/* ---------- API ---------- */

/* 启动轮询任务（在 freertos.c 的 RTOS_THREADS 里调用）。内部会先 MB_Init()。 */
void M2_MasterStart(void);

/* 数据池互斥访问（M4 起统一转发到 m4_pool 的互斥量） */
void M2_PoolLock(void);
void M2_PoolUnlock(void);

/* 网络侧下发继电器控制指令：置目标 + 置脏位，由轮询任务在下一个节拍捎带下发。
   ⚠️ 不要在这里直接发 Modbus 帧 —— RS485 半双工，抢占会和正在进行的读撞车。 */
void M2_SetRelayTarget(uint8_t bitmap);

/* 取一份带锁的数据池快照（网络任务上报用） */
void M2_GetSnapshot(m2_datapool_t *out);

/* 串口打印一次状态汇总（调试/验收用） */
void M2_DumpStatus(void);

#endif /* __M2_MASTER_H */
