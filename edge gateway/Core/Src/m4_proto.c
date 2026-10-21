/*==============================================================================
 * M4 · 应用层协议实现
 *============================================================================*/

#include "m4_proto.h"
#include "m4_pool.h"
#include "m5_link.h"     /* M5：M5_UpSeqPeek() 全局单调上行序号 */

#include <string.h>
#include "main.h"       /* HAL_GetTick */

/* 状态机状态（放在 .c 里，头文件只暴露 uint8_t，避免外部乱改） */
#define ST_HEAD0    0u
#define ST_HEAD1    1u
#define ST_LEN      2u
#define ST_CMD      3u
#define ST_DATA     4u
#define ST_CRC      5u

/*==============================================================================
 * CRC32（软件实现）
 *
 * ⚠️ 为什么不用 STM32F4 的硬件 CRC 外设：
 *    硬件 CRC 按【32-bit 字 + 大端】输入，且不反转，算出来与"标准 CRC32"
 *    （如 Python zlib.crc32、常规 C 实现）结果相反。自研协议两端一致就行，
 *    但为了让 PC 侧能直接用 zlib.crc32 对拍，这里选择软件实现 —— 跨端一致优先。
 *============================================================================*/
uint32_t M4_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8u; j++)
        {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc = (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/*==============================================================================
 * ⭐ 增量 CRC32（M5 OTA 用）
 *
 * 为什么需要：OTA 收 100 KB 固件，不可能把整包放内存（heap 只有 ~8 KB），
 * 所以"收一块算一块"。这个接口就是那个"边收边算"。
 *
 * 用法：
 *     uint32_t st = 0xFFFFFFFFu;                  // 初值
 *     st = M4_Crc32Update(st, blk, len);           // 每块调一次
 *     uint32_t final = st ^ 0xFFFFFFFFu;           // 收完后异或才是最终值
 *
 * ⚠️ 返回值是【未取反的中间状态】，不要直接拿去和 total_crc 比，
 *    一定要在最后做一次 ^ 0xFFFFFFFF —— 这是最常见的错法。
 *============================================================================*/
uint32_t M4_Crc32Update(uint32_t state, const uint8_t *data, uint32_t len)
{
    uint32_t crc = state;      /* ⚠️ 直接接着上次的中间状态，不重置初值 */
    uint32_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0u; j < 8u; j++)
        {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc = (crc >> 1);
        }
    }
    return crc;                /* ⚠️ 未取反：调用方最后自己 ^ 0xFFFFFFFF */
}

/*==============================================================================
 * 状态机
 *============================================================================*/
void M4_ProtoInit(m4_proto_ctx_t *ctx)
{
    if (ctx == NULL) return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->state = ST_HEAD0;
    ctx->last_ms = HAL_GetTick();
}

