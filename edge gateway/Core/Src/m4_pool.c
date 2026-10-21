/*==============================================================================
 * M4 · 统一数据池实现
 *
 * 锁的选择：互斥量（不是二值信号量、也不是关中断）
 *   - 数据池只被任务访问（ISR 不碰），所以不需要关中断
 *   - 用互斥量而不是二值信号量，是为了拿到【优先级继承】：
 *     低优先级的 mbPoll 持锁时若被高优先级的 task_net 等待，
 *     内核会临时把 mbPoll 提到 task_net 的优先级，避免优先级反转
 *     （configUSE_MUTEXES = 1 已开）
 *============================================================================*/

#include "m4_pool.h"

#include "cmsis_os.h"
#include "main.h"       /* HAL_GetTick */

m4_pool_t g_pool;

static osMutexId_t s_mutex = NULL;

/*==============================================================================
 * 初始化与锁
 *============================================================================*/
void M4_PoolInit(void)
{
    if (s_mutex == NULL)
    {
        s_mutex = osMutexNew(NULL);
    }
}

void M4_PoolLock(void)
{
    if (s_mutex != NULL)
    {
        osMutexAcquire(s_mutex, osWaitForever);
    }
}

void M4_PoolUnlock(void)
{
    if (s_mutex != NULL)
    {
        osMutexRelease(s_mutex);
    }
}

void M4_PoolGetSnapshot(m4_pool_t *out)
{
    if (out == NULL) return;

    M4_PoolLock();
    *out = g_pool;          /* 整块拷贝：锁内只有一次 memcpy，几十微秒 */
    M4_PoolUnlock();
}

/*==============================================================================
 * 采集侧写入
 *============================================================================*/
void M4_PoolSetTempHumi(int16_t t, int16_t h)
{
    uint32_t now = HAL_GetTick();

    M4_PoolLock();
    g_pool.temperature = t;
    g_pool.humidity    = h;
    g_pool.ts_temp     = now;      /* ⭐ 值和时间戳必须在同一把锁内写 */
    M4_PoolUnlock();
}

void M4_PoolSetDI(uint8_t bitmap)
{
    uint32_t now = HAL_GetTick();

    M4_PoolLock();
    g_pool.di_status = bitmap;
    g_pool.ts_di     = now;
    M4_PoolUnlock();
}

void M4_PoolSetRelayActual(uint8_t bitmap)
{
    uint32_t now = HAL_GetTick();

    M4_PoolLock();
    g_pool.relay_status = bitmap;
    g_pool.ts_relay     = now;
    M4_PoolUnlock();
}

void M4_PoolSetSlaveOnline(uint8_t idx, uint8_t online)
{
    if (idx >= M4_SLAVE_NUM) return;

    M4_PoolLock();
    g_pool.slave_online[idx] = online ? 1u : 0u;
    M4_PoolUnlock();
}

void M4_PoolAddPoll(uint8_t ok)
{
    M4_PoolLock();
    g_pool.poll_total++;
    if (!ok) g_pool.poll_fail++;
    M4_PoolUnlock();
}

void M4_PoolSetCanStats(uint8_t port, uint32_t rx, uint32_t drop,
                        uint8_t busoff, uint8_t online)
{
    if (port >= M4_CAN_PORT_NUM) return;

    M4_PoolLock();
    g_pool.can_rx[port]     = rx;
    g_pool.can_drop[port]   = drop;
    g_pool.can_busoff[port] = busoff;
    g_pool.can_online[port] = online ? 1u : 0u;
    g_pool.ts_can           = HAL_GetTick();
    M4_PoolUnlock();
}

/*==============================================================================
 * 控制侧
 *============================================================================*/
void M4_PoolSetRelayTarget(uint8_t bitmap)
{
    M4_PoolLock();
    if (g_pool.relay_target != bitmap)
    {
        g_pool.relay_dirty = 1u;    /* 目标变了才置脏位，避免无意义的重复下发 */
    }
    g_pool.relay_target = bitmap;
    M4_PoolUnlock();
}

uint8_t M4_PoolGetRelayTarget(uint8_t *dirty)
{
    uint8_t t;

    M4_PoolLock();
    t = g_pool.relay_target;
    if (dirty != NULL) *dirty = g_pool.relay_dirty;
    M4_PoolUnlock();

    return t;
}

void M4_PoolClearRelayDirty(void)
{
    M4_PoolLock();
    g_pool.relay_dirty = 0u;
    M4_PoolUnlock();
}

void M4_PoolAddCmd(uint8_t bad)
{
    M4_PoolLock();
    g_pool.cmd_total++;
    if (bad) g_pool.cmd_bad++;
    M4_PoolUnlock();
}

/*==============================================================================
 * 状态区
 *============================================================================*/
void M4_PoolSetEthLink(uint8_t up)
{
    M4_PoolLock();
    g_pool.eth_link_up = up ? 1u : 0u;
    M4_PoolUnlock();
}

void M4_PoolSetUptime(uint32_t sec)
{
    M4_PoolLock();
    g_pool.uptime_s = sec;
    M4_PoolUnlock();
}

/*==============================================================================
 * 数据年龄判断
 *
 * ⚠️ 用减法比较，天然免疫 32 位 tick 溢出（约 49 天回绕一次）。
 *    如果写成 "now > ts + max_age"，回绕那一刻会误判成"没过期"。
 *============================================================================*/
int M4_PoolIsStale(uint32_t ts, uint32_t max_age_ms)
{
    return ((HAL_GetTick() - ts) > max_age_ms) ? 1 : 0;
}
