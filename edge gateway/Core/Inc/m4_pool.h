/*==============================================================================
 * M4 · 统一数据池
 *
 * 这是整个网关的"唯一真相来源（single source of truth）"。
 *
 * 分区规则（严格遵守，否则竞态一定会回来）：
 *   采集区 —— 只有采集任务写（mbPoll 写 Modbus 区，m3_can 写 CAN 区）
 *   控制区 —— 只有网络任务写（上位机指令）
 *   状态区 —— 由 m4_task 的监控任务写（链路、uptime）
 *   时间戳 —— 与采集值同一次加锁内写入，用于算"数据年龄"
 *
 * 访问规则：
 *   ① 跨任务访问一律走本文件的 API，不要直接摸 g_pool
 *   ② 要"读多个字段"必须用 M4_PoolGetSnapshot() 拿快照，
 *      不要一个字段一个字段地加锁（会读到"温度新、湿度旧"的撕裂数据）
 *   ③ 锁内不做慢操作（不 printf、不发网络帧、不发 Modbus）
 *============================================================================*/

#ifndef __M4_POOL_H
#define __M4_POOL_H

#include <stdint.h>

#define M4_SLAVE_NUM       2u      /* Modbus 从机数量（与 MB_SLAVE_NUM 一致） */
#define M4_CAN_PORT_NUM    2u      /* CAN1 / CAN2 */

/*==============================================================================
 * 数据池结构
 *============================================================================*/
typedef struct {
    /* ---------------- 采集区：Modbus（mbPoll 写） ---------------- */
    int16_t   temperature;      /* 温度 ×10，253 = 25.3℃ */
    int16_t   humidity;         /* 湿度 ×10 */
    uint8_t   di_status;        /* 8 路 DI 位图，bit0 = DI0 */
    uint8_t   relay_status;     /* 继电器实际状态（回读确认过的） */

    /* ---------------- 采集区：CAN（m3_can 写） ---------------- */
    uint32_t  can_rx[M4_CAN_PORT_NUM];      /* 各端口累计收帧 */
    uint32_t  can_drop[M4_CAN_PORT_NUM];    /* 各端口累计丢帧 */
    uint8_t   can_busoff[M4_CAN_PORT_NUM];  /* 各端口 bus-off 次数 */
    uint8_t   can_online[M4_CAN_PORT_NUM];  /* 1 = 该端口在最近 5s 收到过帧 */

    /* ---------------- 控制区（网络任务写） ---------------- */
    uint8_t   relay_target;     /* 继电器目标位图 */
    uint8_t   relay_dirty;      /* 1 = 有待下发（由轮询任务捎带） */

    /* ---------------- 状态区 ---------------- */
    uint8_t   slave_online[M4_SLAVE_NUM];
    uint32_t  poll_total;       /* ⭐ Modbus 事务总数 */
    uint32_t  poll_fail;        /* ⭐ 失败数 —— 简历上的"成功率 99.x%"就是它算的 */
    uint8_t   eth_link_up;      /* PHY 链路（监控任务写） */
    uint32_t  uptime_s;         /* 运行秒数（监控任务写） */
    uint32_t  cmd_total;        /* 收到并解析的指令总数 */
    uint32_t  cmd_bad;          /* 校验失败/非法指令数 */

    /* ---------------- 时间戳（ms，HAL_GetTick 域） ----------------
       ⭐ 为什么必须有：从机离线时，temperature 会保持"最后一次的值"，
          上位机看到 25.3℃ 会以为一切正常。有时间戳才能算出"这个值是 3 分钟前的"，
          从而把数据显示成灰色/失效。工业现场这种"假正常"是最危险的。 */
    uint32_t  ts_temp;
    uint32_t  ts_di;
    uint32_t  ts_relay;
    uint32_t  ts_can;
} m4_pool_t;

extern m4_pool_t g_pool;

/*==============================================================================
 * 生命周期与锁
 *============================================================================*/
void M4_PoolInit(void);
void M4_PoolLock(void);
void M4_PoolUnlock(void);

/* 快照：一次 memcpy 拷走整块，调用方之后随便用（不再持有锁） */
void M4_PoolGetSnapshot(m4_pool_t *out);

/*==============================================================================
 * 采集侧写入（内部自带锁，且会更新时间戳）
 *============================================================================*/
void M4_PoolSetTempHumi(int16_t t, int16_t h);
void M4_PoolSetDI(uint8_t bitmap);
void M4_PoolSetRelayActual(uint8_t bitmap);
void M4_PoolSetSlaveOnline(uint8_t idx, uint8_t online);
void M4_PoolAddPoll(uint8_t ok);            /* ok=1 成功，0 失败 */

/* CAN 区：由 m3_can 周期性调用一次 */
void M4_PoolSetCanStats(uint8_t port, uint32_t rx, uint32_t drop,
                        uint8_t busoff, uint8_t online);

/*==============================================================================
 * 控制侧（网络任务写，轮询任务读）
 *============================================================================*/
/* 置继电器目标并置脏位。⚠️ 只写数据池，绝不在这里发 Modbus 帧 —— 见 m2_master.c 说明 */
void M4_PoolSetRelayTarget(uint8_t bitmap);

/* 轮询任务用：取目标 + 取脏位（一次加锁） */
uint8_t M4_PoolGetRelayTarget(uint8_t *dirty);

/* 回读一致后清脏位 */
void M4_PoolClearRelayDirty(void);

/* 指令统计 */
void M4_PoolAddCmd(uint8_t bad);

/*==============================================================================
 * 状态区（监控任务写）
 *============================================================================*/
void M4_PoolSetEthLink(uint8_t up);
void M4_PoolSetUptime(uint32_t sec);

/*==============================================================================
 * 辅助：判断某个采集量是否"过期"
 *============================================================================*/
/* 返回 1 = 数据已过期（超过 max_age_ms 没更新） */
int M4_PoolIsStale(uint32_t ts, uint32_t max_age_ms);

#endif /* __M4_POOL_H */