int M4_ProtoFeed(m4_proto_ctx_t *ctx, uint8_t b)
{
    uint32_t now;

    if (ctx == NULL) return -2;

    now = HAL_GetTick();

    /* ⭐ 超时复位：如果距上一个字节超过 200ms，说明上一帧是残缺的（对端只发了半帧
       就停了），必须回到 HEAD0，否则后续所有正常数据都会被当成 DATA 吃进去，
       结果是"通信永久失效" —— 而且现象看起来像是板子突然死了，极难排查。 */
    if ((ctx->state != ST_HEAD0) && ((now - ctx->last_ms) > M4_PROTO_TIMEOUT_MS))
    {
        ctx->state = ST_HEAD0;
    }
    ctx->last_ms = now;

    switch (ctx->state)
    {
    case ST_HEAD0:
        if (b == M4_SOF0) ctx->state = ST_HEAD1;
        /* 不是帧头就原地不动 —— 这就是"自动重新同步"的能力 */
        break;

    case ST_HEAD1:
        ctx->state = (b == M4_SOF1) ? ST_LEN : ST_HEAD0;
        break;

    case ST_LEN:
        /* LEN = 1(CMD) + N(DATA)，所以最小是 1 */
        if ((b == 0u) || (b > (M4_PROTO_MAX_DATA + 1u)))
        {
            ctx->state = ST_HEAD0;
            return -2;
        }
        ctx->len      = b;
        ctx->data_len = (uint8_t)(b - 1u);
        ctx->idx      = 0;
        ctx->state    = ST_CMD;
        break;

    case ST_CMD:
        ctx->cmd      = b;
        ctx->crc_calc = M4_Crc32(&b, 1);       /* CRC 从 CMD 开始算 */
        ctx->state    = (ctx->data_len > 0u) ? ST_DATA : ST_CRC;
        ctx->crc_idx  = 0;
        ctx->crc_acc  = 0;
        break;

    case ST_DATA:
        ctx->data[ctx->idx++] = b;
        /* 增量算 CRC：把已收的 CMD+DATA 重新算一遍（帧很短，开销可忽略） */
        {
            uint8_t tmp[M4_PROTO_MAX_DATA + 1u];
            tmp[0] = ctx->cmd;
            memcpy(&tmp[1], ctx->data, ctx->idx);
            ctx->crc_calc = M4_Crc32(tmp, (uint32_t)(ctx->idx + 1u));
        }
        if (ctx->idx >= ctx->data_len)
        {
            ctx->state   = ST_CRC;
            ctx->crc_idx = 0;
            ctx->crc_acc = 0;
        }
        break;

    case ST_CRC:
        /* CRC32 大端先高字节 */
        ctx->crc_acc = (ctx->crc_acc << 8) | b;
        ctx->crc_idx++;

        if (ctx->crc_idx >= 4u)
        {
            int rc;
            ctx->state = ST_HEAD0;

            if (ctx->crc_acc != ctx->crc_calc)
            {
                return -1;      /* CRC 错：丢弃这一帧（已经复位，不影响后续） */
            }
            rc = (int)ctx->cmd;
            return rc;          /* >0：拿到一帧 */
        }
        break;

    default:
        ctx->state = ST_HEAD0;
        break;
    }

    return 0;
}

/*==============================================================================
 * 打包
 *============================================================================*/
int M4_ProtoPack(uint8_t cmd, const uint8_t *data, uint8_t n,
                 uint8_t *out, uint32_t out_max)
{
    uint32_t frame_len;
    uint32_t crc;
    uint8_t  tmp[M4_PROTO_MAX_DATA + 1u];

    if (n > M4_PROTO_MAX_DATA) return -1;

    frame_len = 3u + 1u + (uint32_t)n + 4u;    /* 头2 + LEN1 + CMD1 + DATA n + CRC4 */
    if ((out == NULL) || (out_max < frame_len)) return -1;

    out[0] = M4_SOF0;
    out[1] = M4_SOF1;
    out[2] = (uint8_t)(n + 1u);                /* LEN = CMD + DATA */
    out[3] = cmd;
    if (n > 0u) memcpy(&out[4], data, n);

    /* CRC 覆盖 CMD + DATA */
    tmp[0] = cmd;
    if (n > 0u) memcpy(&tmp[1], data, n);
    crc = M4_Crc32(tmp, (uint32_t)(n + 1u));

    out[4u + n]      = (uint8_t)((crc >> 24) & 0xFFu);
    out[4u + n + 1u] = (uint8_t)((crc >> 16) & 0xFFu);
    out[4u + n + 2u] = (uint8_t)((crc >> 8)  & 0xFFu);
    out[4u + n + 3u] = (uint8_t)(crc & 0xFFu);

    return (int)frame_len;
}

