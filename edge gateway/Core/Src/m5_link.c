/*==============================================================================
 * M5 · 链路冗余抽象层实现（三态 + 滞回 + 全局单调 seq）
 *============================================================================*/

#include "m5_link.h"
#include "main.h"      /* HAL_GetTick */
#include "m5_esp.h"
#include "lwip/netif.h"       /* netif_is_link_up */

#include <stdio.h>

/*==============================================================================
 * 内部状态
 *============================================================================*/
static m5_link_id_t s_cur      = M5_LINK_PRIMARY;
static uint8_t      s_fail_run;      /* 主链路连续失败次数 */
static uint8_t      s_ok_run;        /* 主链路连续成功次数 */
static uint32_t     s_switch_cnt;    /* 切换次数（诊断） */
static uint32_t     s_lost_cnt;      /* DEGRADED 期间丢掉的条数 */

static uint32_t     s_up_seq;        /* ⭐ 全局单调序号，永不重置 */

static int (*s_eth_send)(const uint8_t *data, uint16_t len) = NULL;

/* 主链路物理层是否 up（PHY 链路状态）——只是"物理通"，不等于"能发出去" */
static int eth_phy_up(void)
{
    extern struct netif gnetif;
    return netif_is_link_up(&gnetif) ? 1 : 0;
}

/*==============================================================================
 * 初始化 / 注册
 *============================================================================*/
void M5_LinkInit(void)
{
    s_cur        = M5_LINK_PRIMARY;
    s_fail_run   = 0u;
    s_ok_run     = 0u;
    s_switch_cnt = 0u;
    s_lost_cnt   = 0u;
    s_up_seq     = 0u;
    s_eth_send   = NULL;

    M5_EspInit();
}

void M5_LinkRegisterEthSend(int (*send)(const uint8_t *data, uint16_t len))
{
    s_eth_send = send;
}

m5_link_id_t M5_LinkCurrent(void)
{
    return s_cur;
}

uint32_t M5_LinkReportInterval(void)
{
    /* ⭐ 降级策略：备份链路慢，周期拉长 10 倍 */
    return (s_cur == M5_LINK_BACKUP) ? M5_REPORT_MS_BACKUP : M5_REPORT_MS_PRIMARY;
}

uint32_t M5_LinkSwitchCount(void) { return s_switch_cnt; }
uint32_t M5_LinkLostCount(void)   { return s_lost_cnt;   }

/*==============================================================================
 * 发送：按当前链路选实现
 *============================================================================*/
int M5_LinkSend(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0u) return -1;

    if (s_cur == M5_LINK_BACKUP)
    {
        /* 备份链路：ESP8266 AT+CIPSEND */
        return M5_EspSend(data, len);
    }

    /* 主链路：以太网。⚠️ 注册回调为 NULL（还没连上）时直接算失败，
       让滞回计数器把它记进去 —— 这正是"9 s 内检测出主链路故障"的实现。 */
    if (s_eth_send == NULL) return -1;

    return s_eth_send(data, len);
}

/*==============================================================================
 * 结果上报 → 滞回计数 → 状态迁移
 *============================================================================*/
void M5_LinkReportResult(int ok)
{
    if (s_cur == M5_LINK_BACKUP)
    {
        /* 在备份链路上：只统计"主链路是否恢复"（主链路此时不发数据，
           所以用 PHY 状态 + 一次探测发送来判断）。 */
        if (eth_phy_up())
        {
            s_ok_run++;
            if (s_ok_run >= M5_LINK_OK_M)
            {
                printf("[link] BACKUP -> PRIMARY (eth back, ok=%u)\r\n",
                       (unsigned)s_ok_run);
                s_cur      = M5_LINK_PRIMARY;
                s_ok_run   = 0u;
                s_fail_run = 0u;
                s_switch_cnt++;
                M5_EspDisconnect();
            }
        }
        else
        {
            s_ok_run = 0u;
        }
        return;
    }

    /* ---------- 当前在主链路 ---------- */
    if (ok)
    {
        s_fail_run = 0u;
        return;
    }

    s_fail_run++;
    if (s_fail_run >= M5_LINK_FAIL_N)
    {
        /* 主链路判定失败 → 切备份。
           注意：切换的【瞬间】并没有真的验证备份链路可用，
           是否真能用由后续 M5_EspSend() 的返回值决定；
           若备份也发不出去 → 累计失败后进 DEGRADED。 */
        printf("[link] PRIMARY -> BACKUP (fail=%u)\r\n", (unsigned)s_fail_run);
        s_cur      = M5_LINK_BACKUP;
        s_fail_run = 0u;
        s_ok_run   = 0u;
        s_switch_cnt++;
    }
}

/*==============================================================================
 * 周期驱动：跑 ESP 状态机 + 检查 DEGRADED
 *============================================================================*/
void M5_LinkTick(uint32_t period_ms)
{
    /* 只要【不是】纯主链路状态，就要让 ESP 有机会建链/保活。
       ⚠️ 为什么主链路正常时也偶尔 tick：这样备份链路一直保持"已配好"，
          真切换时能立刻用，而不是从 AT 指令开始慢慢连（那要几秒）。 */
    M5_EspTick(period_ms);

    /* ⭐ 启动宽限：只驱动 ESP，不做任何切换/告警决策（见 m5_link.h 的说明） */
    if (HAL_GetTick() < M5_LINK_GRACE_MS)
    {
        return;
    }

    /* ---------- DEGRADED 判定 ---------- */
    if (s_cur == M5_LINK_BACKUP)
    {
        if (!M5_EspIsUp() && !eth_phy_up())
        {
            /* 两条都不可用 */
            s_cur = M5_LINK_DEGRADED;
            printf("[link] !! DEGRADED: both links down\r\n");
        }
    }
    else if (s_cur == M5_LINK_DEGRADED)
    {
        if (eth_phy_up())
        {
            s_cur      = M5_LINK_PRIMARY;
            s_fail_run = 0u;
            printf("[link] DEGRADED -> PRIMARY (eth restored)\r\n");
            s_switch_cnt++;
        }
        else if (M5_EspIsUp())
        {
            s_cur    = M5_LINK_BACKUP;
            s_ok_run = 0u;
            printf("[link] DEGRADED -> BACKUP (esp restored)\r\n");
            s_switch_cnt++;
        }
    }
    else
    {
        /* 主链路：PHY 掉了要立刻开始累计失败（不等发送超时，9s 太长） */
        if (!eth_phy_up())
        {
            M5_LinkReportResult(0);
        }
    }
}

/*==============================================================================
 * ⭐ 全局单调序号
 *============================================================================*/
uint32_t M5_UpSeqPeek(void)
{
    return s_up_seq;
}

void M5_UpSeqCommit(int ok)
{
    if (ok)
    {
        s_up_seq++;      /* ⭐ 只有成功才递增：失败用同一个号重发 → 上位机去重 */
    }
    else
    {
        /* DEGRADED 状态下这条数据没人接 —— 记一笔丢失（M6 补 W25Q128 缓存） */
        if (s_cur == M5_LINK_DEGRADED)
        {
            s_lost_cnt++;
        }
    }
}