/*==============================================================================
 * 业务帧：数据池快照
 *
 * DATA 布局（大端，共 35 字节）：
 *   0..1   温度 int16 ×10
 *   2..3   湿度 int16 ×10
 *   4      DI 位图
 *   5      继电器实际状态
 *   6      继电器目标
 *   7      dirty(bit0) | link(bit1) | slave_online0(bit2) | slave_online1(bit3)
 *          | can_online0(bit4) | can_online1(bit5)
 *   8..11  poll_total
 *   12..15 poll_fail
 *   16..19 CAN1 rx
 *   20..23 CAN2 rx
 *   24..25 CAN1 drop
 *   26..27 CAN2 drop
 *   28..29 数据年龄（温度，秒；0xFFFF = 从未更新）
 *   30..33 uptime(秒)
 *   34     cmd_bad 低 8 位
 *
 * ⭐ 第 28..29 字节的"数据年龄"是这份协议里最有价值的一个字段：
 *    它让上位机能区分"温度 25.3℃"和"3 分钟前的 25.3℃"。
 *============================================================================*/
/* ⭐ 35 → 39：M5 在末尾加了 4 字节的【上行序号 seq】（偏移 35~38，大端）。
   为什么必须带 seq：链路主备切换时同一条数据可能被两条链路各发一次，
   上位机按 (device, seq) 去重；没有 seq 就没法区分"重复"和"新数据"。
   ⚠️ 改了这里必须同步改 tools/m4_host_test.py 的 decode_snapshot()（len 35→39）。 */
#define M4_SNAP_DATA_LEN   39u

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8)  & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

int M4_ProtoPackSnapshot(uint8_t *out, uint32_t out_max)
{
    /* 走 Ex 版本：seq 用全局单调序号的 peek 值（递增由 M5_UpSeqCommit 在成功后做） */
    return M4_ProtoPackSnapshotEx(out, out_max, M5_UpSeqPeek(),
                                  INT16_MIN, INT16_MIN, 0xFFu, 0xFFu, 0u, 0u);
}

int M4_ProtoPackSnapshotEx(uint8_t *out, uint32_t out_max,
                           uint32_t seq,
                           int16_t  hist_temp,
                           int16_t  hist_humi,
                           uint8_t  hist_di,
                           uint8_t  hist_relay,
                           uint32_t hist_uptime_s,
                           uint32_t age_s_override)
{
    m4_pool_t  snap;
    uint8_t    d[M4_SNAP_DATA_LEN];
    uint8_t    flags;
    uint16_t   age_s;

    /* ⭐ 快照语义：加锁只为了这一次 memcpy，之后慢慢打包，绝不持锁做慢操作 */
    M4_PoolGetSnapshot(&snap);

    memset(d, 0, sizeof(d));

    /* ⭐ M6：补传模式判定 —— 温湿度传 INT16_MIN 表示"用当前数据池的值"。
       ⚠️ 为什么用 INT16_MIN 当哨兵而不是加个 has_hist 布尔参数：
          加参数会让调用方多写一个字段，容易漏；用哨兵值则默认路径（正常上报）
          完全不用关心这个参数的存在。
          ⚠️ 但要注意 INT16_MIN(-32768) 本身不是合法温度（合法范围 -400~800，即 -40.0~80.0℃），
             所以拿它当哨兵是安全的。 */
    if (hist_temp == INT16_MIN)
    {
        put_be16(&d[0], (uint16_t)snap.temperature);
        put_be16(&d[2], (uint16_t)snap.humidity);
        d[4] = snap.di_status;
        d[5] = snap.relay_status;
    }
    else
    {
        /* ⭐ 补传历史条目：温湿度/DI/继电器用【缓存里的旧值】，
           其余统计类字段（poll_total / can_rx / 链路标志）仍用当前值 ——
           因为那些是"设备当前状态"，补传的是"当时的采集值"，两者语义不同，
           没必要也不应该把统计值穿越回过去。 */
        put_be16(&d[0], (uint16_t)hist_temp);
        put_be16(&d[2], (uint16_t)hist_humi);
        d[4] = hist_di;
        d[5] = hist_relay;
    }
    d[6] = snap.relay_target;

    flags = 0u;
    if (snap.relay_dirty)          flags |= (1u << 0);
    if (snap.eth_link_up)          flags |= (1u << 1);
    if (snap.slave_online[0])      flags |= (1u << 2);
    if (snap.slave_online[1])      flags |= (1u << 3);
    if (snap.can_online[0])        flags |= (1u << 4);
    if (snap.can_online[1])        flags |= (1u << 5);
    d[7] = flags;

    put_be32(&d[8],  snap.poll_total);
    put_be32(&d[12], snap.poll_fail);
    put_be32(&d[16], snap.can_rx[0]);
    put_be32(&d[20], snap.can_rx[1]);
    put_be16(&d[24], (uint16_t)snap.can_drop[0]);
    put_be16(&d[26], (uint16_t)snap.can_drop[1]);

    /* 数据年龄。补传时由调用方直接给出（缓存条目存的是 uptime_s，
       算不出精确的 ms 级年龄，给个近似值即可，语义上"这条是旧的"表达到位就行） */
    if (hist_temp != INT16_MIN)
    {
        age_s = (age_s_override > 0xFFFEu) ? 0xFFFFu : (uint16_t)age_s_override;
    }
    else if (snap.ts_temp == 0u)
    {
        age_s = 0xFFFFu;     /* 从未采集过 → 无效 */
    }
    else
    {
        uint32_t age_s32 = (HAL_GetTick() - snap.ts_temp) / 1000u;
        age_s = (age_s32 > 0xFFFEu) ? 0xFFFFu : (uint16_t)age_s32;
    }

    put_be16(&d[28], age_s);

    /* ⭐ uptime：补传用缓存里记录的采集时刻，正常上报用当前值 */
    put_be32(&d[30], (hist_temp == INT16_MIN) ? snap.uptime_s : hist_uptime_s);
    d[34] = (uint8_t)(snap.cmd_bad & 0xFFu);

    /* ⭐ 序号：正常上报 = 全局 peek 值；补传 = 【缓存条目原本的 seq】。
       ⚠️ 为什么补传必须用原 seq 而不是新 seq：
          上位机按 (device, seq) 去重。如果补传的数据用新 seq，
          上位机会把它当成"新数据"插进时序里，于是时序图出现
          "现在突然冒出一条 20 分钟前的温度" —— 数据看起来像跳变。
          沿用原 seq 才能让它回到时间线上正确的位置。 */
    put_be32(&d[35], seq);

    return M4_ProtoPack(M4_CMD_SNAPSHOT, d, (uint8_t)sizeof(d), out, out_max);
}

int M4_ProtoPackAck(uint8_t *out, uint32_t out_max)
{
    uint8_t d = 0u;
    return M4_ProtoPack(M4_CMD_ACK, &d, 1u, out, out_max);
}

int M4_ProtoPackNak(uint8_t err, uint8_t *out, uint32_t out_max)
{
    uint8_t d[1];
    d[0] = err;
    return M4_ProtoPack(M4_CMD_NAK, d, 1u, out, out_max);
}

int M4_ProtoParseSetRelay(const m4_proto_ctx_t *ctx, uint8_t *bitmap)
{
    if ((ctx == NULL) || (bitmap == NULL)) return -1;
    if (ctx->data_len < 1u) return -1;

    *bitmap = ctx->data[0];
    return 0;
}

/*==============================================================================
 * M5 · OTA 应答帧
 *   DATA = status(1) + offset(4) + extra(4)
 *   offset 给上位机推进度条 / 定位续传起点；extra 用来带附加信息（如期望 offset）
 *============================================================================*/
int M4_ProtoPackOtaAck(uint8_t status, uint32_t offset, uint32_t extra,
                       uint8_t *out, uint32_t out_max)
{
    uint8_t d[9];

    d[0] = status;
    put_be32(&d[1], offset);
    put_be32(&d[5], extra);

    return M4_ProtoPack(M4_CMD_OTA_ACK, d, (uint8_t)sizeof(d), out, out_max);
}
